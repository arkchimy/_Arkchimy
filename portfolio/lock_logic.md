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
- **해제 작업 전달 실패:** `PostQueuedCompletionStatus`가 실패하면 `decIoCount`는 그 자리에서 `releaseSession`을 부른다. 방 락 안에서 이 일이 일어나면 `OnRelease`가 같은 스레드에서 방 락을 다시 잡을 수 있다. 재현 시험은 없다.
