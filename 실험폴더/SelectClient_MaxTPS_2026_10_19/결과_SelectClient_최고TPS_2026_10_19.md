# accept vs AcceptEx(w1/w2/w4) 최고 TPS — Select 클라 8스레드×64 (루프백, 2026-10-19)

## 측정 목적
스레드를 적게 쓰는 클라(`FloodClient_Select`)로 최대 부하를 걸어, 서버 수락 스레드 수(1/2/4)에 따른 최고 TPS와 CPU를 본다.

## 측정 조건
- **서버** (같은 바이너리, 파일명으로 모드 선택): accept(스레드 1) / AcceptEx w1·w2·w4 (slotCnt 1000 고정). backlog 65535.
- **클라이언트**: `FloodClient_Select_t8_o0` — 스레드 8 × 세션 64 = 연결 512개 상시 진행.
  논블로킹 connect → select(timeout 0, 폴링)로 핸드셰이크 완료 확인 → `SO_LINGER(0)` close. 속도 제한 없음.
- **절차**: 서버 시작 → 2초 → 클라 시작 → 3초 워밍업 → 프로파일 리셋 → 12초 → 저장 → 클라 종료 → 3초 → 서버 종료 → 10초 대기
- **반복**: 3회, 회차마다 서버 순서 변경. 표는 3회 중앙값. 시작 전 포트 32000 잔여 연결 0 확인.

## 결과 (클라 8스레드×64, 3회 중앙값)
```
서버          AcceptTPS (min~max)     클라 연결TPS 실패  SrvCPU(유저/커널)  클라CPU  SysCPU  클라 루프 1회
-----------  -----------------------  ----------  ----  -----------------  ------  ------  ------------
accept w1     7,554 ( 7,482~13,725)       9,673     0     0.6 / 3.2         38.4    49.0        177us
AcceptEx w1   7,032 ( 6,590~10,999)       9,268     0     0.7 / 3.2         38.8    47.3        165us
AcceptEx w2   8,829 ( 8,293~10,439)      11,086     0     0.7 / 6.7         33.2    41.1     48,395us
AcceptEx w4  10,057 ( 9,664~10,273)      10,565     0     1.1 / 8.4         31.3    47.7     50,900us
```
(CPU%는 PC 전체(논리코어 14개)를 100으로 본 비율. SrvCPU는 서버 프로세스의 GetProcessTimes 1초 차이 ÷ (경과 시간 × 14), 클라CPU는 같은 방식으로 측정 구간 전체, SysCPU는 GetSystemTimes 기준. 클라 연결TPS = 클라가 초당 핸드셰이크를 완료한 수(select writeSet 반응).
 핸드셰이크는 커널이 처리하므로, 클라 연결TPS − AcceptTPS = 서버 백로그에 쌓이는 속도)

## 서버 프로파일 (us, 3회 중앙값)
```
accept w1    : accept() 109 / closesocket 25
AcceptEx w1  : 완료 138 (등록 67, socket 37) / closesocket 27 / GQCS 4
AcceptEx w2  : 완료 156 (등록 5, socket 67)  / closesocket 77 / GQCS 28
AcceptEx w4  : 완료 221 (등록 5, socket 131) / closesocket 78 / GQCS 175
```

## 관찰
- **두 구간으로 갈린다.**
  - **w1 (accept, AcceptEx)**: 클라 연결TPS가 AcceptTPS보다 초당 ~2k 많음 → 백로그가 쌓임 = **서버가 병목**. 클라 루프는 ~170us로 쉬지 않고 돈다.
  - **AcceptEx w2·w4**: 클라 연결TPS ≈ AcceptTPS(백로그가 쌓이지 않음), GQCS 28~175us(워커가 일감을 기다림) → **서버는 여유, 클라가 병목**. 이 두 값은 서버 최고치가 아니다.
- **스레드 1개끼리는 accept ≈ AcceptEx.** 7.6k vs 7.0k, 1건 처리 시간도 accept 134us(accept+close) vs AcceptEx 165us(완료+close)로 비슷하다. 이전 측정의 차이(11k vs 15k)가 병렬도에서 왔다는 해석과 맞는다.
- **클라 병목 구간에서 클라 루프 1회가 ~50ms.** 이때 루프마다 64개를 전부 재활용(socket+connect+close ×64)하므로 연결 1건당 ~780us. 클라 CPU는 31~33%(8스레드 중 ~4.4코어)로, o0 폴링인데도 스레드가 절반가량 블록돼 있다 → 클라 쪽 socket/connect/close 경로에서도 대기가 생긴다. 어느 호출인지는 태그가 없어 미확정.

## 주의
- 1회차 accept w1(13.7k)·AcceptEx w1(11.0k)이 다른 회차보다 높다(전체 첫 두 실행). 원인 미확정. 중앙값에서는 제외됨.
- 루프백, 서버·클라 CPU와 커널 TCP 스택 공유. 상대 비교용.
- 원본: `logs/`(CP949), `server_*/`·`client/`의 `*_Profiler.txt`, 회차별 값 `raw_runs.csv`.
