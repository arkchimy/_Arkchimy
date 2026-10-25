# 작업 노트 (세션 시작 시 자동으로 읽힘)

취업 포트폴리오용 IOCP 네트워크 라이브러리(C++, Windows) 저장소.
`README.md`는 포트폴리오 첫 화면(채용 담당자용)이므로 작업 메모는 이 파일에 적는다.

## 세션 시작 시
1. 이 파일의 **진행 상황**과 **다음 할 일**을 확인한다.
2. `git status`로 커밋 안 된 변경이 있는지 본다.
3. 작업을 마치면 **진행 상황**, **다음 할 일**, **애로사항**을 갱신한다.

## 작업 방식 (사용자 선호)
- 답변과 코드 주석은 한국어. 주석은 기존 코드처럼 "왜"를 짧게, 줄 끝 `//` 주석 위주.
- **커밋 전에 커밋 문구를 먼저 보여주고 승인받은 뒤 커밋**한다. 성격이 다른 변경은 커밋을 나눈다(chore/feat/refactor 등 접두어 + 한국어).
- push는 요청할 때만.
- 끝에 `Co-Authored-By` 줄을 붙인다(시스템 안내의 형식을 따름).
- 변경 후에는 빌드 + 에코 테스트로 확인하고 결과를 숫자로 보고한다.

## 구조
| 경로 | 내용 |
|---|---|
| `networkLibProject/` | 라이브러리(`NetworkLib`, `RingBuffer` 등) + 서버 `main.cpp`. `networkLibProject.sln` |
| `EchoClient/` | 에코 **정확성** 검증 클라이언트(무작위로 잘라 보내기, 바이트 검증, 비정상/넘침/탐침 모드). `EchoClient.sln` |
| `LoadClient/` | **처리량** 측정용 부하 클라이언트(32B 메시지 `overSend`개 일괄 송신 → 전부 오면 다음 묶음, 순번만 확인). `LoadClient.sln` |
| `_lib/` | 공용: `ProtocolLib/PacketHeader.h`(서버·클라 공용 헤더), `StatLib`(`StatUtil` CPU·1초 타이머, `LatencyHistogram`, 서버·클라가 소스로 포함), `ParserLib`(cfg), `ProfilerLib`, `ExeNameLib` |
| `tools/` | `echo_test.ps1`(자동 에코 테스트), `send_ctrlc.ps1`(백그라운드 서버에 Ctrl+C) |
| `networkLibProject/docs/`, `실험폴더/` | accept vs AcceptEx 실험 기록과 결론 |

## 빌드와 테스트
```powershell
$msbuild = "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe"
& $msbuild networkLibProject\networkLibProject.sln /p:Configuration=Release /p:Platform=x64 /m /v:minimal /nologo
& $msbuild EchoClient\EchoClient.sln /p:Configuration=Release /p:Platform=x64 /m /v:minimal /nologo

# 서버 + 클라 → N초 → 클라 종료(세션 0 확인) → Ctrl+C로 서버 정상 종료 확인
.\tools\echo_test.ps1 -Name 이름 -RunSec 20
# 정상 + 비정상 패킷 클라 동시 실행
.\tools\echo_test.ps1 -Name 이름 -RunSec 15 -ClientCfgs client.cfg,client_badPacket.cfg
# "폴더/cfg"면 그 폴더의 클라 실행 - 부하(LoadClient) + 탐침(EchoClient)
.\tools\echo_test.ps1 -Name 이름 -RunSec 15 -ClientCfgs LoadClient/client.cfg,client_probe.cfg
```
- LoadClient 빌드: `& $msbuild LoadClient\LoadClient.sln /p:Configuration=Release /p:Platform=x64 /m /v:minimal /nologo`
- 서버는 작업 폴더(`networkLibProject/`)의 `server.cfg`, 클라는 작업 폴더의 `client.cfg`를 읽는다.
- 서버 종료는 Ctrl+C(콘솔 키 입력은 제거됨). EchoClient는 아직 `Q` 키라 테스트에선 강제 종료한다.
- 합격 기준: 클라 `unexpectedClose=0`(불일치가 나면 클라가 `__debugbreak`), 클라 종료 후 서버 `세션=0`, `[main] 종료 완료` 출력.

## 애로사항 (환경)
- **PowerShell 5.1**: `&&`, `||`, 삼항 연산자 없음. 네이티브 명령 인자에 큰따옴표가 있으면 깨진다 → `git commit -m`에 `"`가 들어가면 실패. **메시지를 파일로 써서 `git commit -F 파일`**.
- **PowerShell 스크립트(.ps1)는 BOM 있는 UTF-8**이어야 한다. BOM이 없으면 한글을 ANSI로 읽어 파싱 오류가 난다.
- **C++ 소스·헤더도 BOM 있는 UTF-8**. BOM이 없으면 MSVC가 CP949로 읽어 C4819 경고와 엉뚱한 컴파일 오류가 난다(새 파일 `PacketHeader.h`에서 겪음). 새 파일을 만들면 BOM을 붙인다. 기존 파일은 이미 BOM이 있다.
- **vcxproj/filters도 BOM 있는 UTF-8**. 수정할 때 인코딩을 유지한다. 파일 이름을 바꾸면 `.vcxproj`와 `.vcxproj.filters` 둘 다 고친다. 포함 경로는 Debug/Release 두 곳.
- Bash(Git Bash)에는 `jq`, `python`, `node`가 없다. JSON 확인 등은 PowerShell `ConvertFrom-Json`.
- 빌드 마지막의 `'pwsh.exe'은(는) 내부 또는 외부 명령...이 아닙니다`는 빌드 후 이벤트 문제로 결과물과 무관.
- `Start-Process -PassThru`로 띄운 프로세스의 `ExitCode`가 빈 값으로 나올 수 있다 → 종료 확인은 출력 로그(`종료 완료`)로 한다.
- 백그라운드 서버에는 키 입력을 보낼 수 없다 → Ctrl+C 종료 + `send_ctrlc.ps1`(AttachConsole + GenerateConsoleCtrlEvent)로 해결.
- **cfg 파일은 BOM 없는 UTF-8**이어야 한다(소스와 반대). `ParserLib`가 BOM을 키로 읽어 `설정 파일 오류 err=5`. PowerShell 5.1의 `Set-Content -Encoding UTF8`은 BOM을 붙이므로 `[IO.File]::WriteAllText(..., New-Object Text.UTF8Encoding($false))`로 쓴다.

## 애로사항 (설계·코드 함정)
- `Session`은 `std::mutex`를 가져 이동 불가 → `vector::resize` 대신 `vector(n)`을 만들어 `swap`.
- I/O 카운트는 `WSARecv`/`WSASend` **전에** 올린다. 완료가 먼저 처리돼 0으로 떨어지는 것을 막는다.
- 슬롯 재사용 ABA: 세션 ID = (세대 << 32) | 슬롯. 외부는 ID로 `findSession`(acquire 후 ID 비교).
- `start`/`stop`은 `mStartStopLock`으로 감싼다(TOCTOU 방지). **워커나 콜백 안에서 `stop()`을 부르면 데드락**(자기 자신을 join) → 워커는 `mRunning=false`로 요청만 하고 main이 `stop()`.
- AcceptEx로 받은 소켓은 `SO_UPDATE_ACCEPT_CONTEXT`를 해야 `getpeername`/`shutdown`이 동작한다.
- 수신 링(16KB) > 최대 패킷(4098B)을 `static_assert`로 보장 → 완성 패킷을 매번 다 꺼내므로 수신 링이 가득 찰 수 없다.
- 비정상 길이 헤더는 패킷 경계를 잃은 것이므로 페이로드를 기다리지 말고 즉시 끊는다.
- EchoClient: 목표량을 넘긴 뒤 **패킷 경계에서** 송신을 멈춰야 한다. 패킷 중간에서 멈추면 서버가 마지막 패킷을 에코하지 않아 연결이 끝나지 않는다.
- EchoClient: 생성기는 상태가 있어 되돌릴 수 없으므로, `send`가 일부만 보낸 나머지는 `mPending`에 보관한다.
- EchoClient의 `MAX_INFLIGHT`는 서버 송신 링(16384)에서 **제어 패킷 자리 1KB를 뺀 15360**. 링 크기 그대로 쓰면 핑이 링에 끼어 있을 때 에코가 못 들어가 넘침으로 끊긴다(핑 도입 후 `unexpectedClose=2`로 발견).
- 서버 핑은 송신 링이 가득 차면 **연결을 끊지 않고 그 핑만 버린다**(`건너뜀` 통계). 측정 때문에 정상 연결이 끊기면 안 된다. `Data`만 넘침 시 끊는다.
- EchoClient `maxInflight`는 **최대 패킷(4099B) 이상**이어야 한다. 작으면 큰 패킷을 중간까지 보내고 멈추는데 서버는 미완성 패킷을 에코하지 않아 교착(퐁도 경계에서만 나가서 같이 막힘). `init`에서 거부.
- **`OnRelease` 재진입**: 카운트를 0으로 만든 스레드가 바로 해제하면, 컨텐츠 스레드의 `SendPacket`/`Disconnect` 안에서 `OnRelease`가 불려 컨텐츠 락 데드락이 날 수 있다 → 0이 되면 `PostQueuedCompletionStatus`로 해제 작업(`eIoType::Release`)을 넣어 **항상 워커에서** 해제.
- **수락 처리 중 카운트 0 문제**: 첫 수신 전에는 카운트가 0이라 `OnAccept` 안의 `SendPacket`이 실패하고, 수신이 바로 실패하면 `OnRelease`가 `OnAccept`보다 먼저 올 수 있다 → 수락 처리 몫으로 카운트를 잡고 `OnAccept` → `postRecv` → 반납.
- **끊기 경쟁**: `CancelIoEx`만으로는 부족(취소 직전 성공 완료가 수신을 다시 건다) → `mDisconnect` 플래그로 새 I/O를 막고, `WSARecv`/`WSASend`를 건 **뒤에 플래그를 다시 확인**해 걸린 I/O를 취소한다. `postRecv`/`postSend`는 호출하는 쪽이 카운트를 잡고 있어야 한다(건 뒤 세션을 다시 읽으므로).
- **파생 클래스 소멸자에서 `stop()`** 필수: 기반 소멸자 시점엔 파생 부분이 없어 `OnRelease`가 순수 가상 호출이 된다.
- `SendPacket`은 헤더와 페이로드를 따로 push하므로 `getFreeSize()`로 둘 다 들어갈 때만 넣는다(하나만 들어가면 스트림이 깨짐).

## 진행 상황 (2026-10-24 기준)
- 아래는 모두 커밋·push 완료
- AcceptEx 수락, 세션 풀, 에코, 통계, 세션 ID, `SO_UPDATE_ACCEPT_CONTEXT`, start/stop 락, Ctrl+C 종료 (~`5af45d5`)
- 패킷 경계 처리 (`0835c56` 서버, `5e83380` EchoClient), `tools/` 테스트 스크립트와 이 `CLAUDE.md` (`c0b0e1c`)
  - `SendRingBuffer` → `RingBuffer` 일반화(peek, getWriteBufs, moveWrite)
  - `_lib/ProtocolLib/PacketHeader.h`(길이 2바이트, 최대 페이로드 4096)
  - 수신 링으로 받아 완성 패킷 단위 에코, 비정상 길이면 끊기(`비정상패킷` 통계)
  - EchoClient 패킷 스트림 검증 + `badPacket` 모드(`client_badPacket.cfg`)
  - 검증: 패킷 에코 256연결 20초 3.3GB 통과, 정상 256 + 비정상 64 동시 15초에서 비정상 약 6.4만 건 끊김 + 정상 2.1GB 통과, 누수 없음
- 라이브러리/컨텐츠 분리 (`7958524`)
  - 설계 결정: 상속(가상 함수) / 콜백은 IOCP 워커에서 직접 / `OnRecv(id, payload, len)`은 원시 포인터(직렬화 버퍼 때 교체)
  - `NetworkLib`: `OnAccept`/`OnRecv`/`OnRelease` 순수 가상, `SendPacket(id, payload, len)`(헤더를 라이브러리가 붙임), `Disconnect(id)`, 소멸자 virtual
  - `EchoServer`(새 파일): 에코를 컨텐츠로 이동. `main`은 `EchoServer` 생성
  - `client_noRecv.cfg` 추가(송신 링 넘침 테스트)
  - 검증: 에코 20초 3.2GB, 정상+비정상 헤더 15초(비정상 4.6만 끊김), 정상+받지 않는 클라 15초 2회(넘침 끊김 425/584) 모두 `unexpectedClose=0`, 세션 0 복귀, Ctrl+C 정상 종료

## 진행 중: 지연(RTT) 측정 - 서버가 별도 핑을 보내 왕복 시간을 잰다 (2026-10-25 설계 확정)
- 왜 서버 핑인가: 서버 시계만 쓰므로 클라가 다른 PC여도 시계 동기화가 필요 없다.
- 헤더에 `kind` 1바이트 추가(`Data`/`Ping`/`Pong`, 헤더 3바이트). Ping/Pong은 라이브러리가 처리하고 `OnRecv`에는 `Data`만. 모르는 kind는 비정상 패킷.
  - "헤더에 type을 넣지 않는다"는 이전 결정과 충돌하지 않음: 이건 컨텐츠 패킷 종류가 아니라 라이브러리 제어 메시지 구분용.
- 핑 페이로드 `{순번 uint32, t0 int64(서버 QPC)}`, 클라는 같은 페이로드로 Pong. t0 = `SendPacket` 시점(송신 큐 대기 포함 = 체감 지연), t1 = 수신 완료를 꺼낸 직후(GQCS 반환).
- 퐁 인정 범위: `마지막 받은 퐁 순번 < 순번 ≤ 마지막 보낸 핑 순번`. **최신 핑의 퐁만 받으면 RTT > 간격일 때 표본이 전부 버려져 결과가 좋게 나온다.**
- 핑은 **전용 타이머 스레드**가 보낸다. 워커에 타이머를 맡기면(GQCS 타임아웃) 워커가 멈췄을 때 핑도 안 나가 **정작 큰 지연이 측정에서 빠진다** → 측정기는 측정 대상(워커)과 독립이어야 한다.
- 나눠 보내기: 틱 10ms, K = 간격/틱, 틱 카운트 k = tick % K, 슬롯 `idx = k*S; idx += K*S`(S = 표본 비율). `% S == 0` 표본을 그대로 `% K`로 나누면 K·S 공약수 때 한 조각에 몰림.
  - 고해상도 대기 타이머(`CREATE_WAITABLE_TIMER_HIGH_RESOLUTION`) + 절대 시각 기준. 늦게 깨면 놓친 틱을 최대 K개까지 따라잡는다.
  - 핑 트래픽 = 세션 수 ÷ 간격 → 전체 트래픽의 ~1% 이하가 되도록 간격·표본 비율 조절.
- RTT 집계: 워커별 히스토그램(2의 거듭제곱마다 64칸, 오차 ~1.6%, 단일 쓰기), 매초 p50/p99/p99.9/최대.
- 클라(EchoClient): 수신을 패킷 단위로 파싱, Ping이면 퐁을 **보내던 데이터 패킷의 경계에서만** 끼운다(패킷 중간에 끼우면 스트림이 깨짐). RTT에는 클라 처리 시간이 포함됨.
- 단계: ① 헤더 kind ② EchoClient 패킷 단위 수신 + 퐁 ③ 서버 핑·히스토그램·통계 ④ 측정 → **모두 완료, 커밋·push (`d44a299` 서버, `999ecf9` EchoClient, 2026-10-25)**
  - 설정: `server.cfg`의 `pingIntervalMs`(0이면 끔)/`pingSampleMod`, EchoClient `idle`/`maxInflight`, `client_idle.cfg` 추가
  - 회귀(핑 켠 상태): 에코 2.1GB, 비정상 헤더 3.2만 끊김, 넘침 322 끊김 - 정상 클라 모두 `unexpectedClose=0`
- 첫 측정 (루프백, 노트북, 1회씩, 핑 100ms, 워커 2):

  | 조건 | 에코 처리량 | RTT p50 | p99 | p99.9 | max |
  |---|---|---|---|---|---|
  | 무부하 64연결 | - | 431µs | 1.07ms | 2.9ms | 16ms |
  | 에코 256, inflight 15360 | 163MB/s | 13.1ms | 25.6ms | 32.0ms | 45ms |
  | 에코 256, inflight 8192 | 150MB/s | 12.4ms | 24.3ms | 28.7ms | 36ms |
  | 에코 256, inflight 4099 | 123MB/s | 9.9ms | 18.2ms | 23.0ms | 32ms |
  - 부하 중 RTT는 세션 큐 깊이에 비례할 거라 봤으나 inflight를 3.7배 줄여도 RTT는 25%만 줄었다 → **서버 큐가 아니라 다른 곳(EchoClient의 select 루프 한 바퀴 시간, 바이트 단위 검증 비용 등)이 지배하는 것으로 추정, 미확정.**
  - 무부하 431µs도 루프백치고 크다(클라 select 경로, 스레드 스케줄링 추정, 미확정).

## RTT 분해 (2026-10-25, 커밋 `525fca4` StatLib 이동, `05bca6c` EchoClient)
- 추가: EchoClient 매초 `cpu코어=사용코어/워커수`, **탐침 모드**(`probeIntervalMs`, `client_probe.cfg`) - 데이터 없이 직접 핑을 보내 자기 시계로 RTT. 부하 클라와 동시에 띄우면 "자기 큐·루프는 한가한 연결"이 겪는 지연 = 서버·커널 몫
- `init` 인자를 `EchoClientConfig`로 묶음, 퐁 대기열 → `mCtrlQueue`(퐁 + 탐침 핑)
- 측정 (논리 코어 14, 노트북, 1회씩, 탐침 64연결 10ms):

  | 조건 | 서버 CPU(코어) | 부하 클라 CPU | 탐침 RTT p50 / p99 | 부하 세션 RTT(서버 측정) p50 / p99 |
  |---|---|---|---|---|
  | 탐침 단독 | - | - | 1.3ms / 2.7ms | - |
  | 부하 256 + 탐침, 워커 2 | 2.0 (14.6%) | 3.95/4 | 5.1ms / 8.5ms | 12.5ms / 25.9ms |
  | 부하 256 + 탐침, 워커 4 | 3.0~3.5 (21~25%) | 3.9/4 | 2.5ms / 3.1ms | 5.6ms / 16.9ms |
- 해석
  - **워커 2는 서버가 포화**(2코어 = 워커 2개 전부 바쁨) → IOCP 완료 큐 대기가 모든 지연에 더해졌다. 워커 4로 늘리자 탐침 p99 8.5→3.1ms, 부하 세션 p50 12.5→5.6ms.
  - **부하 클라도 포화**(4스레드에 3.9코어) → 에코 처리량(~160MB/s)은 워커를 늘려도 그대로 = **지금 처리량 숫자는 클라 한계**. 부하 세션 RTT의 남은 몫도 클라 루프 + 세션 큐 대기.
  - 서버 CPU는 대부분 커널(유저 ~2%, 커널 ~20%) - 라이브러리 코드보다 루프백 TCP 처리 비용이 크다.
- 미해결: 탐침 단독 p50 1.3ms(서버 핑으로 잰 무부하 431µs보다 큼) - 64개 핑이 동시에 나가는 몰림, 유휴 스레드 깨어나는 지연(전원 상태) 추정, 미확인
- 테스트 스크립트가 클라를 강제 종료해서 클라의 "탐침 RTT 전체" 요약은 안 찍힌다(매초 줄로 판단)

## 처리량 측정용 LoadClient (2026-10-25, `5808e7e` LoadClient, `2433d61` 서버 카운터·CPU 출력, `f1b4325` load_sweep.ps1)
- EchoClient가 먼저 포화돼 서버 처리량을 잴 수 없어서 만든 부하 전용 클라. 사용자 결정: **일괄 방식**(묶음이 전부 와야 다음 묶음), 메시지 32B 하나
  - 메시지 = 헤더 3 + 페이로드 29(앞 8바이트 순번). 묶음을 `send` 한 번으로 보낸다. `overSend` 최대 480(묶음이 서버 송신 링 15360B에 들어가야 함)
  - 송신 버퍼에는 항상 완성된 패킷만 있어 서버 핑의 퐁을 뒤에 붙여도 경계가 안 깨진다
  - closed-loop라 **처리량 전용**. 지연은 탐침으로 잰다
- 서버: 워커별 `WorkerCounter`(64바이트 정렬, 단일 쓰기)로 `수신패킷/s` 출력 - 클라 숫자와 교차 확인
- `echo_test.ps1`: `-ClientCfgs`에 "폴더/cfg"를 쓰면 그 폴더의 클라 실행
- 첫 실행(256세션, overSend 16, 워커 2): 클라 50만 메시지/s = 서버 수신 50만 패킷/s, 클라 CPU 2.7/4(여유), **서버 2.0코어(워커 2 포화)** → 이제 서버 한계를 잴 수 있다

- 서버 통계 CPU 표시를 `cpu코어=사용코어/워커수 (커널 %)`로 변경(PC 전체 대비 %는 14코어 기준이라 작게 보여 읽기 어려웠음)

## 워커 수 측정 (2026-10-25, LoadClient 256세션 overSend 16 + 탐침, 15초, 중앙값, 1회씩)
| 워커 | 패킷/s | 서버 코어 | 클라 코어 | 탐침 p50 / p99 |
|---|---|---|---|---|
| 1 | 39만 | 1.02/1 | 1.67/4 | 5.9 / 13.2ms |
| 2 | 64만 | 1.98/2 | 2.94/4 | 4.5 / 7.4ms |
| 4 | 71만 (2회차 95만) | 3.97/4 | 3.45/4 | 4.5 / 6.5ms (2회차 3.6 / 5.3) |
| 8 | 103만 | 6.98/8 | 3.66/4 | 3.7 / 7.2ms |
| 8, 클라 2개(512세션) | 100만 | 6.33/8 | 2.2 + 2.3 | 4.9 / 8.2ms |
- 모든 조건 `unexpectedClose=0`, 넘침 0, 커널 비율 ~90%
- 해석
  - 워커 1→2는 1.6배, 그 뒤로는 증가 폭이 작다. 코어당 처리량이 39만 → 32만 → 18~24만 → 13만으로 떨어진다.
  - **약 100만 패킷/s에서 정체**: 워커 8 + 클라 2개에서 서버(6.3/8)도 클라(2.2, 2.3)도 포화가 아닌데 더 안 오른다 → 서버·클라가 같은 PC의 CPU(사용 합계 ~11/14코어, 하이브리드 P/E코어)와 루프백 커널 처리를 나눠 쓰는 **PC 전체 한계**로 추정. 서버 단독 한계는 클라를 다른 PC로 옮겨야 알 수 있다.
  - **회차 편차가 크다**(워커 4: 71만 vs 95만) → 숫자를 문서에 쓰려면 전원 연결·전원 모드 고정·3회 중앙값 필요.
  - LoadClient는 closed-loop라 항상 서버를 포화까지 밀어붙인다 → 탐침 RTT(수 ms)는 "포화 상태의 지연". 평소 부하의 지연은 **전송률을 제한하는(open-loop) 부하**로 따로 재야 한다.

## 워커 수 측정 - 환경 고정, 3회 중앙값 (2026-10-25, `tools/load_sweep.ps1 -Tag best_perf`)
- **정리 문서: `networkLibProject/docs/throughput_workers_2026_10_25.md`**, 원본 CSV: `실험폴더/LoadClient_workers_2026_10_25/raw_runs.csv`
- 환경: 전원 연결, 전원 모드 **최고 성능**(측정 후 균형 조정으로 복원), Core Ultra 5 125U(P 2코어/4스레드 + E 8 + 저전력 E 2 = 14스레드), 회차마다 실행 순서를 돌림
- `load_sweep.ps1`: 조건 "워커:LoadClient수" × 회차, `server.cfg`를 임시로 바꿨다가 finally에서 복원, 회차별 CSV(`%TEMP%\arkchimy_echo_test\load_sweep_<Tag>.csv`) + 조건별 중앙값 표
| 워커 | 클라 | 패킷/s 중앙값 (범위) | 서버 코어 | 클라 코어 | 탐침 p50 / p99 | 부하 세션 p50 / p99 |
|---|---|---|---|---|---|---|
| 1 | 1 | 38.1만 (37.6~38.2) | 1.00/1 | 1.63/4 | 6.1 / 12.8ms | 8.3 / 17.4ms |
| 2 | 1 | 54.0만 (48.0~57.7) | 2.01/2 | 2.72/4 | 5.5 / 8.7ms | 6.5 / 12.5ms |
| 4 | 1 | 72.7만 (71.7~79.1) | 4.02/4 | 3.53/4 | 4.5 / 6.3ms | 4.3 / 8.2ms |
| 8 | 1 | **109.9만** (101.4~111.8) | 7.05/8 | 3.62/4 | 3.8 / 6.0ms | 3.3 / 11.4ms |
| 8 | 2 | 98.1만 (95.6~102.5) | 6.26/8 | 4.34/8 (2.17+2.17) | 5.0 / 8.4ms | 6.0 / **106~180ms** |
- 클라 CPU는 "사용 코어 / LoadClient 워커 스레드 수"(LoadClient 1개 = 워커 4개). 클라 2개 조건은 두 프로세스 합. 탐침은 제외
- 15회 모두 `unexpectedClose=0`, 넘침 0
- 해석
  - 환경 고정 후 편차 감소: 워커 1은 ±1%, 워커 8은 ±5%. 워커 2만 ±9%로 여전히 흔들림.
  - 코어당 처리량 38만(워커 1) → 27만(2) → 18만(4) → 16만(8). 워커 1개만 성능 코어를 독차지하고, 늘수록 효율 코어·스레드 경합이 섞이는 것으로 추정.
  - 워커 8에서 클라를 2개로 늘리면 오히려 처리량이 줄고(110만 → 98만), 부하 세션 p99가 100ms를 넘는다(탐침 p99는 8ms 그대로). 서버 8 + 클라 8 + 탐침 스레드가 14스레드(그중 빠른 건 4)를 넘게 나눠 써서, **스케줄되지 못한 클라 스레드의 세션이 수십 ms씩 퐁을 못 보내는** 것으로 추정 → 이 조건의 부하 세션 RTT는 클라 스케줄링 지연이 섞인 값
  - 결론: 이 PC 루프백에서 32B 메시지 **약 110만 패킷/s**(워커 8, 클라 1)가 측정 가능한 최대. 그 이상은 서버·클라가 같은 CPU를 나눠 쓰는 환경 한계(다른 PC 사용 불가 - 사용자 확인)
- 애로사항: `powercfg /getoverlayscheme`는 이 버전(25H2)에서 "매개 변수가 잘못되었습니다" - 현재 모드는 레지스트리 `HKLM\SYSTEM\CurrentControlSet\Control\Power\User\PowerSchemes`의 `ActiveOverlayAcPowerScheme`로 읽는다. 설정은 `powercfg /overlaysetactive <GUID>`(최고 성능 `ded574b5-45a0-4f42-8737-46345c09c238`, 균형 `00000000-0000-0000-0000-000000000000`)

## 다음 할 일 (RTT 이어서)
1. LoadClient에 목표 전송률(open-loop) 옵션 → 부하 대비 지연 곡선(포화 전 지연과 한계점). 함정(Nagle 등) 논의
2. 처리량 개선 실험(선택): 시스템 콜당 메시지 수(overSend), 워커 스레드를 성능 코어에 고정(affinity)했을 때의 차이
2. 하트비트 타임아웃(퐁이 안 오면 끊기)

## 다음 할 일 (그 뒤)
3. 직렬화 버퍼 + 참조 카운트, 송신 큐. 암호화는 **세션별 키(A안)**로 결정 → 평문 패킷은 참조 카운트로 공유, 송신 담당이 보낼 때 세션 키로 암호화하며 송신 버퍼에 씀(순서 보장 위해). 키 교환(ECDH)·암호(AES-GCM)는 미확정
4. 데모(채팅 서버) + 부하 테스트 → README 첫 화면용 수치
5. README를 포트폴리오 구조로 정리(요약, 구조도, 설계 결정과 근거, 트러블슈팅, 검증 방법, 한계)

## 확인할 것 (미해결)
- 정상 + 받지 않는 클라 동시 테스트에서 1회차만 정상 클라 처리량이 8초부터 약 180 → 45MB/s로 떨어짐(끊김·불일치는 없음). 2회차와 분리 전 커밋(`c0b0e1c`)에서는 재현 안 됨 → 노트북 환경 편차로 추정하나 미확정. 측정 환경을 고정한 뒤 다시 볼 것.
- 패킷 에코로 바꾼 뒤 에코 처리량이 약 215MB/s → 175MB/s로 감소. 클라의 바이트 단위 생성 비용 또는 서버의 패킷당 복사 2회(링 → 임시 버퍼 → 송신 링) 추정, 미측정. 3번 작업 때 프로파일로 확인.
- 키 입력 제거로 서버 프로파일 저장(`Profile::Save`) 방법이 없음 → 측정할 때 종료 시 자동 저장 추가.

## 포트폴리오 기록거리 (README에 옮길 것)
- accept vs AcceptEx 결론, slotCnt 결정 (`networkLibProject/docs/`)
- 부하 클라이언트의 측정 왜곡(closed-loop, 즉시 close RST) 발견과 수정
- 세션 ID로 ABA 방지, start/stop TOCTOU를 락으로 해결
- 헤더에 type을 넣지 않은 이유(라이브러리는 자르기만, 종류는 컨텐츠)
- 수신 링 크기를 `static_assert`로 보장해 "링 가득 참" 처리를 없앤 것
- 검증 방법: 시드 기반 패킷 스트림을 무작위로 잘라 보내기, 패킷 경계 종료, 정상·비정상 클라 동시 접속
- 라이브러리/컨텐츠 분리에서 찾은 동시성 함정 3가지(`OnRelease` 재진입 → 워커로 위임, 수락 중 카운트 0, 끊기 플래그 재확인)
- "라이브러리는 메커니즘, 정책은 컨텐츠": 콜백 스레드 정책(단일 스레드 처리 등)을 라이브러리가 강제하지 않음
- RTT 측정 설계: 서버 핑(시계 동기화 불필요) / 측정기는 워커와 독립 스레드(워커가 멈추면 측정도 멈추는 문제) / 최신 핑만 인정하면 느릴 때 표본이 사라지는 문제 / 나눠 보내기(표본과 조각의 공약수 함정) / 핑이 송신 링 자리를 차지해 정상 연결이 끊긴 버그와 해결
- 측정 결과 해석: "큐 깊이에 비례할 것"이라는 가설이 맞지 않음 → 병목 위치를 다시 찾는 과정
- 탐침 클라로 RTT 분해: 서버·클라가 **둘 다 포화**였음을 CPU 코어 환산으로 확인, 워커 2→4에서 탐침 p99 8.5→3.1ms. "처리량 숫자가 클라 한계"임을 밝혀 측정 도구부터 개선해야 한다는 결론
