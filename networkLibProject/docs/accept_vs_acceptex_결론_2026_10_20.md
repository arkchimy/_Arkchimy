# IOCP 서버의 연결 수락 — accept와 AcceptEx 비교

## 1. 결정할 것

서버가 클라이언트 연결을 받는 방식을 정한다. 목표는 **연결이 몰릴 때 백로그가 가득 차 정상 연결이 거절되는 상황을 최소화**하는 것이다.

| # | 결정　　　　　　　　　　　　　　                        | 후보　　　　　　　　　　　　　　　　　　　　                                  |
|   | 　　　　　　　　　　　　　　　　                        | 　　　　　　　　　　　　　　　　　　　　　　                                  |
| 1 | 연결 수락 방식　　　　　　　　　　                      | `accept()` 전담 스레드 / `AcceptEx` + IOCP 워커　　　　　　　　　　　　　　　 |
| 2 | AcceptEx를 쓴다면 미리 걸어둘 소켓 수 (slotCnt)　　　　 | 워커 수만큼 적게 / 동시 접속 수 이상으로 넉넉하게　　                         |
| 3 | IOCP 워커 스레드 수　　　　　　　　　　                 | 1 / 2 / 4　　　　　　　　　　　　　　　　　　　　　　                         |
| 4 | 연결을 끊은 소켓의 처리　　　　　　                     | 매번 닫고 새로 생성 / `DisconnectEx`로 재사용　　　　　　　　　　             |

## 2. 구조적 차이

### 2-1. accept는 전담 스레드가 필요하다
`accept()`는 연결이 올 때까지 호출한 스레드를 멈춰 세우는 블로킹 함수다.
IOCP 워커는 `GetQueuedCompletionStatus`(GQCS)로 완료 통지를 꺼내 송수신을 처리하는 스레드라서, `accept()`에서 블록되면 그동안 다른 완료를 처리하지 못한다.
따라서 accept 방식은 수락만 하는 전담 스레드를 따로 둬야 한다.
`AcceptEx`는 수락 완료가 송수신과 같은 IOCP로 들어오므로, 별도 스레드 없이 워커가 함께 처리한다.

### 2-2. AcceptEx가 하는 일
[AcceptEx 레퍼런스](https://learn.microsoft.com/en-us/windows/win32/api/mswsock/nf-mswsock-acceptex):
> The AcceptEx function combines several socket functions into a single API/kernel transition. ... A new connection is accepted. Both the local and remote addresses for the connection are returned. The first block of data sent by the remote is received.

> If dwReceiveDataLength is zero, accepting the connection will not result in a receive operation. Instead, AcceptEx completes as soon as a connection arrives, without waiting for any data.

- 하나로 합쳐지는 것은 "accept + 주소 조회 + 첫 수신"이다. TCP 핸드셰이크와 accept를 합친다는 뜻이 아니다.
- 이 서버는 수신 크기를 0으로 넘기므로, 연결이 도착하는 즉시 완료 통지가 나온다.

[MSDN Magazine 2000.10, Windows Sockets 2.0: Write Scalable Winsock Apps Using Completion Ports](https://learn.microsoft.com/en-us/archive/msdn-magazine/2000/october/windows-sockets-2-0-write-scalable-winsock-apps-using-completion-ports) (Windows 2000 Networking group):
> If there are no accept operations pending, the event will be signaled by an incoming connection.

> With enough of these connections, your server will start to refuse connections to legitimate clients because it has no more accepts pending.

- `FD_ACCEPT`는 "백로그에 꺼내 갈 연결이 생겼다"는 신호다. 이 신호가 **걸어둔 AcceptEx가 없을 때만** 뜬다는 것은, 걸어둔 AcceptEx가 있으면 연결이 백로그에 남지 않고 그 소켓으로 바로 간다는 뜻이다.
- 걸어둔 AcceptEx를 다 쓰면 정상 클라이언트도 거절되기 시작한다. 즉 걸어둔 수가 연결을 받아낼 용량 역할을 한다.

### 2-3. 수락 경로 비교
```
AcceptEx — 걸어둔 소켓이 있을 때
  socket() → AcceptEx() 등록 ··· [핸드셰이크 완료] → 커널이 즉시 걸어둔 소켓에 붙임 → 완료 통지 → 워커가 꺼냄
                                   (서버 스레드를 기다리지 않음)

AcceptEx — 걸어둔 소켓을 다 썼을 때
  [핸드셰이크 완료] → 백로그 대기 ··· socket() → AcceptEx() 등록 → 백로그에서 꺼내 붙임 → 완료 통지

accept
  [핸드셰이크 완료] → 백로그 대기 ··· accept() (커널이 소켓 생성까지)
```

연결 1건당 서버가 부르는 시스템 콜:
| 방식　　　         | 호출　　　                                                     |
| 　　　　　         | 　　　　　                                                     |
| accept　　　　　   | `accept()` + `closesocket()`　　　　　                         |
| AcceptEx　　　　　 | `GQCS` + `closesocket()` + `socket()` + `AcceptEx()`　　　　　 |

- AcceptEx는 시스템 콜이 더 많다. AcceptEx의 이점은 호출 수가 아니라 **처리 시점**에 있다. 소켓 생성과 등록을 연결이 도착하기 **전에** 해 두기 때문에, 연결이 도착한 순간에는 서버 스레드를 기다리지 않는다.
- 걸어둔 소켓이 부족하면 연결이 백로그에서 기다려야 하므로 accept와 같은 구조가 되고, 시스템 콜은 더 많은 "비싼 accept"가 된다. 그래서 slotCnt가 AcceptEx 설계의 핵심 변수다.

## 3. 측정 환경

### 3-1. 장비와 OS
| 항목　　　　　　　　　                         | 값　　　　　　　　　　　　　　　　　　　　　                                                          |
| 　　　　　　　　　　　                         | 　　　　　　　　　　　　　　　　　　　　　　                                                          |
| PC　　　　　　　　　　　                       | LG gram 15ZB90S (노트북)　　　　　　　　　　　　　　　　　　　                                        |
| CPU　　　　　　　　　　　                      | Intel Core Ultra 5 125U - P코어 2개(4스레드) + E코어 8개 + 저전력 E코어 2개 = 논리 코어 14개, 15W급　 |
| 전원　　　　　　　　　                         | **전원 어댑터 연결** (Win32_Battery.BatteryStatus = 2로 매 실행 전 확인), 전원 계획 "균형 조정"       |
| OS　　　　　　　　　　　                       | Windows 11 Pro 10.0.26200　　　　　　　　　　　　　　　　　　　　　　                                 |
| Winsock　　　　　　　　　　　                  | 2.2 (`WSAStartup(MAKEWORD(2, 2))`)　　　　　　　　　　　　　　　　　　　　　　                        |
| ws2_32.dll / mswsock.dll　　　　　　　　　　　 | 10.0.26100.1　　　　　　　　　　　　　　　　　　　　　　                                              |
| afd.sys (커널 소켓 드라이버)　　　             | 10.0.26100.8036　　　　　　　　　　　　　　　　　　　　　　                                           |
| tcpip.sys (TCP 스택)　　　　　　　　　         | 10.0.26100.8521　　　　　　　　　　　　　　　　　　　　　　                                           |

- **루프백(127.0.0.1)으로 측정했다.** 이 노트북의 유선 랜은 USB Fast Ethernet(100Mbps)이라, 실제 랜으로 재면 서버가 아니라 랜카드 한계를 재게 된다.
- **배터리 상태 측정은 쓰지 않았다.** 배터리 상태에서는 같은 조건에서도 결과가 1.5~3배 흔들렸다. 같은 시스템 콜 비용이 두 값으로 갈리기도 했다(connect 약 220us vs 450us). 그래서 모든 측정을 전원 어댑터를 연결한 상태에서 했다.

### 3-2. 서버
- accept 서버와 AcceptEx 서버는 **같은 실행 파일**이다. 수락 방식, 워커 수, slotCnt는 실행 파일 이름으로 지정한다(예: `networkLibProject_acceptex_w4_s8.exe`). backlog는 설정 파일로 지정한다.
- 수락한 연결은 바로 `SO_LINGER(0)` + `closesocket()`으로 RST를 보내 끊는다. 이 측정은 **수락 경로만** 잰다(송수신 없음).
- backlog 200은 `listen()`이 허용하는 최솟값이다. 200 미만을 넘기면 200으로 설정되는 것을 미리 확인했다.

### 3-3. 부하 클라이언트
`FloodClient_Select`: 스레드 8개 × 스레드당 소켓 64개 = **동시 진행 연결 512개**.
```
스레드 1개의 루프 (소켓 64개)
  socket() → SO_LINGER(0) → 논블로킹 → connect()
     → select(writeSet, exceptSet, 100ms)
         writeSet  : 핸드셰이크 성공 → 성공 +1 → closesocket(RST) → 새 소켓으로 다시 connect
         exceptSet : 연결 실패(거절 등) → 실패 +1 → closesocket → 새 소켓으로 다시 connect
```
- 핸드셰이크가 끝난 뒤에 끊으므로 서버 백로그에 정상 연결이 들어간다.
- 결과가 나와야 다음 시도를 하는 구조(closed-loop)다. 서버가 느려지거나 거절이 늘면 시도 수도 줄어든다. 그래서 결과표에 시도/s를 같이 적는다.
- 이 클라를 쓴 이유는 앞서 쓴 부하기들의 문제 때문이다.
  - 블로킹 connect 클라: 스레드마다 연결 1개라 많은 스레드가 필요하고, 서버가 느려지면 클라도 같이 느려졌다.
  - 논블로킹 connect 후 즉시 close 클라: 핸드셰이크 전에 RST를 보내, 서버가 아니라 커널의 half-open 연결 처리를 측정하게 됐다.
  - ConnectEx + IOCP 클라: 연결마다 `bind()`가 필요해 그 비용으로 클라가 먼저 느려졌다.

### 3-4. 측정 절차
```
서버 시작 → 2초 → 클라 시작 → 3초 워밍업 → (프로파일 리셋) → 측정 구간 → (프로파일 저장) → 클라 종료 → 서버 종료 → 10초 대기
```
- 측정 구간은 5분이다. backlog 65535 재측정만 1분이다.
- 매 실행 전에 포트 32000에 남은 연결이 0개인지 확인했다.
- 조건마다 1회 측정했다. 같은 조건을 두 번 잰 경우는 결과 절에 둘 다 적는다.

### 3-5. 지표
| 지표　　　           | 뜻　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　                                                |
| 　　　　　           | 　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　                                                |
| AcceptTPS　　　　　  | 서버가 1초에 처리한 수락 수(accept 반환 수 / AcceptEx 완료 통지 수). 표에는 측정 구간의 평균, 중앙값, 최소~최대를 적는다　　　　        |
| 시도/s　　　         | 클라의 초당 성공 + 실패 수　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　                                          |
| 실패율　　           | 측정 구간의 실패 수 / 시도 수. 누적값이 아니라 구간 비율이라 측정 시간과 무관하다　　　　　　　　　　　                                 |
| SrvCPU (유저/커널)　 | 서버 프로세스 CPU 시간(`GetProcessTimes`)을 1초 단위로 차분. **논리 코어 14개 전체를 100%로 본 비율**　　　　　　　　　　　　　　　　　 |
| 클라CPU　　　        | 클라 프로세스의 측정 구간 CPU 시간 / (경과 시간 x 14)　　　　　　　　　　　　　　　　　　　　　　　　　　　                             |
| SysCPU　　　　　     | 시스템 전체 CPU(`GetSystemTimes`), 같은 기준　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　                      |

프로파일 태그 (QPC로 호출 직전~반환 직후를 잼):
| 태그　　　                         | 구간　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　                                        |
| 　　　　　                         | 　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　                                        |
| `AcceptEx_GQCS`　　　　　          | GQCS 호출. **완료를 기다린 시간이 포함**되므로, 길면 워커가 일감을 기다리며 놀고 있다는 뜻이다　　                  |
| `AcceptEx_Complete`　　　　　      | 완료를 꺼낸 뒤 처리 전체(closesocket + socket + AcceptEx 등록 포함)　　　　　　　　　　　　　　　　　　　　　　　　 |
| `AcceptEx_Socket`　　　　　        | 다음 연결용 `socket()`　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　                            |
| `AcceptEx_Post`　　　　　          | `AcceptEx()` 등록 호출　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　                          |
| `CloseReset_CloseSocket`　　　　　 | `closesocket()`　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　                         |
| `Accept_Accept`　　　　　          | `accept()` 호출. 블로킹이라 **연결을 기다린 시간이 포함**된다　　　　　　　　　　　　　　　　　　                   |
| 클라 `Flood_Select`　　　          | select 대기. 길면 클라가 서버를 기다린다는 뜻(서버 병목), 짧으면 클라가 병목　　　　　　　　　　                    |

## 4. 측정 결과와 해석

### 4-1. accept vs AcceptEx
**주장:** backlog 200 조건에서 AcceptEx가 accept보다 처리량이 높고 덜 흔들린다.

**조건**
- backlog 200, 5분 연속, 각 1회.
- accept: 전담 스레드 1개. AcceptEx: 워커 1개, slotCnt 1000.
- 클라: 동시 진행 연결 512개.

**결과**
- accept: AcceptTPS 평균 11,212/s, 중앙값 11,142, 최소 2,998 ~ 최대 19,749. 실패율 0.07%(실패 8.3/s). 실패는 처음 3분 동안에만 났다.
- AcceptEx: AcceptTPS 평균 15,844/s, 중앙값 16,138, 최소 11,705 ~ 최대 22,955. 실패 0건.
- 서버 CPU(유저/커널): accept 0.5% / 3.1%, AcceptEx 0.7% / 5.5%.

**의미**
- 평균은 AcceptEx가 1.4배 높다. 최솟값은 3.9배 높다. accept는 1초 처리량이 3천까지 떨어지는 구간이 있었고, AcceptEx는 1만 1천 아래로 떨어지지 않았다.
- 실패가 accept에서만 난 것은, 백로그 200칸이 동시 연결 512개보다 작아 백로그가 찼기 때문이다.
- AcceptEx는 걸어둔 소켓 1000개가 동시 연결 512개보다 많아서, 이 조건에서는 구조상 거절이 날 수 없었다. 따라서 이 측정은 **거절 수를 같은 조건에서 비교한 것이 아니다.** 같은 조건에서 거절을 비교하려면 동시 연결 수가 backlog + slotCnt보다 많아야 한다.
- 서버 커널 CPU는 AcceptEx가 1.8배 높다. 처리량(1.4배)보다 더 늘었는데, 연결 1건당 시스템 콜이 더 많다는 2-3의 구조와 맞는다.

**판정:** backlog 200 조건에서는 뒷받침된다. backlog를 크게 둔 조건에서도 같은지는 이 문서에서 판단하지 않는다(5분 재측정 여부 미정).

### 4-2. AcceptEx의 수용 용량 = backlog + slotCnt
**주장:** 걸어둔 AcceptEx 소켓은 백로그 칸을 같이 쓰지 않는다. 서버가 한 번에 받아둘 수 있는 연결 수는 backlog + slotCnt다.

**조건**
- AcceptEx 서버, **워커 0개**, backlog 200, slotCnt 300 / 100. 워커가 없으므로 완료 통지를 꺼내지 않고 소켓을 다시 걸어두지도 않는다. 걸어둔 소켓은 한 번씩만 쓰이고, 그 뒤의 연결은 백로그에 쌓이기만 한다.
- 클라: 동시 진행 연결 512개(4-1과 같은 클라). 성공하면 RST로 끊지만, 끊긴 연결도 백로그 칸에서 빠지지 않는다.
- 따라서 성공 수가 멈춘 값이 서버의 수용 용량이다. 각 2회.

**결과**
| slotCnt | 성공 수 1회차 | 성공 수 2회차 | 별도 공간일 때 예측 (200 + slotCnt) | 백로그 칸을 같이 쓸 때 예측 |
|         | 　　　　　    | 　　　　　    | 　　　　　　　　                    | 　　　　　　　　　　　      |
| 300     | 500　　　　　 | 501　　　　　 | 500　　　　　　　　                 | 200　　　　　　　　　　　   |
| 100     | 303　　　　　 | 306　　　　　 | 300　　　　　　　　                 | 200　　　　　　　　　　　   |

**의미**
- 성공 수는 첫 1초 안에 위 값까지 오르고 그 뒤로 멈췄다. 이후에는 동시 진행 512개가 전부 거절됐다.
- 두 slotCnt 모두 "200 + slotCnt"와 맞는다. 걸어둔 소켓은 백로그와 별도 공간이다. 2-2에서 MSDN 글을 근거로 한 해석("걸어둔 수가 받아낼 용량 역할을 한다")이 Windows 11에서도 성립한다.
- 예측보다 0~6개 많다. 원인은 확인하지 않았다. 용량 계산에 주는 차이는 1~2%다.

**판정:** 확인됨. AcceptEx의 수용 용량은 backlog + slotCnt다.

### 4-3. 같은 용량에서 비교하면
4-1에서 AcceptEx가 거절 0이었던 것은 걸어둔 소켓 1000개만큼 용량이 컸기 때문일 수 있다. accept도 backlog를 크게 잡으면 용량을 늘릴 수 있으므로, 용량을 같게 맞춰 다시 비교했다.

**주장:** 걸어둔 소켓을 다 써서 연결이 백로그까지 닿으면, 워커 1개 AcceptEx는 accept보다 거절이 많다. 워커를 2개 이상 두면 accept보다 거절이 적고 처리량도 높다.

**조건**
- 수용 용량 256으로 맞춤: accept = backlog 256, AcceptEx = backlog 200 + slotCnt 56 (4-2의 계산식).
- 클라: 동시 진행 연결 512개(4-1과 같음). 용량 256보다 많으므로 양쪽 모두 백로그가 찰 수 있다.
- accept는 수락 스레드 1개, AcceptEx는 워커 1 / 2 / 4개. 각 5분 1회.

**결과**
| 서버                    | AcceptTPS 평균 | 중앙값       | 최소 ~ 최대            | 실패율       | 실패가 난 초        |
| 　　                    | 　　           | 　　　       | 　　　　               | 　　　       | 　　　　　          |
| accept, backlog 256　　 | 10,854　　     | 11,142　　　 | 2,211 ~ 15,909　　　　 | 0.038%　　　 | 50 / 294　　　　　  |
| AcceptEx 워커 1         | 10,215　　     | 10,668　　　 | 679 ~ 17,418　　　　   | 0.336%　　　 | 263 / 295　　　　　 |
| AcceptEx 워커 2         | 13,455　　     | 13,821　　　 | 2,522 ~ 19,360　　　　 | 0.006%　　　 | 19 / 300　　　　　  |
| AcceptEx 워커 4         | 12,507　　     | 12,973　　　 | 5,014 ~ 17,221　　　　 | 0.000%　　　 | 0 / 300　　　　　   |

1분 단위 초당 실패 수:
| 서버                    | 1분    | 2분    | 3분    | 4분    | 5분    |
| 　　                    | 　     | 　     | 　     | 　     | 　     |
| accept, backlog 256　　 | 15.6　 | 3.5　  | 0.3　  | 0.1　  | 0.8　  |
| AcceptEx 워커 1         | 22.4　 | 41.5　 | 34.5　 | 39.8　 | 37.9　 |

**의미**
- **워커 1개면 AcceptEx가 accept보다 못하다.** 실패율이 약 9배(0.336% vs 0.038%)이고 처리량도 6% 낮다. accept는 처음 1~2분 뒤 실패가 거의 0으로 줄었지만, AcceptEx 워커 1개는 5분 내내 초당 35~40건이 이어졌다(실패가 난 초 263 / 295).
- 4-1의 거절 0은 수락 방식이 아니라 **slotCnt 1000만큼 용량이 커서**였다. slotCnt는 백로그 앞에서 연결을 먼저 받아주는 보조 용량이고, 그것을 다 쓰고 백로그까지 닿으면 이 이점은 사라진다.
- 워커 1개가 약한 이유는 완료 꺼내기, closesocket, socket, AcceptEx 다시 걸기를 혼자 다 하기 때문으로 본다. 쓴 소켓을 다시 걸어두는 속도가 accept 스레드가 백로그를 비우는 속도보다 느리다. 워커 1개 측정은 프로파일을 저장하지 않아 직접 확인하지는 않았다.
- **워커 2개부터 역전된다.** 워커 2개는 실패율 0.006%(accept의 1/6), 처리량 13,455/s(accept의 1.24배)다. 워커 4개는 실패 0이고 처리량은 12,507/s로 accept보다 높지만 워커 2개보다는 7% 낮다.
- 한계: accept는 수락 스레드 1개, AcceptEx는 워커 2 / 4개라 스레드 수가 다르다. accept 수락 스레드를 늘린 비교는 하지 않았다.

**판정:** 뒷받침된다. 같은 용량이면 워커 1개 AcceptEx는 accept보다 거절이 많고, 워커 2개 이상이면 accept보다 거절이 적고 처리량이 높다.

### 4-4. slotCnt를 늘리면
**주장:** slotCnt를 늘리면 백로그를 거쳐 소켓을 붙이는 비용이 줄고 처리량이 오른다. 다만 효과는 보조적이다.

**조건**
- 4-3과 같은 서버와 클라, backlog 200, 워커 2 / 4개에서 slotCnt만 56 → 500으로 바꿈. slotCnt 500은 용량 700이 동시 연결 512보다 커서 거절은 구조상 나지 않는다.
- slotCnt 500은 3분, slotCnt 56은 5분(4-3의 측정). 각 1회, 서버 프로파일 저장.

**결과** (프로파일은 호출 1회 평균, 워커 평균, 단위 us)
| 서버                | AcceptTPS 평균 | GQCS 대기 | 완료 처리 전체    | AcceptEx 등록 | socket() | closesocket() |
| 　　                | 　　           | 　　      | 　　　　　　      | 　　          |          |               |
| 워커 2, slotCnt 56  | 13,455　　     | 43.7　　  | 104.4　　　　　　 | 16.9　　      | 31.3     | 51.5          |
| 워커 2, slotCnt 500 | 14,969　　     | 9.7　　   | 123.6　　　　　　 | 4.4　　       | 53.0     | 61.0          |
| 워커 4, slotCnt 56  | 12,507　　     | 123.6　　 | 195.6　　　　　　 | 32.4　　      | 89.7     | 67.7          |
| 워커 4, slotCnt 500 | 13,404　　     | 62.1　　  | 236.3　　　　　　 | 14.2　　      | 145.2    | 71.4          |

**의미**
- **AcceptEx 등록 비용이 줄었다.** 워커 2개에서 16.9us → 4.4us(1/4), 워커 4개에서 32.4us → 14.2us(1/2). 걸어둔 소켓이 모자라면 등록 호출이 백로그에서 연결을 꺼내 붙이는 일까지 하므로 비싸진다. slotCnt를 늘리면 그 경로를 덜 탄다.
- **처리량은 7~11% 올랐다**(워커 2개 13,455 → 14,969, 워커 4개 12,507 → 13,404). 같은 조건 회차 간 차이가 약 6%였으므로 큰 차이는 아니다.
- **완료 처리 전체 시간은 줄지 않고 늘었다.** socket()이 워커 2개에서 31.3us → 53.0us, 워커 4개에서 89.7us → 145.2us로 늘어, 등록 비용이 줄어든 몫보다 크다. 처리량이 오른 것은 GQCS 대기, 즉 워커가 일감을 기다리며 노는 시간이 줄었기 때문이다(워커 2개 43.7us → 9.7us).
- 워커 2개 → 4개에서 완료 처리가 2배로 느는 현상은 slotCnt 500에서도 그대로다(123.6us → 236.3us, socket() 53.0us → 145.2us). 워커를 늘렸을 때의 비용 증가는 백로그가 아니라 socket() 쪽에서 온다.
- socket()이 비싸지는 원인과 GQCS 대기가 줄어든 원인은 확인하지 않았다(6절).

**판정:** 일부 뒷받침된다. slotCnt를 늘리면 백로그 경로의 등록 비용이 줄고 처리량이 조금 오르지만, 연결 1건의 처리 비용 자체는 줄지 않는다. slotCnt는 백로그 앞의 보조 용량으로서 의미가 크고, 처리 비용을 줄이는 수단은 아니다.
