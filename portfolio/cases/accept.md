# IOCP 서버의 연결 수락 — AcceptServer와 AcceptExServer 비교

## 1. 목표

IOCP 서버의 연결 수락을 **AcceptServer**(`accept()` 전담 스레드)와 **AcceptExServer**(`AcceptEx` + IOCP 워커) 중 어느 쪽으로 할지,
 **측정한 성능 지표를 기준으로 판단하고 그 근거와 결정을 남긴다.**

**판단 기준 지표**
| 지표　　　             | 보는 것　　　　　　　　　　　　　　         |
| 실패율　　             | 연결이 몰릴 때 정상 연결이 거절되는 비율    |
| AcceptTPS 평균　　　   | 1초에 수락한 연결 수(TPS)　　　　　　　　　 |
| AcceptTPS 중앙값, 최소 | 처리량이 흔들리지 않고 유지되는지　　       |

**결정할 것**
| # | 결정　　　　　　　　                             | 후보　　　　　　　　                              |
| 1 | 연결 수락 방식　　　　                           | AcceptServer / AcceptExServer　　　　　　　　　　 |
| 2 | AcceptExServer라면 미리 걸어둘 소켓 수 (slotCnt) | 워커스레드 수 X 2 / 넉넉하게                      |
| 3 | AcceptExServer라면 IOCP 워커 스레드 수　　       | 1 / 2 / 4　　　　　　　　　　                     |

**결정에서 제외한 것:** 
끊은 소켓을 `DisconnectEx`(`TF_REUSE_SOCKET`)로 재사용하는 방식은 쓰지 않는다. 
`DisconnectEx`는 정상 종료라서 서버가 먼저 끊으면 서버 쪽에 TIME_WAIT가 남는다. 서버는 TIME_WAIT를 남겨둘 수 없음.


## 2. 측정 환경
### 2-1. 장비와 OS
| 항목        | 값　　　　　　　　　　　　　　　　　　　　　                                                          |
| PC　　      | LG gram 15ZB90S (노트북)　　　　　　　　　　　　　　　　　　　                                        |
| CPU　　     | Intel Core Ultra 5 125U - P코어 2개(4스레드) + E코어 8개 + 저전력 E코어 2개 = 논리 코어 14개, 15W급　 |
| 전원        | **전원 어댑터 연결** (Win32_Battery.BatteryStatus = 2로 매 실행 전 확인), 전원 계획 "균형 조정"       |
| OS　　      | Windows 11 Pro 10.0.26200　　　　　　　　　　　　　　　　　　　　　　                                 |
| Winsock　　 | 2.2 (`WSAStartup(MAKEWORD(2, 2))`)　　　　　　　　　　　　　　　　　　　　　　                        |

- **루프백(127.0.0.1)으로 측정했다.** 이 노트북의 유선 랜은 USB Fast Ethernet(100Mbps)이라, 실제 랜으로 재면 서버가 아니라 랜카드 한계를 재게 된다.

### 2-2. 서버
- AcceptServer와 AcceptExServer는 **같은 실행 파일**이다. 
수락 방식, 워커 수, slotCnt는 실행 파일 이름으로 지정한다(예: `networkLibProject_acceptex_w4_s56.exe`). backlog는 설정 파일로 지정한다.
- AcceptServer는 블로킹 `accept()`를 부르는 수락 전담 스레드 1개로 동작한다.
 블로킹 `accept()`는 연결이 올 때까지 호출한 스레드를 멈춰 세우므로, IOCP 워커에서 부르면 그동안 다른 완료를 처리하지 못한다. 그래서 수락만 하는 스레드를 따로 둔다.

- AcceptExServer는 시작할 때 `AcceptEx`를 slotCnt개 걸어두고, 워커가 완료를 꺼낼 때마다 끊고 → `socket()` → `AcceptEx()`로 다시 걸어둔다. 
수락 완료가 송수신과 같은 IOCP로 들어오므로, 별도 스레드 없이 워커가 함께 처리한다.

- 수락한 연결은 바로 `SO_LINGER(0)` + `closesocket()`으로 RST를 보내 끊는다. 

### 2-3. 부하 클라이언트
`FloodClient_Select`: 스레드 8개 x 스레드당 소켓 64개 = **동시 진행 연결 512개**. 모든 측정에서 같은 클라를 코드 수정 없이 썼다.
```
스레드 1개의 루프 (소켓 64개)
  socket() -> SO_LINGER(0) -> 논블로킹 -> connect()
     -> select(writeSet, exceptSet, 0ms | 100ms)
         writeSet  : 핸드셰이크 성공 -> 성공 +1 -> closesocket(RST) -> 새 소켓으로 다시 connect
         exceptSet : 연결 실패(거절 등) -> 실패 +1 -> closesocket -> 새 소켓으로 다시 connect
```
- 핸드셰이크가 끝난 뒤에 끊으므로 서버 백로그에 정상 연결이 들어간다.
- 결과가 나와야 다음 시도를 하는 구조(closed-loop)다. 서버가 느려지거나 거절이 늘면 시도 수도 줄어든다.
- 클라가 RST로 끊은 연결도 서버 백로그 칸에서는 빠지지 않는다. 서버가 꺼내 가기 전까지 칸을 차지한다. 근거: 백로그 큐 크기를 측정할 때, `accept()`를 하지 않는 서버에 클라가 연결한 뒤 `closesocket()`으로 RST를 보내고 다시 연결 요청을 반복했다. 끊긴 연결이 칸을 계속 차지해 백로그 크기만큼만 성공하고 그 뒤로는 거절됐다.
- 이 클라를 쓴 이유는 앞서 쓴 부하기들의 문제 때문이다.
  - 블로킹 connect 클라: 스레드마다 연결 1개라 많은 스레드가 필요하고, 서버가 느려지면 클라도 같이 느려졌다.
  - 논블로킹 connect 후 즉시 close 클라: 핸드셰이크 전에 RST를 보내, 서버가 SYN_RECV(half-open) 상태의 연결을 만들었다 버리는 일만 반복하는 현상이 나왔음.

### 2-4. 측정 절차
서버 시작 -> 2초 -> 클라 시작 -> 3초 워밍업 -> (프로파일 리셋) -> 측정 구간 -> (프로파일 저장) -> 클라 종료 -> 서버 종료 
- 조건마다 1회 측정했다(용량 확인 2회, 워커 1개 slotCnt 56은 5분 / 3분 2회).
측정 목록:
| 측정　　　　         | 서버 설정                                                                                     | 측정 구간     | 프로파일  | 결과 절   |
| 　　　　　　         | 　　　　                                                                                      | 　　　　      | 　　　　  | 　　　    |
| 수용 용량 확인       | AcceptExServer 워커 0, backlog 200, slotCnt 300 / 100　　                                     | 10~30초　　　 | 안 함　　 | 3-1　　　 |
| backlog를 같게　　　 | AcceptServer backlog 200 / AcceptExServer 워커 1, backlog 200, slotCnt 1000　　               | 5분　　　     | 안 함　　 | 3-2　　　 |
| 용량을 같게　        | AcceptServer backlog 256 / AcceptExServer 워커 1　　                                          | 5분　　　     | 안 함　　 | 3-3　　　 |
| 용량을 같게　        | AcceptExServer 워커 2, 4, backlog 200, slotCnt 56　　                                         | 5분　　　     | 저장　　  | 3-3　　　 |
| slotCnt를 늘리면　　 | AcceptExServer 워커 2, 4, backlog 200, slotCnt 500　　                                        | 3분　　　     | 저장　　  | 3-4　　　 |
| 워커 1개 원인　      | AcceptExServer 워커 1, backlog 200, slotCnt 1000 / 56　　                                     | 3분　　　     | 저장　　  | 3-3　　　 |
| 워커 1개 원인　      | AcceptExServer 워커 1, slotCnt 500 + backlog 200 / slotCnt 200 + backlog 500, 클라 select 0ms | 3분　　　     | 저장　　  | 3-3　　　 |

### 2-5. 지표
| 지표　　　           | 뜻　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　                                                |
| 　　　　　           | 　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　                                                |
| AcceptTPS　　　　　  | 서버가 1초에 처리한 수락 수(accept 반환 수 / AcceptEx 완료 통지 수). 측정 구간의 평균, 중앙값, 최소~최대를 적는다         |
| 실패율　　           | 측정 구간의 클라 실패 수 / 시도 수(성공 + 실패)　　　　　　　　　　　　　　　　　　　　                                   |
| 실패가 난 초         | 측정 구간 중 실패가 1건 이상 난 초의 수. 실패가 일부 구간에 몰렸는지, 계속 났는지를 본다　　                              |
| SrvCPU (유저/커널)　 | 서버 프로세스 CPU 시간(`GetProcessTimes`)을 1초 단위로 차분. **논리 코어 14개 전체를 100%로 본 비율**　　　　　　　　　　 |

프로파일 태그 (QPC로 호출 직전~반환 직후를 잼):
| 태그                         | 구간　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　                                          |
| 　　                         | 　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　                                          |
| `AcceptEx_GQCS`　　          | GQCS 호출. **완료를 기다린 시간이 포함**되므로, 길면 워커가 일감을 기다리며 놀고 있다는 뜻이다                    |
| `AcceptEx_Complete`　　      | 완료를 꺼낸 뒤 처리 전체(closesocket + socket + AcceptEx 등록 포함)　　　　　　　　　　　　　　　　　　　　　　   |
| `AcceptEx_Socket`　　        | 다음 연결용 `socket()`　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　                              |
| `AcceptEx_Post`　　          | `AcceptEx()` 등록 호출　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　                            |
| `CloseReset_CloseSocket`　　 | `closesocket()`　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　                           |
| 클라 `Flood_Socket`          | 클라의 `socket()` 호출. 서버 `AcceptEx_Socket`과 같은 PC에서 같은 함수를 부르므로 비교용으로 본다　　　　　　　　 |

## 3. 측정 결과와 해석

### 3-1. AcceptExServer의 수용 용량 = backlog + slotCnt
**주장:** 서버가 한 번에 받아둘 수 있는 연결 수는 backlog + slotCnt다.

**조건**
- AcceptExServer, **워커 0개**, backlog 200, slotCnt 300 / 100. 
워커가 없으므로 완료 통지를 꺼내지 않고 소켓을 다시 걸어두지도 않는다. 걸어둔 소켓은 한 번씩만 쓰이고, 그 뒤의 연결은 백로그에 쌓이기만 한다.
- 클라가 RST로 끊은 연결도 백로그 칸에서 빠지지 않으므로, 성공 수가 멈춘 값이 서버의 수용 용량이다.

**결과**
| slotCnt | 성공 수 1회차 | 성공 수 2회차 | 별도 공간일 때 예측 (200 + slotCnt) |
| 300     | 500　　　　　 | 501　　　　　 | 500　　　　　　　　                 |
| 100     | 303　　　　　 | 306　　　　　 | 300　　　　　　　　                 |

**의미**
- 성공 수는 첫 1초 안에 위 값까지 오르고 그 뒤로 멈췄다. 이후에는 동시 진행 512개가 전부 거절됐다.
- 두 slotCnt 모두 "200 + slotCnt"와 맞는다. 

**판정:** 
 AcceptExServer의 수용 용량은 backlog + slotCnt다. 

### 3-2. backlog를 같게 두면
**주장:** backlog를 같게 두면 AcceptExServer가 AcceptServer보다 거절이 적고 처리량이 높다.

**조건**
- 양쪽 backlog 200. AcceptServer는 수락 스레드 1개, AcceptExServer는 워커 1개 + slotCnt 1000.
- 수용 용량은 AcceptServer 200, AcceptExServer 1200(3-1의 계산식). 클라 동시 연결 512개는 AcceptServer 용량보다 많고 AcceptExServer 용량보다 적다.

**결과**
| 서버                                | AcceptTPS 평균 | 중앙값       | 최소 ~ 최대             | 실패율       | 실패가 난 초        | SrvCPU (유저/커널) |
| 　　                                | 　　           | 　　　       | 　　　　                | 　　　       | 　　　　　          | 　　　　           |
| AcceptServer　　                    | 11,212　　     | 11,142　　　 | 2,998 ~ 19,749　　　　  | 0.073%　　　 | 103 / 297　　　　　 | 0.5 / 3.1　　　　  |
| AcceptExServer 워커 1, slotCnt 1000 | 15,844　　     | 16,138　　　 | 11,705 ~ 22,955　　　　 | 0.000%　　　 | 0 / 297　　　　　   | 0.7 / 5.5　　　　  |

**의미**
- AcceptExServer가 평균 1.4배, 최솟값 3.9배 높다. AcceptServer는 1초 처리량이 3천까지 떨어지는 구간이 있었고, AcceptExServer는 1만 1천 아래로 떨어지지 않았다.
- 실패는 AcceptServer에서만 났다. 용량 200이 동시 연결 512개보다 작아 백로그가 찼기 때문이다. AcceptExServer는 용량 1200이라 구조상 거절이 날 수 없었다.
- 즉 이 결과의 거절 차이는 **수락 방식보다 용량 차이**에서 왔을 수 있다. 3-3에서 용량을 같게 맞춰 다시 비교한다.
- 서버 커널 CPU는 AcceptExServer가 1.8배 높다. 처리량(1.4배)보다 더 늘었는데, 연결 1건당 시스템 콜이 더 많다는 구조(4-2)와 맞는다.

**판정:** backlog 200 조건에서는 뒷받침된다. 단 거절 차이는 용량 차이를 포함한다.

### 3-3. 용량을 같게 두면
**주장:** 걸어둔 소켓이 모자라 연결이 백로그를 거치면 AcceptExServer는 느려진다. 워커 1개면 AcceptServer보다 거절이 많지만, 워커를 2개 이상 두면 AcceptServer보다 거절이 적고 처리량도 높다.

**조건**
- 수용 용량 256으로 맞춤: AcceptServer = backlog 256, AcceptExServer = backlog 200 + slotCnt 56(3-1의 계산식).
- 클라 동시 연결 512개는 용량 256보다 많으므로 양쪽 모두 백로그가 찰 수 있다.
- AcceptServer는 수락 스레드 1개, AcceptExServer는 워커 1 / 2 / 4개.

**결과**
| 서버                         | AcceptTPS 평균 | 중앙값       | 최소 ~ 최대            | 실패율       | 실패가 난 초        | SrvCPU (유저/커널) |
| 　　                         | 　　           | 　　　       | 　　　　               | 　　　       | 　　　　　          | 　　　　           |
| AcceptServer backlog 256　　 | 10,854　　     | 11,142　　　 | 2,211 ~ 15,909　　　　 | 0.038%　　　 | 50 / 294　　　　　  | 0.5 / 3.0　　　　  |
| AcceptExServer 워커 1        | 10,215　　     | 10,668　　　 | 679 ~ 17,418　　　　   | 0.336%　　　 | 263 / 295　　　　　 | 0.5 / 3.9　　　　  |
| AcceptExServer 워커 2        | 13,455　　     | 13,821　　　 | 2,522 ~ 19,360　　　　 | 0.006%　　　 | 19 / 300　　　　　  | 0.9 / 7.7　　　　  |
| AcceptExServer 워커 4        | 12,507　　     | 12,973　　　 | 5,014 ~ 17,221　　　　 | 0.000%　　　 | 0 / 300　　　　　   | 1.1 / 8.7　　　　  |

1분 단위 초당 실패 수:
| 서버                         | 1분    | 2분    | 3분    | 4분    | 5분    |
| AcceptServer backlog 256　　 | 15.6　 | 3.5　  | 0.3　  | 0.1　  | 0.8　  |
| AcceptExServer 워커 1        | 22.4　 | 41.5　 | 34.5　 | 39.8　 | 37.9　 |

**의미**
- **워커 1개면 AcceptExServer가 AcceptServer보다 거절이 많다.** 실패율이 약 9배(0.336% vs 0.038%)다. AcceptServer는 처음 1~2분 뒤 실패가 거의 0으로 줄었지만, AcceptExServer 워커 1개는 5분 내내 초당 35~40건이 이어졌다. 처리량도 6% 낮다(10,854 vs 10,215).
- **3-2와 결과가 뒤집혔다.** 같은 워커 1개, 같은 backlog 200에서 slotCnt만 1000 → 56으로 줄이자 처리량이 15,844 → 10,215로, 실패율이 0% → 0.336%로 바뀌었다. 3-2에서 거절이 0이었던 것은 수락 방식이 아니라 slotCnt 1000만큼 용량이 커서였다.

**뒤집힌 원인 - 프로파일** (워커 1개, 3분씩 별도 측정, 호출 1회 평균, 단위 us)
| 조건                           | AcceptTPS | 실패율       | GQCS 대기 | 완료 처리 전체   | AcceptEx 등록 |
| 　　                           |           | 　　　       | 　　      | 　　　　　　     | 　　          |
| backlog 200 + slotCnt 1000　　 | 15,138    | 0%　　　     | 3.5　　   | 64.1　　　　　　 | 4.1　　       |
| backlog 200 + slotCnt 56　　   | 7,955     | 0.841%　　　 | 58.0　　  | 68.3　　　　　　 | 8.2　　       |

- **워커가 느려진 게 아니다.** 연결 1건의 완료 처리 비용은 거의 같다(64.1 vs 68.3us). 
차이는 GQCS 대기, 즉 워커가 다음 완료 통지를 기다리며 논 시간에서 난다(17배). 처리량 = 1 / (GQCS 대기 + 완료 처리)로 계산하면 14,793 / 7,918로 측정값과 맞는다.

- **백로그를 거치는 경로가 원인이다.** 용량 700(> 512)으로 고정해 거절을 없애고, 슬롯과 백로그의 비율만 바꿨다.
| 조건                          | AcceptTPS | 실패  | GQCS 대기 | 완료 처리 전체   | AcceptEx 등록 |
| slotCnt 500 + backlog 200　　 | 13,366    | 0　　 | 13.6　　  | 62.4　　　　　　 | 6.6　　       |
| slotCnt 200 + backlog 500　　 | 8,556     | 0　　 | 32.3　　  | 87.4　　　　　　 | 19.2　　      |

  - 같은 용량, 거절 0인데 슬롯이 적은 쪽이 36% 느리다. 백로그에서 연결을 꺼내 붙이는 등록 비용이 2.9배, GQCS 대기가 2.4배다.
- 정리하면, 걸어둔 소켓이 모자라 연결이 백로그를 거치면 완료 통지가 늦게 들어오고, 워커는 그동안 논다. 백로그를 거치는 연결이 늦게 들어오는 이유는 확인하지 않았다.
- 한계
  - 용량 700 비교는 클라 select 대기를 0ms로 두고 쟀다. 다른 측정은 100ms다. 클라의 select는 원래도 기다리지 않고 바로 돌아오고 있었다(16~24us).
  - slotCnt 56의 3분 측정(7,955/s, 0.841%)은 5분 측정(10,215/s, 0.336%)보다 나빠서, 측정 간 변동이 크다. 위 비교는 같은 측정 묶음 안에서만 한다.

**워커를 늘리면**
- **워커 2개부터 역전된다.** 워커 2개는 실패율 0.006%(AcceptServer의 1/6), 처리량 13,455/s(AcceptServer의 1.24배)다. 워커 4개는 실패 0, 처리량 12,507/s로 워커 2개보다 7% 낮다.
- 한계: AcceptServer는 수락 스레드 1개, AcceptExServer는 워커 2 / 4개라 스레드 수가 다르다.

**판정:** 뒷받침된다. 같은 용량이면 워커 1개 AcceptExServer는 AcceptServer보다 거절이 많고, 워커 2개 이상이면 거절이 적고 처리량이 높다. 워커 1개가 약한 원인은 처리 비용이 아니라, 백로그를 거치는 연결이 늦게 들어와 워커가 기다리는 데 있다.

### 3-4. slotCnt를 늘리면
**주장:** slotCnt를 늘리면 연결이 백로그를 덜 거쳐 GQCS 대기가 줄고 처리량이 오른다. 연결 1건의 처리 비용은 줄지 않는다.

**조건**
- 3-3과 같은 서버와 클라, backlog 200, 워커 2 / 4개에서 slotCnt만 56 → 500으로 바꿈. slotCnt 500은 용량 700이 동시 연결 512개보다 커서 거절은 구조상 나지 않는다.
- slotCnt 500은 3분, slotCnt 56은 5분(3-3의 측정). 서버 프로파일 저장.

**결과** (프로파일은 호출 1회 평균, 워커 평균, 단위 us)
| 서버                | AcceptTPS 평균 | GQCS 대기 | 완료 처리 전체    | AcceptEx 등록 | socket() | closesocket() |
| 　　                | 　　           | 　　      | 　　　　　　      | 　　          |          |               |
| 워커 2, slotCnt 56  | 13,455　　     | 43.7　　  | 104.4　　　　　　 | 16.9　　      | 31.3     | 51.5          |
| 워커 2, slotCnt 500 | 14,969　　     | 9.7　　   | 123.6　　　　　　 | 4.4　　       | 53.0     | 61.0          |
| 워커 4, slotCnt 56  | 12,507　　     | 123.6　　 | 195.6　　　　　　 | 32.4　　      | 89.7     | 67.7          |
| 워커 4, slotCnt 500 | 13,404　　     | 62.1　　  | 236.3　　　　　　 | 14.2　　      | 145.2    | 71.4          |

**의미**
- **AcceptEx 등록 비용이 줄었다.** 워커 2개에서 16.9us → 4.4us(1/4), 워커 4개에서 32.4us → 14.2us(1/2). 걸어둔 소켓이 모자라면 등록 호출이 백로그에서 연결을 꺼내 붙이는 일까지 하므로 비싸진다(4-2). slotCnt를 늘리면 그 경로를 덜 탄다.
- **처리량은 7~11% 올랐다**(워커 2개 13,455 → 14,969, 워커 4개 12,507 → 13,404).
- **완료 처리 전체 시간은 줄지 않고 늘었다.** socket()이 워커 2개에서 31.3us → 53.0us, 워커 4개에서 89.7us → 145.2us로 늘어, 등록 비용이 줄어든 몫보다 크다. 처리량이 오른 것은 GQCS 대기, 즉 워커가 일감을 기다리며 노는 시간이 줄었기 때문이다(워커 2개 43.7us → 9.7us). 처리량은 "워커 수 / (GQCS 대기 + 완료 처리)"와 네 조건 모두 맞는다.
- **워커 2개와 4개 비교:** slotCnt 500에서 처리량은 14,969 vs 13,404로 워커 2개가 10% 높다. 3-3(slotCnt 56)에서도 워커 2개가 7% 높아 같은 방향이다. 단 워커 4개는 GQCS 대기가 62.1us로 길어, 부하가 워커 4개를 채우지 못했을 수 있다.
- 워커 2개 → 4개에서 완료 처리가 2배로 느는 현상은 slotCnt 500에서도 그대로다(123.6us → 236.3us, socket() 53.0us → 145.2us). 워커를 늘렸을 때의 비용 증가는 백로그가 아니라 socket() 쪽에서 온다.
- GQCS 대기가 줄어든 것은 3-3의 결과(백로그를 거치면 완료 통지가 늦게 들어옴)와 같은 방향이다. slotCnt를 늘리니 연결이 백로그를 덜 거쳤고, 그만큼 워커가 기다리는 시간이 줄었다.
- socket()이 비싸지는 원인은 확인하지 않았다.

**판정:** 뒷받침된다. slotCnt를 늘리면 연결이 백로그를 덜 거쳐 처리량이 오른다. 연결 1건의 처리 비용은 줄지 않으므로, slotCnt는 처리 비용을 줄이는 수단이 아니라 연결이 백로그로 가지 않게 하는 용량이다(5절 결론 2). 워커는 slotCnt 500에서 2개가 4개보다 처리량이 10% 높다(5절).

## 4. 구조적 차이 - 레퍼런스로 정리

### 4-1. AcceptEx가 하는 일
- `AcceptEx`는 accept + 주소 조회 + 첫 수신을 한 번의 커널 진입으로 처리한다. TCP 핸드셰이크와 accept를 합치는 것이 아니다. [1]
- 수신 크기를 0으로 넘기면 연결이 도착하는 즉시 완료된다. 이 서버는 0으로 넘긴다. [1]
- 걸어둔 `AcceptEx`가 있으면 연결은 백로그에 남지 않고 걸어둔 소켓으로 바로 간다. [2]
- 걸어둔 `AcceptEx`를 다 쓰면 연결은 백로그 큐에 쌓인다. 거절은 백로그까지 차서 backlog + slotCnt를 넘은 뒤에 시작된다(3-1).

### 4-2. 수락 경로 비교
```
AcceptEx - 걸어둔 소켓이 있을 때
  socket() -> AcceptEx() 등록 ... [핸드셰이크 완료] -> 커널이 즉시 걸어둔 소켓에 붙임 -> 완료 통지 -> 워커가 꺼냄
                                   (서버 스레드를 기다리지 않음)

AcceptEx - 걸어둔 소켓을 다 썼을 때
  [핸드셰이크 완료] -> 백로그 대기 ... socket() -> AcceptEx() 등록 -> 백로그에서 꺼내 붙임 -> 완료 통지

accept
  [핸드셰이크 완료] -> 백로그 대기 ... accept() (커널이 소켓 생성까지)
```

연결 1건당 서버가 부르는 시스템 콜:
| 방식               | 호출                                                     |
| 　　               | 　　                                                     |
| AcceptServer　　   | `accept()` + `closesocket()`　　                         |
| AcceptExServer　　 | `GQCS` + `closesocket()` + `socket()` + `AcceptEx()`　　 |

- AcceptExServer는 시스템 콜이 더 많다. 이점은 호출 수가 아니라 **처리 시점**에 있다. 소켓 생성과 등록을 연결이 도착하기 **전에** 해 두기 때문에, 연결이 도착한 순간에는 서버 스레드를 기다리지 않는다.
- 걸어둔 소켓이 부족하면 연결이 백로그에서 기다려야 하므로 AcceptServer와 같은 구조가 된다. 측정에서는 이 경로를 탈수록 처리 비용보다 **완료 통지를 기다리는 시간**이 더 크게 늘었다. 워커 1개에서 완료 처리 비용은 거의 같았고(64.1 vs 68.3us) GQCS 대기가 17배였다(3-3).

## 5. 결론

| # | 결정　　　　        | 선택　　　　　　　　　　　　　               | 근거　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　　                                            |
| 1 | 연결 수락 방식      | AcceptExServer　　　　　　　　　　　　　　　 | 3-3: 같은 용량에서 워커 2개 이상이면 실패율이 낮고 처리량이 높다　　　　　　　　　　　　　　　　　　                              |
| 2 | slotCnt　　　　　　 | 동시 접속 예상치 이상으로 넉넉하게           | 3-1, 3-3, 3-4: 걸어둔 소켓이 백로그 앞의 용량이 되고, 연결이 백로그를 거치면 거절이 없어도 완료 통지가 늦어진다　　               |
| 3 | IOCP 워커 수　　　  | 2개 (지금 부하 기준)　　　　　　　　         | 3-3, 3-4: 워커 1개는 같은 용량에서 AcceptServer보다 거절이 많고, 2개면 거절이 거의 없다. 4개의 이점은 이 부하로는 확인되지 않았다 |

**1. AcceptExServer를 쓴다.**
AcceptExServer가 항상 빠른 것은 아니다. 연결 1건당 시스템 콜은 오히려 더 많다(4-2). 
이점은 소켓 생성과 등록을 연결이 오기 전에 해 둔다는 것이고, 그 이점은 걸어둔 소켓이 남아 있을 때만 나온다. 
 걸어둔 소켓이 적어도(slotCnt 56, AcceptServer와 같은 용량) 워커 2개부터 AcceptServer보다 처리량이 1.24배, 실패율이 1/6이었다(3-3).

**2. slotCnt는 넉넉하게 둔다.**
걸어둔 소켓은 백로그와 별도인 용량이다(3-1).
 연결이 백로그를 거치면 완료 통지가 늦게 들어와 워커가 기다리게 된다. 
 거절이 없는 같은 용량(700)에서도 슬롯이 적은 쪽(slotCnt 200 + backlog 500)이 36% 느렸다(3-3). 
 그래서 백로그가 아니라 걸어둔 소켓이 먼저 연결을 받아내도록 둔다. 

## 참고 자료

**[1] AcceptEx 레퍼런스**
https://learn.microsoft.com/en-us/windows/win32/api/mswsock/nf-mswsock-acceptex
> The AcceptEx function combines several socket functions into a single API/kernel transition. ... A new connection is accepted. Both the local and remote addresses for the connection are returned. The first block of data sent by the remote is received.

> If dwReceiveDataLength is zero, accepting the connection will not result in a receive operation. Instead, AcceptEx completes as soon as a connection arrives, without waiting for any data.

**[2] MSDN Magazine 2000.10, Windows Sockets 2.0: Write Scalable Winsock Apps Using Completion Ports (Windows 2000 Networking group)**
https://learn.microsoft.com/en-us/archive/msdn-magazine/2000/october/windows-sockets-2-0-write-scalable-winsock-apps-using-completion-ports
> If there are no accept operations pending, the event will be signaled by an incoming connection.

- 첫 문장의 event는 `FD_ACCEPT`, 즉 "백로그에 꺼내 갈 연결이 생겼다"는 신호다. 이 신호가 걸어둔 AcceptEx가 **없을 때만** 뜬다는 것은, 걸어둔 AcceptEx가 있으면 연결이 백로그에 남지 않고 그 소켓으로 바로 간다는 뜻이다.
