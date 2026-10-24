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
| `EchoClient/` | 에코 검증 클라이언트. `EchoClient.sln` |
| `_lib/` | 공용: `ProtocolLib/PacketHeader.h`(서버·클라 공용 헤더), `ParserLib`(cfg), `ProfilerLib`, `ExeNameLib` |
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
```
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
- EchoClient의 `MAX_INFLIGHT`(16384)는 서버 송신 링 크기와 맞춘 값 - 이 이하로 유지하면 서버 링이 넘치지 않는다.
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

## 다음 할 일
1. 직렬화 버퍼 + 참조 카운트(브로드캐스트 시 1회 생성), 송신을 패킷 포인터 큐 + WSABUF 묶음으로. `OnRecv`/`SendPacket`의 원시 포인터 인자를 패킷 클래스로 교체
2. 데모(채팅 서버) + 부하 테스트 → README 첫 화면용 수치
3. README를 포트폴리오 구조로 정리(요약, 구조도, 설계 결정과 근거, 트러블슈팅, 검증 방법, 한계)

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
