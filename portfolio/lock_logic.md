# 락 구조: 목적과 락 안의 로직

**서버의 락은 다섯 가지 목적으로 나뉘고, 소켓 송신(`WSASend`)은 어느 락 안에서도 부르지 않는다.** 메시지 처리 경로에서 잡는 락은 세션별 `mSendLock`과 방별 `Room.Lock`뿐이고, 나머지는 연결·해제, 시작·종료, 통계, 오류 때만 잡는다.

- 기준: 2026-11-06 코드(`tbDeferSend` 제거 후)
- 대상: `_lib/NetworkLib/NetworkLib.cpp`, `networkLibProject/ChatServer.h`
- 제외: `ProfilerLib`의 내부 락(측정 도구)

## 한눈에 보기

| 목적             | 락               | 개수     | 메시지 경로 |
| ---------------- | ---------------- | -------- | ----------- |
| 세션 송신 상태   | `mSendLock`      | 세션마다 | 예          |
| 방 구성원과 순서 | `Room.Lock`      | 방마다   | 예          |
| 빈 세션 슬롯     | `mFreeLock`      | 서버 1개 | 아니요      |
| 시작·종료 직렬화 | `mStartStopLock` | 서버 1개 | 아니요      |
| 통계 목록        | `mCounterLock`   | 서버 1개 | 아니요      |
| 오류 기록        | `mNetErrorLock`  | 서버 1개 | 아니요      |

## 1. 세션 송신 상태: `mSendLock`

**송신 링, `mbSendFlag`, `mWsaSendQPC`를 하나의 상태로 유지한다.**

| 함수                | 락 안의 로직                                    |
| ------------------- | ----------------------------------------------- |
| `marshalToSendBuf`  | 빈 공간 확인, 헤더·페이로드 넣기                |
| `postSend`          | 송신 중 확인, 범위 읽기, 플래그 켜기, 시각 기록 |
| `completeSend`      | 보낸 만큼 빼기, 플래그 끄기, 시각 읽기          |
| `runSendStallWatch` | 플래그와 시각을 읽어 정체 판정                  |

함께 묶은 이유는 다음과 같다.

- `marshalToSendBuf`: 헤더와 페이로드 중 하나만 들어가면 스트림이 깨진다.
- `postSend`: 링 확인과 플래그 변경이 나뉘면 데이터가 링에 남은 채 아무도 보내지 않을 수 있다.
- `postSend`: 시각을 락 밖에서 쓰면 감시 스레드가 "송신 중" 플래그와 이전 송신의 시각을 함께 읽어 정상 클라를 정체로 오판할 수 있다.
- `completeSend`: 플래그를 내리면 다른 워커가 새 송신을 걸며 시각을 덮어쓸 수 있어 락 안에서 읽는다.

락 밖에서 하는 일은 다음과 같다.

- `WSASend`와 그 결과 처리
- 넘침 끊기(`requestDisconnect`, `CancelIoEx`)
- 워커별 카운터 갱신

`postSend`가 락 안에서 읽은 버퍼 위치를 락 밖에서 써도 되는 이유는 두 가지다. `mbSendFlag`가 켜져 있는 동안 같은 세션에 다른 `WSASend`를 걸지 않고, 그 구간의 데이터는 송신이 끝난 뒤 `completeSend`에서만 뺀다.

## 2. 방 구성원과 메시지 순서: `Room.Lock`

**구성원 목록과 방 순번을 보호하고, 구성원 모두가 같은 순서로 메시지를 받게 한다.** 락 범위가 링에 넣기까지 이어지는 이유가 순서 보장이다.

| 함수        | 락 안의 로직                                  |
| ----------- | --------------------------------------------- |
| `broadcast` | 방 순번 증가, 메시지에 기록, 전원의 링에 넣기 |
| `join`      | 구성원 추가, `JoinOk`를 링에 넣기             |
| `leave`     | 구성원 제거, 요청 퇴장이면 `LeaveOk` 넣기     |

함께 묶은 이유는 다음과 같다.

- `broadcast`: 락 밖에서 링에 넣으면 워커 둘의 송신이 섞여 받는 사람마다 순서가 달라진다.
- `join`: `JoinOk`가 이 방의 어떤 `Chat`보다 앞서야 한다.
- `leave`: 이전 방의 `Chat`이 모두 `LeaveOk`보다 앞서야 한다.

`broadcast`의 흐름에서 락 범위는 다음과 같다.

```text
페이로드를 지역 버퍼로 복사
[Room.Lock 잡기]
  방 순번 +1, 메시지에 기록
  구성원마다 SendPacket
    findSession으로 세션 확인
    [mSendLock] 링에 넣기 [해제]
    보낼 목록에 올리기
    decIoCount
[Room.Lock 풀기]
통계 갱신
...
flushSends에서 받는 사람마다 WSASend (락 밖)
```

## 3. 빈 세션 슬롯: `mFreeLock`

**빈 슬롯 스택을 여러 워커가 안전하게 쓰게 한다.**

| 함수           | 락 안의 로직                      |
| -------------- | --------------------------------- |
| `allocSession` | 슬롯 꺼내기, 세대 올리기, ID 기록 |
| `freeSession`  | 슬롯 번호 돌려주기                |
| `Stop`         | 모든 슬롯이 돌아왔는지 확인       |

`allocSession`이 꺼낸 시점의 I/O 카운트는 0이라 `tryIncIoCount`가 실패한다. 그래서 ID를 쓰는 동안 그 값을 읽는 다른 스레드는 없다.

## 4. 시작·종료 직렬화: `mStartStopLock`

**`Start`와 `Stop`이 겹치거나 중복 실행되지 않게 한다.** 두 함수 전체를 감싼다.

| 함수    | 락 안의 로직                                               |
| ------- | ---------------------------------------------------------- |
| `Start` | 설정 복사, 세션 풀, IOCP·워커, 리슨, AcceptEx, 감시 스레드 |
| `Stop`  | 감시 중단, 리슨 닫기, 세션 끊기, 해제 대기, 워커 종료      |

`Stop`은 `Sleep(10)` 반복과 워커 종료 대기까지 락을 쥔 채 진행한다. 그래서 워커나 콜백 안에서 `Stop`을 부르면 자기 자신을 기다리게 되어 데드락이 난다.

## 5. 통계와 오류

| 락              | 함수           | 락 안의 로직                 |
| --------------- | -------------- | ---------------------------- |
| `mCounterLock`  | `getMyCounter` | 새 워커 카운터를 목록에 추가 |
| `mCounterLock`  | `GetChatStat`  | 목록을 돌며 카운터 합산      |
| `mNetErrorLock` | `Start`        | 이전 실행의 오류 지우기      |
| `mNetErrorLock` | `setNetError`  | 첫 오류의 단계와 코드 기록   |
| `mNetErrorLock` | `GetNetError`  | 단계와 코드 복사             |

- `mCounterLock`은 목록 구조만 보호한다. 카운터 값은 쓰는 스레드가 하나라 락 없이 올린다.
- `mNetErrorLock`은 단계와 코드를 한 쌍으로 유지한다.

## 6. 락 순서

**락이 겹치는 경로는 `Room.Lock` → `mSendLock` 하나다.** 방 락 안의 `SendPacket`에서 생기고, 반대 순서로 잡는 곳은 없다.

`OnRelease`는 방 락을 잡으므로 방 락 안에서 다시 불리면 데드락이 난다. 이를 막으려고 `decIoCount`는 카운트가 0이 되면 해제를 직접 하지 않고 `PostQueuedCompletionStatus`로 워커에 넘긴다.

## 7. 락 없이 처리하는 상태

| 상태            | 방식                             |
| --------------- | -------------------------------- |
| `mIoCount`      | Interlocked 함수                 |
| `mbDisconnect`  | Interlocked 쓰기, 일반 읽기      |
| `mbSendTarget`  | Interlocked 교환                 |
| `tSendTargets`  | 스레드 전용(`thread_local`)      |
| `WorkerCounter` | 워커별 단일 쓰기                 |
| `mRoomOf`       | 같은 세션 콜백은 동시 실행 안 됨 |
| `SlotWatch`     | `std::atomic`                    |

`releaseSession`의 `mSendBuf`·`mRecvBuf`·`mbSendFlag` 초기화도 락 없이 한다. I/O 카운트가 0이고 슬롯을 돌려주기 전이라 그 세션을 쓰는 스레드가 없기 때문이다.

## 8. 알아 둘 점

- **방 락 안의 시스템 콜:** 링이 넘치면 `requestDisconnect`의 `CancelIoEx`가 방 락 안에서 불린다. 카운트가 0이 되면 `PostQueuedCompletionStatus`도 방 락 안에서 불린다.
- **해제 작업 전달 실패:** `PostQueuedCompletionStatus`가 실패하면 `decIoCount`는 `crashOnIocpFailure`로 `GetLastError`를 `stderr`에 남기고 `__debugbreak`로 프로세스를 멈춘다. 해제를 맡길 곳이 없어 복구하지 않는다. 예전에는 그 자리에서 해제해 방 락을 다시 잡는 데드락 경로가 있었다(9-6장, 9-7장).

## 9. 실험 계획: 방 락 안의 `OnRelease` 재진입

**"재진입이 실제로 일어나는가"와 "일어나면 어떤 증상인가"를 확인한다.** 결과는 9-6장이다(README_ERRORS 2-11). 전달 실패를 강제로 만드는 시험이라, 실제 운영에서 얼마나 자주 일어나는지는 답하지 못한다.

### 9-1. 예상 경로

```text
A 세션의 broadcast
[Room.Lock 잡기]
  B 세션에 SendPacket
    findSession으로 B의 카운트 +1
    링이 넘쳐 requestDisconnect (B의 수신 취소)
      다른 워커가 취소 완료를 처리해 카운트 -1
    decIoCount로 B의 카운트가 0
      PostQueuedCompletionStatus 실패
      releaseSession → OnRelease(B) → leave
        같은 Room.Lock을 다시 잡기  <-- 여기
```

같은 스레드에서 `std::mutex`를 다시 잠그는 것은 정의되지 않은 동작이다. 그래서 증상이 멈춤이 아니라 예외나 비정상 종료일 수도 있다.

### 9-2. 준비

저장소를 건드리지 않도록 임시 작업 트리에서 빌드한다. **시험 코드는 커밋하지 않는다.**

```powershell
git pull
git worktree add --detach ..\_Arkchimy_deadlock HEAD
cd ..\_Arkchimy_deadlock
```

`networkLibProject/server.cfg`는 작업 트리 안에서 `chatRoomCnt = 16`으로 바꾸고, 시험할 때 `workerCnt`를 2와 4로 바꾼다.

### 9-3. 시험 코드

**① 재진입 표시** (`networkLibProject/ChatServer.h`): 모든 빌드에 넣는다.

```cpp
static thread_local bool tbInBroadcastLock = false; // 파일 위쪽

// broadcast: 구성원에게 보내는 for 문을 감싼다
tbInBroadcastLock = true;
for (const uint64_t member : room.Members) { ... }
tbInBroadcastLock = false;

// OnRelease 맨 앞
if (tbInBroadcastLock)
{
	printf("[deadlock-test] 방 락 안에서 OnRelease 재진입\n");
}
```

**② 전달 실패 강제** (`_lib/NetworkLib/NetworkLib.cpp`, `decIoCount`): 실패 강제 빌드에만 넣는다.

```cpp
// 변경 전: if (!PostQueuedCompletionStatus(mIOCP, 0, ..., &session.mReleaseOv))
if (true) // [실험] 전달 실패 강제
```

**③ 구간 넓히기** (`SendPacket`): ②만으로 재진입 표시가 나오지 않을 때만 넣는다. 다른 워커가 취소 완료를 먼저 처리하게 해서 이 스레드의 `decIoCount`가 마지막이 되도록 한다.

```cpp
const bool bSent = marshalToSendBuf(*session, payload, len);
if (!bSent) // [실험]
{
	Sleep(1);
}
decIoCount(*session);
```

### 9-4. 실행

세 sln(`networkLibProject`, `EchoClient`, `LoadClient`)을 Release x64로 빌드한다. 정상 채팅 클라와 받지 않는 클라를 같은 방에 넣어 넘침 끊기를 만든다.

```powershell
.\tools\echo_test.ps1 -Name deadlock_w2 -RunSec 20 `
  -ClientCfgs LoadClient/client_chat.cfg,LoadClient/client_chatNoRecv.cfg
```

| 순서 | 빌드      | 워커 | 횟수 |
| ---- | --------- | ---- | ---- |
| 1    | ①         | 2, 4 | 각 3 |
| 2    | ① + ②     | 2, 4 | 각 5 |
| 3    | ① + ② + ③ | 2, 4 | 각 5 |

- 3번은 2번에서 재진입 표시가 한 번도 나오지 않았을 때만 한다.
- 로그는 `%TEMP%\arkchimy_echo_test\이름\server.txt`이고 CP949라 `Get-Content -Encoding Default`로 읽는다.
- 원인 위치가 필요하면 Debug 빌드를 Visual Studio로 실행해 멈춘 지점의 호출 스택을 본다.

### 9-5. 기록과 판정

회차마다 재진입 표시 횟수, `[stat]` 줄이 끊긴 시각, `[main] 종료 완료` 유무, `server_err.txt` 내용, 정상 클라 끊김을 적는다.

| 결과                      | 판단                        |
| ------------------------- | --------------------------- |
| ①만: 표시 없음, 정상 종료 | 대조군 정상                 |
| ② 이상: 표시 후 멈춤·종료 | 위험 확인, 수정 여부 결정   |
| ② 이상: 표시 후 정상 종료 | 재진입은 확인, 원인 더 조사 |
| ③까지 표시 없음           | 이 조건으로는 재현 안 됨    |

- ②는 모든 해제를 그 자리에서 처리하게 바꾸므로 다른 문제도 함께 드러날 수 있다. 재진입 표시가 없는 이상은 이 위험의 근거로 쓰지 않는다.
- 결과는 기기 이름과 함께 이 장과 README_ERRORS 2-11에 적는다. 다른 기기의 숫자와 섞지 않는다.
- 끝나면 `git worktree remove ..\_Arkchimy_deadlock`으로 작업 트리를 지운다.
- 고칠지와 방법은 결과를 보고 정한다.

### 9-6. 결과 (2026-11-06, PC A)

**전달 실패가 방 락 안에서 일어나면 멈추지 않고 서버 프로세스 전체가 비정상 종료한다.** 강제 실패 빌드 20회 중 4회에서 재진입이 일어났고, 4회 모두 종료했다. 재진입이 없던 16회는 정상 종료했다.

- 기기: PC A(Core Ultra 5 125U), 코드 `18b0758`, Release x64, 20초
- 조건: 방 16, 정상 채팅 256세션 + 받지 않는 클라 64세션
- 원본: `실험폴더/lock_reentry_2026_11_06/`(회차별 CSV, 시험 코드, 실행 스크립트)

| 빌드         | 횟수 | 재진입 | 비정상 종료 | 정상 종료 |
| ------------ | ---- | ------ | ----------- | --------- |
| ①            | 6    | 0      | 0           | 6         |
| ① + ②        | 10   | 3      | 3           | 7         |
| ① + ② + 진단 | 10   | 1      | 1           | 9         |

- 워커별 재진입은 워커 2가 10회 중 2회, 워커 4가 10회 중 2회다. 횟수가 적어 워커 수의 영향은 판단할 수 없다.
- ③ 구간 넓히기는 ②에서 재진입이 나와 하지 않았다.
- **진단**은 문서에 없던 추가 시험이다. `OnRelease`의 `leave`를 `try`로 감싸 예외를 출력하고 다시 던졌다. 출력은 다음과 같다.

```text
[deadlock-test] 방 락 안에서 OnRelease 재진입
[deadlock-test] leave 예외 inLock=1 code=36 what=resource deadlock would occur: resource deadlock would occur
```

- **증상:** 이 빌드의 MSVC `std::mutex`는 같은 스레드의 재잠금을 멈추지 않고 `std::system_error`(code 36)로 던진다. 잡는 곳이 없어 프로세스가 종료한다(`0xc0000409`, 4회 모두 `NetworkLib.dll`의 같은 주소). 표준에서는 정의되지 않은 동작이라 이 동작에 기대면 안 된다.
- **피해 범위:** 끊기는 세션 하나가 아니라 서버의 모든 연결이다. 4회 중 3회는 정상 클라까지 끊겼다(`unexpectedClose` 258–277). 나머지 1회는 클라를 끄는 시점(서버 시작 약 21초 뒤)에 종료해 클라 기록에 남지 않았다. 빠른 경우는 서버 시작 약 3초 뒤에 종료했다.
- **대조군:** ①은 표시와 비정상 종료가 없고 넘침 끊기가 1280건 있었다. 전달 실패를 강제하지 않은 6회에서는 넘침 끊기가 있어도 재진입이 없었다.
- **답하지 못한 것:** 실제 운영에서 `PostQueuedCompletionStatus`가 실패하는 빈도. 이번 시험은 실패를 강제했다.
- **판정(9-5):** "표시 후 멈춤·종료"에 해당한다. 위험은 확인됐다.

### 9-7. 결정과 시험 (2026-11-06, PC A)

**IOCP 호출 자체가 실패하면 해제하지 않고 로그를 남긴 뒤 프로세스를 멈춘다(사용자 결정).** `OnRelease`를 부르지 않으므로 방 락 재잠금 경로가 없어진다. 해제 작업 PQCS뿐 아니라 GQCS, GQCSEx, 종료 신호 PQCS도 같은 함수(`crashOnIocpFailure`)로 처리한다. 개별 I/O의 실패(끊김, 취소)와 정상 종료 신호는 지금처럼 처리한다.

- **이유:** IOCP가 작업을 받지 못하면 해제를 맡길 곳이 없고, 복구 방법도 모른다. 워커가 조용히 빠져나가면 남은 세션을 처리할 스레드가 없어 `Stop`이 끝나지 않을 수도 있다. 핸들이 닫히는 경우는 `Stop`의 순서(슬롯 전부 반환 → 워커 종료 → `CloseHandle`) 때문에 이 코드에서 일어나지 않는다. 다른 실패 조건은 Microsoft 문서에 나열되어 있지 않다.
- **멈추는 방법:** `__debugbreak`를 쓴다. 처리되지 않은 예외로 끝나 덤프 핸들러가 잡을 수 있다. `__fastfail`은 핸들러를 건너뛴다.
- **로그:** `stderr`로 남긴다. 라이브러리가 직접 출력하는 유일한 곳으로, 곧 죽어서 `GetNetError`를 읽을 쪽이 없기 때문이다.

**실제 실패 시험:** 호출 하나에만 `nullptr` 핸들을 넘기는 임시 빌드를 곳마다 따로 만들어 돌렸다. 실행된 네 곳은 모두 로그를 남기고 예외 `0x80000003`으로 종료했고, `[main] 종료 완료`는 나오지 않았다. 워커들이 동시에 같은 실패를 만나면 로그가 여러 줄 나온다.

| 호출             | 횟수 | 로그                           |
| ---------------- | ---- | ------------------------------ |
| 해제 작업 PQCS   | 1    | `PQCS(해제 작업) 실패 err=6`   |
| GQCSEx           | 1    | `GQCSEx 실패 err=6`            |
| GQCS             | 1    | `GQCS 실패 err=6`              |
| 종료 신호 PQCS   | 1    | `PQCS(종료 신호) 실패 err=6`   |
| 종료 신호 재전달 | 6    | 실행 안 됨, 6회 모두 정상 종료 |

- 해제 작업 PQCS는 이 구조 이전의 로그 형식으로 채팅 부하에서도 워커 2·4를 2회씩 시험해 같은 결과를 얻었다.
- **확인 못 한 것:** 종료 신호 재전달은 한 워커가 신호를 2개 이상 함께 꺼낼 때만 실행되는데 6회 모두 그런 일이 없었다. 잘못된 핸들 외의 실제 실패 조건도 모른다. 덤프 생성은 아직 없어 시험하지 않았다.

**채택하지 않은 후보: 묶음 끝으로 미루기.** 전달이 실패한 세션을 스레드별 목록에 넣고 `flushSends`에서 해제하는 방식이다. 시험은 통과했지만, 실패 조건을 모르는 경로에 코드를 늘리는 것보다 멈추는 편이 낫다고 정했다. 시험 결과는 아래와 같고, 이때의 전달 실패는 `if (true)`로 강제한 것이다.

| 빌드           | 횟수 | 재진입 | 비정상 종료 | 정상 종료 |
| -------------- | ---- | ------ | ----------- | --------- |
| 수정 전 ② 이상 | 20   | 4      | 4           | 16        |
| 미루기 + ②     | 20   | 0      | 0           | 20        |

전달 실패가 난 위치는 다음과 같다. 회차마다 1536건(입장 수와 같음, 세션당 카운트 0은 한 번)이고 20회 합계는 30720건이다.

| 위치            | 건수   |
| --------------- | ------ |
| `SendPacket` 안 | 5      |
| 그 밖의 워커    | 30,715 |
| 워커가 아닌 곳  | 0      |

- `SendPacket` 안의 5건은 워커 4의 10회 중 4개 회차에서 나왔다. 워커 2의 10회에서는 0건이었다.
- 20회 모두 정상 클라 끊김 0, 넘침 1280, 세션 0, 정상 종료, Application Error 이벤트 0이다.

**일반 회귀(실제 코드, 전달 실패 강제 없음):**

| 시험                | 결과                              |
| ------------------- | --------------------------------- |
| 빌드, 규칙 검사     | Release·Debug 경고 0, 위반 0      |
| 에코                | 정상 끊김 0, 정상 종료            |
| 비정상 헤더         | 4.4만 끊김, 정상 끊김 0           |
| 받지 않는 클라      | 넘침 520, 정상 끊김 0             |
| Debug + 비정상 헤더 | 3.7만 끊김, Assert 없음           |
| 방 옮기기 워커 2    | 3.9만 번, 순서 검사 통과, 넘침 0  |
| 방 옮기기 워커 4    | 6.0만 번, 순서 검사 통과, 넘침 12 |
| 섞인 클라 2회       | 정상·하트비트 끊김 0, 최대 47ms   |

- 받지 않는 클라 넘침은 이전 기록(124–192)보다 많다. 미루기 후보를 시험할 때 수정 전 빌드와 번갈아 4회씩 재도 509–527 vs 510–529로 같았으므로 해제 경로 수정 때문은 아니다. 기록보다 많은 이유는 확인하지 않았다.
- 방 옮기기 워커 4의 정상 클라 넘침은 README_ERRORS 2-7의 알려진 동작이다. 이번 1회로 수정 전과 비교하지는 않았다.
- 처리량은 비교하지 않았다. 바뀐 코드는 전달이 실패할 때만 실행된다.
