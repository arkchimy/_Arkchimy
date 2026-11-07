# 네트워크 라이브러리 소스 구조 정리 (설계 논의)

- 시작: 2026-11-07, PC A
- 상태: 결정, 구현 완료(2026-11-07). 시험과 처리량 비교는 하지 않았다(4장)
- 범위: 헤더만 나눈다. `NetworkLib.cpp`는 한 파일로 둔다

---

## 1. 요약

**`NetworkLib.h`를 공개 헤더 3개와 내부 헤더 3개로 나누고, cpp는 나누지 않는다.** 함수 내용과 순서는 그대로다.

- 효과: 서버가 `Session`, `RingBuffer`, `MyOverlapped`, `WorkerCounter`의 정의를 보지 않는다
- 코드 변경: `NetworkLib` 생성자 선언과 내보내기, 서버 파일 include 1줄, `NetworkLib.cpp` include

코드 검토에서 찾은 항목(`TCP_NODELAY`, 넘침 통계 등)은 이 작업에 넣지 않고 테스트 때 다룬다.

## 2. 헤더 배치

**공개 헤더** - 서버가 include한다.

| 파일           | 들어갈 것                             |
| -------------- | ------------------------------------- |
| `NetworkLib.h` | `NetworkLib`, `WsadataRAII`           |
| `NetConfig.h`  | `NetConfig`, `NetError`, 링 크기 상수 |
| `NetStat.h`    | `NetStat`                             |

**내부 헤더** - `NetworkLib.cpp`만 include한다.

| 파일              | 들어갈 것                                           |
| ----------------- | --------------------------------------------------- |
| `Session.h`       | `Session`                                           |
| `IoOverlapped.h`  | `eIoType`, `MyOverlapped`, `AcceptOv`               |
| `WorkerCounter.h` | `WorkerCounter`, `AddSingleWriter`, `CountIoResult` |

- 링 크기 상수를 공개 헤더에 두는 이유: `main.cpp:267`이 `SEND_RING_SIZE`를 출력한다. 그래서 링 크기를 바꾸면 서버도 다시 컴파일해야 한다
- `NetworkLib.h`는 `Session`, `MyOverlapped`, `AcceptOv`, `WorkerCounter`를 선언만 한다. `MSWSock.h`와 `RingBuffer.h`는 include하지 않는다
- `ChatServer.h`는 `NetworkLib.h`를 거쳐 얻던 `PacketHeader.h`를 직접 include한다
- `NetworkLib.h`가 `WinSock2.h`를 먼저 include하는 순서는 유지한다(README_ERRORS 1-26)

## 3. 결정

### 3-1. `Session` 숨기기

결정: **생성자를 선언하고 DLL에서 내보낸다** (사용자, 2026-11-07)

`NetworkLib`에 선언한 생성자가 없으면 서버 컴파일러가 암시적 생성자를 만들고, 그 생성자가 멤버 파괴 코드(`~vector<Session>`)까지 요구한다. 같은 모양의 작은 클래스를 `cl /std:c++20`으로 컴파일해 비교했다. 표의 생성자는 모두 암시적이거나 선언만 한 경우다.

| 멤버                                | 암시적 생성자 | 생성자 선언 |
| ----------------------------------- | ------------- | ----------- |
| `vector<Session>`                   | 실패, `C2036` | 통과        |
| `std::unique_ptr<Session>` 배열     | 실패, `C2338` | 통과        |
| 프로젝트 `UniquePtr<Session>` 배열  | 경고, `C4150` | 통과        |
| `vector<Session*>`, 나머지는 그대로 | 실패, `C2036` | 시험 안 함  |
| 내부 타입 3개 모두 일반 포인터      | 통과          | 시험 안 함  |

- 일반 포인터는 세 멤버와 사용 코드를 고쳐야 하고, 해제를 직접 해야 하며, 세션 메모리가 연속 배치에서 개별 할당으로 바뀐다. 핫패스의 메모리 배치까지 바뀌어 파일 정리 범위를 넘으므로 채택하지 않았다
- `vector<Session>`을 유지해 세션 메모리 배치는 그대로다
- 실제 프로젝트에서 Release·Debug 링크가 통과했고, 내보내기는 생성자 1개만 늘었다

### 3-2. cpp 나누기

결정: **나누지 않는다** (사용자, 2026-11-07)

`NetworkLib_Worker/Accept/Recv/Send/Session/Watch.cpp`로 나눠 빌드까지 했다가 커밋 전에 되돌렸다.

- 약 1,060줄은 한 파일로 읽기에 무리가 없고, 49줄짜리 파일까지 생겼다
- 수신 → `OnRecv` → 송신 → 해제 경로를 읽을 때 파일을 계속 오가야 한다
- 파일을 나눈 탓에 `NetworkLibInternal.h`, `extern tWorkerIdx`, `reserveSendTargets`가 필요해졌다
- 핫패스 함수가 여러 파일로 흩어져 인라인 유지를 따로 확인해야 했다

찾기 쉽게 하고 싶으면 파일을 나누는 대신 한 파일 안에서 함수를 기능별로 모은다. 이번에는 하지 않았다.

## 4. 확인한 것과 하지 않은 것

**확인함**

- 서버 sln의 Release·Debug 빌드 경고 0, 규칙 훅 전체 검사 위반 0
- 옮기기 전 `NetworkLib.cpp`와 비교: include 5줄과 생성자 2줄만 늘고 빠진 줄 없음
- 내보내기: 생성자 1개 추가

**하지 않음** (사용자 결정, 2026-11-07 - 검증은 일단 하지 않는다)

- 서버 실행, 에코 회귀 3종, Debug 서버 + 비정상 헤더, 섞인 클라, 방 옮기기 순서 검사
- 옛 서버 exe와 새 DLL을 섞어 실행. 멤버 배치가 그대로라 `.def`의 `LIBRARY` 이름은 바꾸지 않았다
- 처리량 비교. 비교한다면 PC B에서 `1166e17`과 번갈아 잰다
