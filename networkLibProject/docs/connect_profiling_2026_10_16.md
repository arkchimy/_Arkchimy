# ConnectEx(IOCP) 클라이언트가 블로킹 connect보다 느린 이유 — 프로파일러 측정 (2026-10-16)

`handoff_2026_10_15.md` 10절의 "블로킹 connect가 ConnectEx+IOCP보다 빠르다"를 프로파일러로 단계별로 재서
원인을 확인했다. **결론: IOCP/ConnectEx 방식이 느린 게 아니라, ConnectEx 클라이언트가 동시에 걸어 둔
연결 수(slotCnt 10000)가 원인이었다. 동시 연결 수를 8로 맞추자 두 방식의 처리량이 같아졌다.**
10절의 결론(syscall 추가분 + GQCS 워커 깨우기 비용)은 이 측정으로 반박된다.

---

## 1. 비교 대상

| 프로젝트 | 방식 | 동시 연결 수 | 스레드 |
|---|---|---|---|
| `FloodClient` | ConnectEx + IOCP | `slotCnt` (10000 → 8) | 워커 `workerCnt` = 8 |
| `FloodClient_Blocking` | 블로킹 `connect()` | 스레드당 1개 = 8 | `threadCnt` = 8 |

- `FloodClient_Blocking`은 이전 블로킹 버전(바탕화면 백업)을 새 솔루션으로 옮긴 것. 코드는 원본과 같고
  프로파일러 태그, 스레드 이름, 키 입력(A 저장 / D 리셋 / Q 종료)만 추가.
- 두 버전 모두 `_lib/ProfilerLib`(`ProfileScope`)로 단계별 시간을 잼. **공통 단계는 같은 태그 이름**을 써서
  결과 파일의 같은 행끼리 비교할 수 있게 함.
- 환경: 루프백(127.0.0.1), Release x64. TPS는 서버 쪽 집계 기준.

### 태그 대응
```
                FloodClient_Blocking            FloodClient (ConnectEx)
소켓 생성        Flood_Socket                    Flood_Socket
연결            Flood_Connect (한 호출)          Flood_Bind + Flood_IocpRegister + Flood_ConnectEx
                                                + Flood_GQCS + Flood_UpdateContext
LINGER          Flood_SetLinger                 Flood_SetLinger
닫기            Flood_CloseSocket               Flood_CloseSocket
1건 전체        Flood_Loop                      Flood_GQCS + Flood_Complete
```

---

## 2. 측정 결과 (워커 8개 평균, us)

| 태그 | Blocking | ConnectEx slotCnt 10000 | ConnectEx slotCnt 8 |
|---|---|---|---|
| Socket | 127 | 44 | 137 |
| Bind | (connect 안에 포함) | **414** (최소 25~33) | 71 (최소 22~23) |
| IocpRegister | — | 3.9 | 2.7 |
| ConnectEx / Connect | 235 | 161 | 150 |
| GQCS | — | 3.1 | 1.6 |
| UpdateContext | — | 6.7 | 4.2 |
| SetLinger | 1.5 | 1.1 | 1.1 |
| CloseSocket | 177 | **793** (최소 28~40) | 169 (최소 11~21) |
| **1건 전체** (Loop / Complete) | **541** | **1427** | **538** |
| 서버 측 TPS | 약 15,000 | 약 6,000 | Blocking과 동일 |

결과 파일: `FloodClient/x64/Release/FloodClient_20261016_162706_Profiler.txt`(10000),
`FloodClient_20261016_164132_Profiler.txt`(8), `FloodClient_Blocking_20261016_162624_Profiler.txt`.

### 처리량 계산이 실측과 맞음
```
처리량 상한 = 스레드 수 ÷ 1건당 처리 시간
Blocking          8 ÷ 0.541ms ≈ 14,800 TPS   (실측 약 1.5만)
ConnectEx 10000   8 ÷ 1.427ms ≈  5,600 TPS   (실측 약 6,000)
ConnectEx 8       8 ÷ 0.538ms ≈ 14,900 TPS   (실측 Blocking과 동일)
```
→ 두 방식 모두 **워커가 처리 작업만으로 꽉 차 있고**, 처리량은 1건당 처리 시간으로 정해진다.

---

## 3. 알아낸 것 (확실한 순)

### 3-1. GQCS(완료 대기 + 워커 깨우기)는 원인이 아니다 — 확정
- `Flood_GQCS`가 3.1us / 1.6us로 1건 처리 시간의 0.2~0.3%.
- 10절의 "GQCS는 컨텍스트 스위치 비용이 숨어 있어 비싸다" 가설은 기각.
- slotCnt 8에서도 GQCS가 작은 이유(추정): `Flood_ConnectEx` 호출 자체가 150us로, 등록만 하고 돌아오는
  비동기 호출치고 너무 길다. 루프백에서는 핸드셰이크가 ConnectEx 호출 안에서 거의 끝나서,
  반환 시점에 완료 패킷이 이미 큐에 있는 것으로 보인다. → 루프백에서는 비동기의 이점이 거의 없다.

### 3-2. 차이는 `closesocket`(70%)과 `bind(0)`(39%)에서 났다 — 확정
- 1건 전체 차이 약 890us = CloseSocket −624 + Bind −344 + Socket +93 + 기타 −15.
- 안쪽 태그 합계(1425 / 535)가 바깥 `Flood_Complete`(1430 / 538)와 일치 → 측정이 빠뜨린 구간 없음.

### 3-3. 원인은 동시 연결 수다 — 확정
- 바꾼 조건은 `slotCnt` 하나(10000 → 8). 스레드 수, 코드, 서버는 그대로.
- slotCnt 8에서 `CloseSocket`, `Bind`가 Blocking 수준으로 떨어지고 처리량도 같아짐.

### 3-4. 늘어난 것은 "일의 양"이 아니라 "기다린 시간"이다 — 근거 강함
- 두 태그 모두 **최소값(경합 없이 처리된 호출)은 그대로, 평균만 4~6배**.
- 프로파일러는 QPC 벽시계 시간이라 락 대기, 선점, CPU 차례 대기가 모두 포함된다.

### 3-5. 그 대기의 출처 — 미확정
- 클라이언트 워커 수는 두 조건 모두 8개이고, 락 안의 일의 양도 같다(최소값 동일).
  → **워커 8개끼리의 경합만으로는 4~6배 대기를 설명할 수 없다.**
- 남는 후보:
  1. **워커 바깥의 경쟁자**: 루프백이라 클라이언트와 서버가 같은 커널 TCP 스택을 쓴다.
     서버 스레드와 커널의 패킷 처리(SYN/RST 수신)가 같은 포트·연결 테이블을 건드린다.
     1만 개 조건에서는 연결 1건이 약 1.7초 살아 있고(10000 ÷ 6,000), 8개 조건에서는 약 0.5ms.
  2. **락이 아닌 대기**: CPU 부족으로 실행 준비가 됐는데 차례를 기다린 시간
     (클라이언트, 서버, 커널이 같은 14개 논리 코어를 나눠 씀).
- 두 측정 모두 CPU%를 기록하지 않아서 구분하지 못했다.

---

## 4. 측정 중에 기각한 해석

| 해석 | 기각 이유 |
|---|---|
| GQCS의 워커 깨우기 비용이 크다 | GQCS가 1건 시간의 0.3% |
| closesocket이 느린 건 IOCP에 등록된 소켓이라서 | IOCP 등록(4us), UpdateContext(7us) 같은 핸들·IOCP 작업은 빠름. slotCnt만 줄여도 Blocking 수준으로 내려감 |
| 프로세스 핸들 테이블을 순차 탐색해서 느리다 | 핸들 테이블은 핸들 값이 인덱스라 바로 찾아감. 핸들을 쓰는 다른 호출(SetLinger 1us 등)은 연결 수와 무관하게 빠름 |
| 워커 8개가 커널 락을 더 오래 잡아서 대기가 늘었다 | 락을 오래 잡는다면 최소값도 늘어야 하는데 그대로임. 경쟁 스레드 수도 같음 |

---

## 5. slotCnt를 정하는 기준

slotCnt = 동시에 진행 중인 연결 수. 워커 수와 따로 정하는 값이다.

```
필요한 동시 연결 수 ≈ 목표 처리량 × 연결 1건이 끝나는 데 걸리는 시간   (리틀의 법칙)
루프백: 15,000 TPS × 약 0.5ms ≈ 8개
```
- 더 걸어 두면 처리량은 늘지 않고 커널이 들고 있는 연결만 늘어 `bind`/`closesocket`이 느려진다.
- 지연이 큰 실제 네트워크(RTT 수십 ms)라면 같은 처리량에 수백 개가 필요하다.

| 목적 | 적절한 slotCnt |
|---|---|
| 연결 속도 비교 | 처리량 × 지연에 맞는 작은 값 (루프백이면 한 자릿수~수십) |
| backlog 채우기, 고동시성 실험 | 큰 값 (1만 개가 의미 있는 경우) |

현재 `FloodClient/client.cfg`(프로젝트 폴더, `x64/Release`)는 `slotCnt = 8`.

---

## 6. 다음 할 일
- [ ] 두 조건(slotCnt 10000 / 8)의 서버·클라이언트 CPU% 기록 → 3-5의 후보 2(CPU 부족) 판정
- [ ] 서버를 다른 PC에서 실행하고 slotCnt 10000 재측정 → 3-5의 후보 1(루프백에서 서버와 커널 공유) 판정
      (유선 100Mbps라 TPS는 비교 불가, `CloseSocket`/`Bind`의 평균 대 최소 비율로 비교)
- [ ] (선택) WPR + WPA 대기 분석으로 `closesocket`/`bind`가 멈춘 커널 함수 확인
- [ ] (선택) slotCnt 8, 16, 64, 256, 1000, 10000으로 늘려 가며 TPS와 `CloseSocket`이 꺾이는 지점 측정
- [ ] `handoff_2026_10_15.md` 10절을 이 문서 기준으로 고쳐 쓰기 (지금은 상단에 정정 안내만 추가함)
