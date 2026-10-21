# accept vs AcceptEx 처리량 실험 (2026-10-17)

## 목적
블로킹 accept 스레드 1개와 AcceptEx(IOCP 워커 N개) 중 어느 쪽이 연결 수락을 더 많이 처리하는지 측정한다.
워커 스레드 수를 늘리면 처리량이 늘어나는지도 함께 본다.

## 1. 오늘 작업한 코드

### 1-1. 파일명으로 실험 변수 지정 (`_lib/ExeNameLib/ExeName.h`, 헤더 전용)
exe 파일명을 `_`로 나눈 토큰에서 값을 읽는다. 파일명에 키가 없으면 cfg 값을 쓴다.
ip/port처럼 환경에 따라 바뀌는 값은 cfg에 그대로 둔다.

| exe | 키 | 의미 | 예 |
|---|---|---|---|
| networkLibProject | `accept` / `acceptex` | 서버 모드 | `networkLibProject_acceptex_w4.exe` |
| networkLibProject | `w` | AcceptEx 워커 수 | |
| FloodClient (ConnectEx) | `s`, `w` | slotCnt, 워커 수 | `FloodClient_s16_w8.exe` |
| FloodClient_NonBlocking | `t` | 스레드 수 | `FloodClient_NonBlocking_t8.exe` |

- 실행할 때 값의 출처를 출력한다. 예: `[main] workerCnt = 4 (파일명)`
- 프로세스 단위 affinity도 넣었다가 뺐다. 의미 있는 것은 스레드 단위 affinity이고, 이것은 나중에 한다.

### 1-2. 클라이언트 즉시 시작
FloodClient와 FloodClient_NonBlocking 모두 's' 키를 기다리지 않고 실행하자마자 시작한다.

### 1-3. 서버 1초 통계 개선 (`networkLibProject/StatUtil.h/.cpp`)
- **StatTicker**
  - `timeBeginPeriod(1)`을 켜고, `nextTime += 1000` 방식으로 기다린다. 이렇게 하면 지연이 누적되지 않는다.
  - TPS는 QPC로 잰 실제 경과 시간으로 나눈다.
- **CpuSampler**
  - 프로세스 CPU를 유저/커널로 나눠 잰다 (`GetProcessTimes`, 전체 논리 코어 대비 %).
  - 시스템 전체 CPU도 잰다 (`GetSystemTimes`).
- 출력 예: `[AcceptServer] 반환 tps=… avg=… total=… | cpu 프로세스=4.6% (유저 0.6 커널 4.0) 시스템=47.5%`
- 서버와 FloodClient_NonBlocking은 `setvbuf(stdout, nullptr, _IONBF, 0)`를 쓴다. 리다이렉트한 로그가 강제 종료 때 버퍼에 남아 사라지지 않게 하기 위해서다.

### 1-4. FloodClient_Blocking을 논블로킹 클라이언트로 변경 (2026-10-19 FloodClient_NonBlocking으로 이름 변경)
- 반복 순서: `socket` → `FIONBIO` → `connect` → 상태 확인 → `SO_LINGER 0` → `closesocket`.
- 핸드셰이크를 기다리지 않는다.
- 닫기 직전 상태를 분류해 1초마다 출력한다: 수립 / 거절(10061) / 기타 / 진행 중.
  - `connect`가 0을 반환하면 수립이다.
  - `WSAEWOULDBLOCK`이면 대기 시간 0으로 `select`를 호