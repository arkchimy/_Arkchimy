# 작업 노트 (세션 시작 시 자동으로 읽힘)

취업 포트폴리오용 IOCP 네트워크 라이브러리(C++, Windows) 저장소.
`README.md`는 포트폴리오 첫 화면(채용 담당자용)이므로 작업 메모는 이 파일에, 에러 기록은 `README_ERRORS.md`에 적는다.

## 세션 시작 시
1. **`README_ERRORS.md`(에러와 해결 방법)를 읽는다** - 아래 줄로 자동으로 함께 불러온다. 같은 에러를 다시 겪지 않게 작업 전에 확인한다.
2. 이 파일의 **진행 상황**과 **다음 할 일**을 확인한다.
3. `git status`로 커밋 안 된 변경이 있는지 본다.
4. 작업을 마치면 **진행 상황**, **다음 할 일**, **애로사항**을 갱신하고, 새로 겪은 에러는 `README_ERRORS.md`에 형식(증상 → 원인 → 해결 → 재발 방지)대로 추가한다.

@README_ERRORS.md

C++ 코드는 아래 규칙(포프 C++ 표준 + 이 프로젝트의 해석·예외)을 따른다 - `_lib/NetworkLib/`, `networkLibProject/`, `_lib/ProtocolLib/`가 대상
@CODING_STANDARD.md

## 로그인 서버 (2026-11-15) - 먼저 읽을 것
- **설계**: `portfolio/design/login_server.docx`(판 이력 없이 지금 구조만, 남은 결정 9-1 범위 = 1→2→3 순서, 9-2 시험 = 표대로, 둘 다 사용자 "추천대로"). 옛 `login_server.md`는 1판이라 지울지 사용자 결정 대기
- **1단계 완료(미커밋)**: 로그인 서버 `Game/Login/`(포트 33002, `server.cfg` loginPort) - 가입·로그인, 인증 스레드 = `DbThread` 재사용, Argon2id 1.3 + 솔트 + 페퍼(`PasswordHash`, 없는 아이디도 가짜 확인 한 번), 한 세션 5번 실패 끊기. 세션 키 32바이트를 Redis `session:<AccountNo>`(값 = 키 + 아이디, EX 30)에. Redis 클라 직접 구현 `Game/Server/RedisClient`(AUTH·SET EX·GET·DEL, 끊기면 재연결, DEL은 재시도 안 함). 게임 서버 `LoginReq` = AccountNo + 증명(3단계에서 바뀜 - 세션 키에서 파생한 증명 키로 AccountNo + "enter" + EnterChallenge의 키 MAC, BLAKE2b 16바이트) → DB 스레드가 GET → 확인 → DEL 1이면 통과(확인 전엔 지우지 않음). 공용 `Game/Protocol/LoginProtocol.h`·`SessionAuth`·`LoginClient`. 클라 `register`/`login <id> [pw]`(pw 생략 = 시험용 `test-password`), 시험용 `auth`/`enter <no> <키> [bad]`. 봇은 처음 한 번 가입, 인증실패 통계(판정)
- **Argon2id 설정 측정(이 PC, `LoginServer.exe --bench`)**: 64MiB·2회 중앙값 63ms·최대 90ms - "최대도 100ms 안"에서 메모리가 가장 큼 → login.cfg 기본값. (128MiB·1회 83/104ms, 46MiB·4회 89/133ms)
- **준비물**: libsodium 1.0.22 MSVC(`C:\_Deps\libsodium-1.0.22`, 공식 minisign 서명 검증함, DLL판 - Debug /MDd·Release /MT 때문), 경로는 `Game/libsodium.props`(환경 변수 `SODIUM_DIR`). Redis 3.0.504(사용자 바탕 화면, `REDIS_DIR`), 시험이 127.0.0.1·requirepass·저장 끔으로 띄우고 끈다(비밀번호는 임시 conf 파일로 넘기고 지움). 비밀은 `db.cfg`(redisHost·redisPort·redisPassword 추가)·`Game/Login/login.cfg`(pepper·argonOps·argonMemMiB, .gitignore, 예시 `login.cfg.example`)
- **시험**: `run_all.ps1` 8개(새 `login.ps1` 14판정 포함) PASS, `New-TestAccount`가 `LoginServer.exe --hash test-password`로 credentials도 만든다. soak에 로그인 서버 메모리·Redis 키 수·인증실패 판정 추가. 솔루션 Release·Debug 경고 0, 규칙 위반 0
- **cpp-reviewer 리뷰 반영(2026-11-15)**: ① Redis가 멈추거나 꺼지면 DB 스레드(저장도 함)가 몇 초씩 막힘 → 비블록 connect + select 1초, 응답 1초, 연결 실패·시간 초과 뒤 5초는 바로 실패. 입장·인증 대기열 상한(게임 DB 대기 500, 로그인 인증 대기 200이면 바로 거절) ② GET과 DEL 사이 같은 계정의 새 로그인이 키를 덮어쓰면 새 키가 지워짐 → `DelIfEquals`(EVAL로 같은 값일 때만 DEL) ③ 5번째 실패 응답 뒤 끊기 전에 다음 요청이 들어오던 틈 → 끊은 뒤 표시를 내림 ④ 비밀번호 지역 사본·Redis 송신 버퍼·응답의 세션 키 지우기 ⑤ 규칙: return 하나(authOnAuthThread), out 매개변수 Assert, `SessionAuth`는 배열 참조, auto 제거, 범위 `–`(출력 문자열은 CP949라 ASCII `-`), 주석 정정(가입 응답이 아이디 존재를 알려 주므로 로그인 쪽 시간 맞추기는 "비밀번호 맞혀 보기"만 막는다, 입장 실패 셋은 키가 이미 쓰여 로그인 서버부터 다시)
- **알고 둔 한계(리뷰, 고치지 않음)**: 실패 5번 제한은 연결마다라 다시 접속하면 그만(계정·주소별 제한 없음) / 입장 증명이 연결에 묶이지 않아(nonce 0, 서버 도전 값 없음) 가로챈 LoginReq를 먼저 보내면 들어온다 - **2단계에서 서버 nonce나 채널 묶기를 AD에 넣는 것 검토** / 가입 INSERT 중 연결이 끊기면 가입됐는데 Exists로 답할 수 있다 / 로그인 서버 이전의 `accounts` 행은 그 아이디로 먼저 가입한 사람이 가져간다(기존 DB를 쓸 때 옮기는 방법 필요) / 아직 게임에 들어오지 않은 계정은 운영 툴로 차단 못 함(NotFound) / 라이브러리 수신 링·Redis 응답 버퍼의 비밀번호·키 사본은 지우지 않는다 / 게임 서버의 Redis 확인이 저장과 같은 DB 스레드(나누면 계정을 올리는 순서를 다시 정해야 함)
- **2단계 완료(로그인 구간 암호화)**: `Game/Protocol/SecureChannel`(`ChannelCipher` - 방향별 키, 순번 논스, XChaCha20-Poly1305 / `AcceptClientHello`·`FinishClientHandshake` - 양쪽 임시 X25519(crypto_kx), 서버가 (서버 임시 공개 키 + 클라 임시 공개 키)에 고정 Ed25519로 서명). 로그인 서버 첫 패킷 = ClientHello, 그 뒤 암호문만, 응답은 세션 `SendLock` 안에서 Seal + SendPacket. 서버 키는 `LoginServer.exe --genkey` → 비밀 키 `login.cfg signSecretKey`, 공개 키 `Game/Login/server_public.cfg`(.gitignore, 예시 있음, 클라·봇 빌드 때 복사). `LoginClient`는 `LoginServerAddr`(IP·포트·공개 키) + `eLoginCall`(Ok·NoConnection·BadServer·Broken). 클라 시험 명령 `authrepeat`·`authbad`·`pinkey`
- **3단계(게임 구간 암호화, 구현·시험 중)**: 게임 서버가 OnAccept에서 `EnterChallenge`(16바이트) → 입장 증명 AD = AccountNo + "enter" + challenge(가로챈 LoginReq를 다른 연결에서 못 씀 - 리뷰 의심 3 해결). 하위 키: 증명 1, 클라→서버 3, 서버→클라 4. LoginRes Ok를 평문으로 보내고 같은 `Player` 송신 락 안에서 통로를 연다. 게임 서버의 모든 송신은 `sendGame`(송신 락 안에서 세션 ID 확인 → Seal → SendPacket - 칸이 재사용돼도 새 세션 순번을 건드리지 않게), 입장 전 평문은 LoginReq만 받는다. 클라·봇은 EnterChallenge를 받은 뒤 입장, 입장 뒤 송수신 암호화. 클라 시험 명령 `tamper`. login.ps1 21판정 PASS
- **문서 반영(2026-11-15)**: 설계 `login_server.docx`에 11장 "구현 결과와 리뷰"(9장은 "정한 것"), 옛 `login_server.md`는 지움. 구조 문서 `game_architecture.md` 9장 "로그인 서버와 암호화"(10–12장 번호 밀림) + 같은 내용 docx, 흐름 문서 `게임서버_흐름.docx` 3판(⓪ 로그인 서버, ① EnterChallenge, ② DB 스레드 입장 확인·통로, 함수 링크 156개 - 생성은 scratchpad `make_flow_doc.py` + `link_flow_doc.py`(Game/Login 포함)라 다음 세션엔 없다), README(로그인·암호화 줄, 준비물, 경로 표)
- **경계 시험(2026-11-16)**: cpp-reviewer가 설계한 TC-L01–L12를 `tools/game/login_edge.ps1`(판정 42개, 약 4분, LOCK TABLES로 DB 스레드를 붙잡음 - run_all에는 넣지 않음)로. 클라 시험 명령 `enterdump`(입장 패킷을 보내지 않고 16진수로)·`authflood <n> <id>`. 찾은 문제 둘을 고침: ① 접속 후 첫 패킷 기한이 없어 빈 연결로 세션 칸을 다 채울 수 있었다 → 로그인 서버 `loginIdleSec`(기본 10, 요청 중 제외, 통계 `기한끊김`), 게임 서버 `entryLimitSec`(기본 60, 입장 처리 중 제외, 통계 `입장기한끊김`), 공용 `Game/Server/IntervalThread.h` ② MySQL 연결이 끊긴 뒤 첫 읽기가 다시 하지 않아 실패(게임 서버는 세션 키를 지운 뒤) → `DbConn::Query`가 끊김이면 다시 붙어 한 번 더(읽기 전용, 트랜잭션은 Exec). 주석 정정(입장 증명은 키 MAC, DbError는 키가 남을 수도). 시험 도구: Redis 포트를 db.cfg 값으로, 끈 뒤 포트가 놓일 때까지 기다림. 남은 것(고치지 않음): 게임 서버 입장 실패 횟수 제한 없음(기한 60초로 시간만 묶임), 클라가 끊긴 인증 요청도 해시를 끝까지 계산
- **로그인·암호화 포함 1시간 장시간 시험 PASS(2026-11-15 21:40–22:40)**: 판정 13개, 로그인 460번·인증실패 0, 예상밖끊김 0, 메모리 54MB 그대로, Redis 키 최대 0 - `game_soak.md` 5장 셋째 절, 원본 `실험폴더/game_soak_2026_11_15/1h_login_bot30/`

## 게임 서버 진행 상황 (2026-11-14) - 먼저 읽을 것
- **계획한 기능 구현 완료**(`portfolio/design/게임서버_설계.docx`): 로그인·인벤토리 → 파티 → 던전(CSV 시나리오, 10초 턴 타이머, 전리품·장비) → 경매장(`76bd064`) → MySQL 저장 → 운영 툴. 커밋은 `git log`로
- **MySQL**: DB 스레드 1개 + FIFO 대기열(`Game/Server/DbThread`), 바뀔 때마다 저장(`GameDB`, 트랜잭션 = 데이터 + `logs` 한 줄), 저장 요청은 메모리를 바꾼 플레이어 락·경매장 락 안에서 넣는다. 처음 보는 아이디의 로그인은 `HoldSession` → DB 스레드 → `UnholdSession`. 재시작 때 열린 경매 취소·환불. 끊기면 다시 연결(COMMIT 전 실패만 재실행). 접속 정보는 `Game/Server/db.cfg`(.gitignore, `db.cfg.example` 참고) - 이 PC는 MySQL 서버 9.4, 클라 라이브러리는 `C:\Program Files\MySQL\MySQL Server 8.0`(다르면 환경 변수 `MYSQL_DIR`)
- **운영 툴**: 게임 서버 안 운영자 포트(`AdminServer`, 127.0.0.1:33001, db.cfg `adminKey`, 5초 인증 기한) + `Game/Admin/AdminTool.exe`(recall·ban·unban은 서버로, log·item은 DB 직접). 이 작업 중 라이브러리 버그(객체가 둘이면 워커 혼동, README_ERRORS 2-13)를 고쳤다 - 포트폴리오 사례 후보
- **장시간 시험**: `.\tools\game\soak.ps1 -Minutes 60 -Bots 30` - `GameBot.exe`(계정마다 스레드, 3명 한 조로 던전·경매·장비·2–6분마다 스스로 끊고 재접속)를 돌리며 1분마다 서버 통계(메모리·핸들·세션·로그인·계정·파티·경매·DB대기·DB실패)를 `%TEMP%\arkchimy_game_test\soak\samples.csv`에 모으고, 끝나면 봇 예상밖끊김·프로토콜오류 0, DB실패 0, 세션 0, 정상 종료, 골드 = 로그 합계, 메모리 증가(예열 뒤→마지막) 기준 이하를 판정. **1시간·5시간(봇 30개) PASS**(2026-11-15, 5시간: 던전 23,986판, 메모리 53–54MB, 핸들 705–710, DB대기 최대 1, 수락 = 해제 2,254) - 결과 `portfolio/verification/game_soak.md`, 원본 `실험폴더/game_soak_2026_11_15/`
- **구조 문서**: `portfolio/game_architecture.md`(라이브러리 `architecture.md`와 같은 형식 - 장마다 결론·선택·버린 안), 흐름 문서 `게임서버_흐름.docx`는 기준 커밋 `dbf326c`로 갱신(모니터 스레드, 시작 실패 출력, 구조 문서 연결). 생성 스크립트는 세션 scratchpad의 `make_flow_doc.py`라 다음 세션엔 없다 - 고칠 때는 Word를 직접 고친다
- **시험 방법**: `.\tools\game\run_all.ps1`(빌드 후, db.cfg 필요) - 경매·재시작·DB 재연결·운영 툴·이중 로그인·4명 던전·회복약 7개를 PASS/FAIL로 판정. 시험마다 필요한 계정을 DB에 직접 만들어(`New-TestAccount`) 서로 기대지 않고 몇 번이든 다시 돌릴 수 있다. 공통 함수는 `tools/game/GameTestCommon.ps1`. DB를 비우려면 `DROP TABLE logs, auctions, items, accounts;`(다음 시작 때 다시 만든다)
- **문서 반영 완료(2026-11-14)**: 설계 9판(3-11 MySQL 6개, 3-12 운영 툴 4개, 테스트 케이스 표), 포트폴리오 Word에 6장 "사용 예: 텍스트 TRPG 던전 + 경매장 서버"·4-4 사례(서버 객체 둘과 워커 판단)·1장 문구·저장소 표 추가(기존 문단을 틀로 복사해 끼워 넣음, 사용자 확인 대기)
- **남은 한계**: 회수가 던전 중 장비도 벗김(운영 예외), `TakeItemFromAnyone`이 계정 표 락을 쥔 채 전체 순회, 회수·저장 실패 시 메모리와 DB 불일치는 `DB실패` 카운터로만 보인다, 비밀번호 없음·계정 생성 무제한·계정을 메모리에서 내리지 않음(데모 범위 - 남의 아이디로 로그인 가능, 부계정 골드), 리슨 소켓에 `SO_EXCLUSIVEADDRUSE` 없음(같은 사용자 프로세스가 포트를 나눠 가질 수 있음 - 재시작 bind 실패 위험이 있어 적용 안 함). 전체 리뷰(2026-11-14, 1차 + 재검증)에서 고친 것: 로그인 중 표시를 계정보다 먼저 읽고 표시를 내린 뒤 응답(한 세션이 두 계정에 로그인돼 첫 아이디가 재시작까지 잠기던 경쟁), 남은 턴 시간, DB 생성은 시작 때만, 이름 규칙. 약어 DB·SQL은 대문자(`GameDB`, `MakeRecallSQL`, CODING_STANDARD에 추가)
- 커밋 안 된 사용자 파일: `portfolio/iocp_network_library_portfolio.docx`(지울지 사용자 결정)
- 덤프 분석: `python tools\dump_info.py <덤프>`(디버거 없이 dbghelp.dll). `Echo/Server/Dump_20261112_...dmp`는 bind 10048(포트 사용 중)으로 분석 끝(README_ERRORS 1-33)

## 다음 세션에서 이어서 할 일 (2026-11-03 인수인계, PC B → 노트북 PC A)
- **(2026-11-11, PC A) 작업 API 삭제 → `HoldSession`/`UnholdSession`(사용자 결정, `portfolio/design/db_job.md` 2-10)**: 작업 완료를 IOCP로 넘기면 워커마다 따로 처리돼 같은 세션의 완료 순서가 보장되지 않는다. 라이브러리는 "워커 밖 스레드가 세션을 쓰는 동안 해제하지 않기"만 제공하고 DB 큐·스레드·순서·결과 처리는 모두 컨텐츠가 맡는다(라이브러리에 DB·Job이라는 말이 없다). `AcquireSession`·`PostJobComplete`·`OnJobComplete`·`JobOverlapped.h`·`eIoType::Job`·`JobPerSession`·`echoJobMs` 삭제, `mJobCnt` → `mHoldCnt`(오용 확인용, `crashOnHoldMisuse`). 확인: DLL Release·Debug 경고 0, 규칙 위반 0. 실행 시험은 새 에코 서버 뒤. README·`make_portfolio_docx.py` 문구는 고쳤고 Word 파일은 다시 생성해야 한다. `server.cfg`의 `chatRoomCnt`·`chatIdleSec`·`chatJoinSec`은 지운 채팅 서버 설정이라 새 서버 때 정리
- **(2026-11-11, PC A) 에코·채팅 서버와 클라이언트·시험 도구를 지움 - 테스트 케이스를 쓰며 처음부터 다시 만든다(사용자 결정)**: 지운 것 - `networkLibProject/EchoServer.h`·`ChatServer.h`·`main.cpp`, `_lib/ProtocolLib/ChatProtocol.h`, `EchoClient/`, `LoadClient/`, `tools/echo_test.ps1`·`mixed_test.ps1`·`load_sweep.ps1`. 남긴 것 - 라이브러리 전부(작업 API 포함, DB 작업은 에코 서버의 시험 모드만 지우기로 함), 서버 프로젝트 껍데기(sln·vcxproj·`NetConfigLoader`·`server.cfg`), `send_ctrlc.ps1` 등 범용 도구, 측정 데이터(`실험폴더/`, `portfolio/`). **서버 exe는 `main`이 없어 지금 링크되지 않는다** - DLL은 `NetworkLib.vcxproj`를 직접 빌드(Release·Debug 경고 0). 순서: 에코 → 채팅 → 테스트 케이스 → 오목. 이 파일·README·portfolio의 옛 명령과 도구 이름(echo_test 등)은 지금 없는 것이다. 옛 코드는 `75a3091`에서 복구 가능
- **(2026-11-10, PC A) 포트폴리오 구성 2판과 송신 정체 판정 수정** - 일주일 기한, 라이브러리 설계가 뼈대(결정·이유·버린 안), 오목 대전 서버(MySQL)는 사용 예, 핵심 성과 표 삭제, 고민 사례 3개(송신 경로·세션 수명·무응답 클라), 검증은 테스트 케이스 방식(`portfolio/verification/test_cases.md` 초안, 사용자와 따로 논의). 전체는 `portfolio/design/portfolio_structure.md` 2판. 용어: 수신 윈도우, 무응답 클라, 워커 밖 스레드의 송신. 송신 정체 판정 수정(README_ERRORS 2-12): 워커 밖 스레드가 맡길 때 `mWsaSendQPC = INT64_MAX`. 다음은 `architecture.md` 다시 쓰기 또는 오목 서버 시작(사용자 결정)
- **(2026-11-10, PC B → 다른 기기로 인수인계) 먼저 읽을 것 - 남은 결정·다음 할 일**
  - **(결정됨 2026-11-14)** `.def`의 LIBRARY 이름은 바꾸지 않고 "서버와 DLL은 항상 같은 커밋으로 같이 빌드한다"로 주석을 고침(사용자: 추천대로)
  - **마지막 변경 뒤 시험 안 함(사용자 결정):** `mJobCnt`를 `PostJobComplete`에서 내리도록 바꾼 뒤에는 빌드(경고 0·규칙 위반 0)만 했다. 다음에 에코 `echoJobMs = 1` 시험 한 번 돌려 볼 것
  - **다음 할 일(사용자와 정한 순서):** ① 위 결정 ② 작고 확실한 개선 - `SendPacket` 직후 `Disconnect`해도 메시지가 나가게(강퇴 사유), `TCP_NODELAY`, 설정값 검증(`WorkerCnt`·`MaxSession`·`SlotCnt`) ③ 안정성 검증 - 장시간(1–3시간)·접속 폭주 시험, 남은 정확성 확인(작업 수 한도, 오용 시 멈춤, `Stop` 진단, 외부 스레드 송신과 끊기 경쟁) ④ 세션별 암호화(ECDH + AES-GCM, 송신 링에 넣을 때 세션 키로) ⑤ MySQL 서버(로그인 AccountNo, 중복 로그인 막기, 로그아웃 저장 - 가짜 DB는 쓰지 않는다, 성능 측정 없이 기능으로)
  - **사용자 방향(2026-11-10):** 포트폴리오 중심은 네트워크 라이브러리 - 검증·개선을 먼저, 서버는 그 위에 빨리 만든다. 성능 측정은 이미 충분해 같은 종류를 더 하지 않는다. **직렬화 버퍼·공유 송신은 제안하지 않는다**(세션별 암호화 때문에 세션별 송신 링으로 정함)
- **(2026-11-09–10, PC B) 라이브러리 마무리 - 워커 수 원인 확인 + IOCount 기반 작업 API**: ① `portfolio/design/worker_scaling.md` - 워커를 늘리면 느려지는 원인 = **IOCP 동시 실행 수**(동시에 꺼내는 워커가 많으면 꺼낸 완료가 줄어 덜 모음). 워커 4·동시 2 = 1048만 ≈ 워커 2 1049만, 워커 4 기본 884만(`실험폴더/worker_conc_2026_11_09/`). PC 전체 CPU는 워커 2에서 84%, 3 이상 89–94%. 결정: `iocpConcurrency` 설정만 추가하고 기본값 유지(워커 2, 0=코어 수) - 워커를 동시 실행 수보다 많이 둘 이유(블록되는 컨텐츠)가 아직 없음. `load_sweep` 모드 `+conc<N>`, PC 전체 CPU 열 추가. ② `portfolio/design/db_job.md` 2-9 - DB 같은 블록 작업: **`AcquireSession(id)`**(ID 확인 + IOCount +1 + `mJobCnt` +1, `jobPerSession` 기본 4를 넘으면 실패) / **`PostJobComplete(id, JobOverlapped*)`**(`mJobCnt` -1, 음수면 오용으로 멈춤, 컨텐츠 작업 객체의 OVERLAPPED로 PQCS) / 가상 **`OnJobComplete(id, JobOverlapped*)`**(워커, 끝나면 IOCount -1). 새 공개 헤더 `JobOverlapped.h`(`eIoType`·`MyOverlapped`·`JobOverlapped`), 작업 객체는 `JobOverlapped`를 상속. 처음에는 세션별 작업 칸 + 비트마스크로 만들었다가 사용자 지적("IOCount 기반으로 하자고 했는데")으로 다시 짰다. 짝 맞추기는 컨텐츠 책임(결정 C), **`OnJobComplete`는 같은 세션의 다른 콜백과 동시에 불릴 수 있다**(결정 ①), 끊기 요청은 받았지만 아직 해제되지 않은 세션에는 `AcquireSession` 성공(이어 걸기는 컨텐츠가 하지 않음). 오용(ID·처리 전 재투입·Post가 Acquire보다 많음·`OnJobComplete` 미재정의)은 릴리즈에서도 `crashOnJobMisuse`로 멈춤, `Stop` 10초 초과 시 `stderr` 진단. 같은 계정 재접속은 컨텐츠의 AccountNo 표(중복 로그인 막기, 지울 때는 로그아웃 저장 완료 뒤). 시험(`mJobCnt` 이동 전 구현): 에코 `echoJobMs` 켬·끔 각 3종 + `jobPerSession = 1` + Debug, 섞인 클라 1회 통과. cpp-reviewer 2회
- **(2026-11-09 저녁, PC B) 포트폴리오 측정 2건 - 문서 반영 완료(미커밋일 수 있음)**: ① 세션 카운트 변경 A/B(`a6f6374`와 번갈아 3회, `실험폴더/refcount_ab_2026_11_09/`) - 채팅 방 16 워커 2 1025만 vs 1052만/s, 워커 4 882만 vs 889만, 에코 136만 vs 137만, 모두 범위 겹침 → **처리량 차이 없음**(경합이 적어 CAS가 첫 시도에 성공, 둘 다 lock 명령 하나). `portfolio/cases/session_lifetime.md` 5장·README 사례 4번에 반영. ② **비용 분해 PC B 재측정**(`실험폴더/cost_breakdown_2026_11_09/`, 스크립트는 PC B 경로·VS 18·서버 대기와 재시도 추가, 변형 base·coarse만) → `architecture.md` 7장·README 비용 줄 교체. `OnRecv` 약 60ns/전달 일정, `WSASend` 1번 약 7us ÷ 모은 메시지 수. 워커 2 기본(WSASend당 64): `WSASend` 56%·`OnRecv` 33%, 많이 모음(256): 33%·53%, 워커 4(30개, 꺼낸 완료 2.6): 일하는 시간의 72%가 `WSASend`, 27%는 대기. **발견: 프로파일러를 켜면 더 모이고 더 빠르다**(워커 2 끈 5회 56–93개·1031만–1148만/s, 켠 5회 64–256개·1090만–1768만/s) - "느려지면 완료가 쌓여 더 모은다"(4-10)는 방향의 근거. 측정 중 Chrome이 11–34% 쓴 회차가 있어 워커 2는 Chrome 끄고 5회 다시 잼. 첫 측정은 서버 재기동 `Access is denied`(1-17)로 중간에 멈춤 → 대기·재시도 추가 후 처음부터. 다음: 첫 성능 사례 = "워커를 늘리면 모으기가 깨져 처리량이 준다"(4-10) 후보, 사용자와 결정
- **(2026-11-09, PC B) 세션 카운트를 해제 표시 비트 방식으로(사용자 구현, cpp-reviewer·직접 검토 3판)**: `mIoCount`를 `volatile int64_t`로, 63번 비트(`RELEASE_FLAG`, `Session.h`)가 "해제 작업이 들어갔고 다음 수락 전까지 아무도 잡으면 안 됨". 초기값 = 표시, `sessionUnLock`이 0을 만들면 `postReleasePQCS`가 CAS(0 → 표시)에 성공한 한 스레드만 해제 작업을 넣음, `sessionLock`(옛 `tryIncIoCount`)은 무조건 +1 후 표시가 보이면 **되돌리지 않고** 실패, 수락 때 `InterlockedExchange64(..., 1)`로 표시를 지우며 수락 몫을 잡는다(새 ID를 쓴 뒤). 검토에서 깨졌던 판: 올린 뒤 1이면 되돌리기(몫 없이 true), 표시를 지우는 곳 없음, 빈 슬롯 초기값 0(감시·`Stop`이 가짜 해제 → 빈 슬롯 번호 중복·`Stop` 무한 대기), `allocSession`에서 0 쓰기(수락 준비 중 해제) - **표시를 지우는 시점은 수락(새 ID 뒤)이어야 하고 `releaseSession`에서 0으로 되돌리면 안 된다**. 함께: `postRecv` 즉시 실패에 `requestDisconnect`(0이 되는 모든 경로가 끊기 플래그를 먼저 세움), 공개 `GetIdx` 삭제(ChatServer 안 private `getIdx`로), `WsadataRAII`의 `IsOk`/`GetErr` 삭제(실패 시 `__debugbreak`, main의 확인 삭제), 공개 헤더 주석 일부 삭제(사용자 결정). 시험(PC B, Release): 에코 회귀 3종(4,137MB, 비정상 6.3만, 넘침 192, 정상 끊김 0), 송신 정체 `sendStallSec = 3` + `sendBufZero = 1` 64건·기본 버퍼 0, 채팅 방 16(1,030만/s), 방 옮기기 워커 2·4(5.1만·4.2만 번, 넘침 0), Debug + 비정상 헤더 Assert 없음, 섞인 클라 3회 통과(최대 지연 22–33ms). 처리량 비교는 안 함. 남은 지적(고치지 않음): `tlsSendTargets`(규칙은 `t`), `sessionLock`/`sessionUnLock`·`static CAS` 이름, 틀린 주석(`allocSession`, `NetworkLib.h` "decIoCount", `Session.h` "disconnectFlag"), 공개 헤더에서 지운 콜백·`Stop` 약속. 이제 VS 18(2026)로 연다 - MSBuild 경로 `...\Microsoft Visual Studio\18\Community\...`, 도구 집합은 v143 그대로. stash(`GetIdx 비static 실험`)는 `GetIdx`가 삭제돼 쓸모없음 - 지울지 사용자 결정
- **(2026-11-08, PC B) `a6f6374`까지 받음**: 세 sln Release·서버 Debug 빌드 경고 0, 규칙 위반 0, `NetworkLib.dll` 생성 확인. 서버 실행·회귀 시험은 아직 안 함. PC B에 있던 커밋 안 된 사용자 변경(`GetIdx`를 `static`이 아닌 멤버로, `findSession`은 직접 형 변환)은 받은 코드의 주석 정리와 충돌해 `git stash`(`PC B 로컬: GetIdx 비static 실험`)에 보관 - 살릴지 버릴지 사용자 결정
- **(2026-11-07, PC A, 구현 완료, 커밋 `424247b`) 워커가 아닌 스레드의 `SendPacket`** - 사용자 제안. 설계 문서 `portfolio/design/external_send.md`, 시험 `실험폴더/external_send_2026_11_07/`. 외부 스레드는 `mSendLock` 안에서 `mbSendFlag`를 잡고 `mSendOv`를 0바이트 완료로 PQCS. 확인: 빌드 경고 0, 외부 스레드 시험 5회, 회귀 3종(정상 끊김 0, 세션 0, 정상 종료). **안 한 것**: 섞인 클라, 방 옮기기, Debug 서버 실행, 외부 스레드 방송 + 끊기 경쟁, PC B 처리량(`1166e17`과 번갈아)
- **(2026-11-07, PC A) 라이브러리 헤더 나누기(사용자 결정, `portfolio/design/source_layout.md`)**: 함수 내용은 그대로 두고 헤더만 공개 `NetworkLib.h`·`NetConfig.h`·`NetStat.h`, 내부 `Session.h`·`IoOverlapped.h`·`WorkerCounter.h`로 나눔. **cpp는 `NetworkLib.cpp` 하나로 유지**(기능별 분리는 시도 후 사용자 결정으로 되돌림). 서버가 `Session` 정의를 보지 않도록 `NetworkLib` 생성자를 선언하고 `.def`로 내보냄. 확인: 서버 sln Release·Debug 빌드 경고 0, 규칙 위반 0, 원본 cpp와 줄 대조. **사용자 결정으로 서버 실행·회귀 시험·처리량 비교는 아직 안 함** - 처리량을 비교하려면 PC B에서 `1166e17`과 번갈아 잰다. 같은 날 코드 검토에서 찾은 항목은 고치지 않고 테스트 때 다룬다: `TCP_NODELAY` 미설정, `SendPacket` 직후 `Disconnect`하면 메시지가 안 나감, 넘침 통계 중복 가능(`marshalToSendBuf`), `postRecv` 즉시 실패 경로에 `requestDisconnect` 없음, `WorkerCnt`·`MaxSession`·`SlotCnt` 검증 없음, `mIP`·`mPort`를 읽는 곳 없음
- **상태(2026-11-05 저녁, PC B)**: **`/O2` 재측정 완료**(미커밋일 수 있음 - git status 확인). 결과와 표는 `실험폴더/portfolio_o2_2026_11_05/결과.md` - 인용은 이 문서로. 방향은 대부분 유지, 크기 변경: skip −82% → −61%/−51%, 모아 보내기 +57–58%, 방 16 12.7–12.9배. **묶음 단위 skip 켬 +31–35%는 재현 안 됨(+0.5–2.9%, 차이 없음)**, 기아 최대 지연은 33–68ms → 14–15ms로 수 초 기아 재현 안 됨, 4-10 재현(다른 프로세스 탓 아님). 사용자 결정(2026-11-05): 근거를 전부 재측정 값으로 교체 - README, `portfolio/cases/broadcast.md`, `portfolio/verification/mixed_clients.md`, 초안 문서 반영함. 기본값 유지 근거는 처리량이 아니라 송신 완료 대기 제거·지연 p99, README 첫 문장은 −61%. 재측정 못 한 값(지연 주입 끊김, 문맥 전환, 비용 분해, 송신 버퍼 0, `wsasend_cost`)은 "재측정 안 함"으로 표시. 다음은 문서 검토(사용자) → 넘침 컨텐츠 제약 → 암호화. 아래 줄의 "재측정" 계획은 이것으로 끝남
- **(2026-11-06, PC B) `tbDeferSend` 제거(사용자 결정 - 타이머·로직 스레드 없는 구조)**: `SendPacket`은 워커에서만(`tWorkerIdx < 0`이면 `false`), 워커 루프는 묶음 끝에서 항상 `flushSends`, `completeRecv`는 `mergeSends = 0`일 때만. 검증: Release·Debug 경고 0, 규칙 위반 0, `mergeSends` 0·1 × 에코·비정상 헤더·받지 않는 클라(정상 끊김 0, 넘침 127–192)·방 옮기기, Debug + 비정상 헤더 Assert 없음, 섞인 클라 2회 통과. 임시 빌드 시험: GQCS·GQCSEx × `mergeSends` 0·1 모두 `OnAccept` 송신이 수신 없이 도착, 다른 스레드는 `false`(수정 전 빌드는 `mergeSends = 0`에서 거부). 방 옮기기 워커 4 넘침은 번갈아 8회씩 수정 후 1회 76건, 수정 전 1회 106건으로 양쪽 모두 가끔 난다(회귀 근거 없음, 8회라 작은 차이는 판별 못 함). 처리량 비교는 안 함. Visual Studio가 열려 있으면 파일 저장 뒤 `vcpkgsrv`·`cl`이 코어를 써서 측정이 오염된다 - 통계 줄이 2–13줄로 줄었고, 그때 넘침 386건이 나왔으나 재현 안 됨
- **(다음 세션 먼저 읽을 것, 2026-11-06 사용자 결정) 멀티스레드 성능 측정은 PC B(i5-8600, 6코어 6스레드, 하이브리드 아님)에서만 한다.** PC A(Core Ultra 5 125U)는 성능 코어와 효율 코어가 섞여 있어(P 2코어 4스레드 + E 8 + 저전력 E 2) 워커 스레드가 어느 코어에서 도느냐에 따라 숫자가 달라지므로 워커 수·스레드 확장 비교에 부적합하다. PC A에서는 끊김·순서 같은 정확성 시험과 문서 작업만 한다. **다음 작업은 PC B에서 이어서:** ① `git pull`(추적 해제된 `실험폴더`의 `.exe` 154개가 그 PC에서 지워진다 - 옛 실험을 돌리려면 해당 커밋에서 복구) ② 측정 전 CPU 상위 프로세스 확인(README_ERRORS 1-24), Visual Studio 닫기 ③ `실험폴더/cost_breakdown_2026_11_06/`의 스크립트로 비용 분해 재측정(스크립트의 경로 `C:\_ArkchimyProject`, `C:\_Arkchimy_prof`와 `prof_analyze`의 임시 폴더를 PC B에 맞게 확인) → `portfolio/architecture.md` 7장과 README 구조의 비용 줄 교체 ④ 그 결과로 첫 성능 사례 선택 - 후보는 `gqcsBatch` 키우기(PC A에서 꺼낸 완료가 상한 64에 닿음, PC B는 재측정 값으로 다시 판단. PC B 11-05 값은 워커 2에서 꺼낸 완료 20으로 상한 아래), 워커가 늘면 처리량이 주는 원인(PC B에서만 나타남), 방 전체 송신 API(`OnRecv` 안의 비용만 줄임). 처리량과 함께 지연 p99·넘침을 본다. 구조 문서를 사용자와 함께 읽는 일도 남아 있다
- **(2026-11-06, PC A) 포트폴리오 구조 정리 - 사용자가 추천안 전부 승인**: 순서는 구조 문서 → 비용 분해 → 저장소·문서 정리 → **성능 사례 1개(사용자와 측정 결과를 보고 고름, 다음 할 일)** → 넘침 컨텐츠 제약 → 암호화. 비용 분해 결과(`portfolio/architecture.md` 7장, 배터리·균형 조정이라 비율만): 워커 시간의 `WSASend` 약 60%, `OnRecv` 전체 약 30%, 커널 65–70%. 성능 작업 기준은 "사례로 쓸 수 있고 숫자의 이유를 직접 설명할 수 있는 것". 커밋·push 완료(`1319f0b`)
- **(완료, 2026-11-06, PC A) 방 락 안 `OnRelease` 재진입 실험** - `portfolio/design/lock_logic.md` 9-6, README_ERRORS 2-11. 전달 실패 강제 20회 중 4회 재진입, 4회 모두 `std::system_error`(code 36)로 서버 비정상 종료, 나머지 16회와 대조군 6회는 정상 종료. **수정 완료(사용자 결정, 커밋 `8362ab7`)**: IOCP 호출(해제 작업·종료 신호 PQCS, GQCS, GQCSEx) 자체가 실패하면 해제하지 않고 `crashOnIocpFailure`(`stderr` 로그 + `__debugbreak`)로 멈춘다(덤프는 사용자가 추가 예정). 잘못된 핸들로 실제 실패를 만들어 네 곳 확인(종료 신호 재전달은 실행 안 돼 미확인), 회귀 통과(같은 문서 9-7). 묶음 끝으로 미루는 후보는 시험 후 채택 안 함. 시험 코드는 `실험폴더/lock_reentry_2026_11_06/`(저장소 코드에는 넣지 않음), 임시 작업 트리는 지움. 실제 운영 빈도는 모른다. 받지 않는 클라 넘침이 기록(124–192)보다 많은 460–530이다(해제 경로 수정 전·후 같음, 이유 미확인). 다음은 문서 검토(사용자) → 넘침 컨텐츠 제약 → 암호화
- **상태(2026-11-05, PC A)**: `README.md` 첫 화면 초안 완성(처리량은 `/Od` 당시 측정값이라고 안내문에 밝힘, 사용자 검토 대기). 다음은 **PC B에서 `/O2` 재측정**(측정 전 CPU 상위 프로세스 확인, 4-10도 함께 가림) → README 수치·안내문과 `portfolio/cases/broadcast.md` 갱신 → 문서 검토(사용자) → 넘침 컨텐츠 제약 → 암호화
  - **PC B 재측정 때 함께 고칠 수치(사용자 결정, 2026-11-05)**: 방 16 "9.6–9.9배"(30만 → 284만, 43만 → 426만)는 `flush_ab_runs.csv` 중앙값으로 다시 계산하면 **9.5–9.8배**(29.4만 → 280만, 42.4만 → 416만, 서버 통계)라 기록과 조금 다르다. 지금 바로 고치지 않고 재측정 값으로 함께 바꾼다 - README 핵심 성과·대표 사례 5번, `portfolio/cases/broadcast.md`, 초안 문서 10장, 이 파일의 표. 대표 사례 5번의 채팅 skip 끔 비교(워커 2, 116개 → 1.06개, 516만 → 29.4만/s, `flush_ab_runs.csv` 수정 전 빌드)도 같은 구조를 다시 잴 수 없으면 "수정 전 구조, `/Od` 당시"로 유지. 아래 11-01 표의 521만·29만은 CSV 재계산(518만·26.5만)과 다르니 인용 전 CSV로 확인
  - **연결 수락 비교는 재측정 대상이 아니다**: 실험용 서버가 지금 라이브러리와 달라(코드 삭제 `bbcbb60`, 실행 파일은 `실험폴더/`) 당시 값(PC A, 2026-10-21)으로 쓰고 README에 한계를 적었다. 인용 기준은 `portfolio/cases/accept.md` - 10-20 초안(`portfolio_accept_draft`)의 "slotCnt 수십 개면 충분"은 이 문서로 뒤집힌 옛 결론이니 쓰지 않는다
- **(2026-11-05, PC B) 라이브러리 분리 - 형식은 DLL로 결정(사용자)**: 설계 문서 `portfolio/design/library_packaging.md`, 2-2–2-5 결정 대기, 구현 전. 추천은 소스를 고치지 않는 `.def` 방식 + 라이브러리 `printf` 제거. 임시 폴더 시험 빌드로 DLL만 교체(함수 내부·`Session`·링 크기)가 동작함을 확인, `NetworkLib` 멤버 변경은 서버도 빌드 필요(문서 4장)
  - 2-2–2-5 모두 추천안으로 결정(사용자). 진행: 문서 3장 1단계(`/O2` 복원), 2단계(라이브러리 `printf` 제거, `GetNetError`·`GetConfig`) 완료, 3단계(DLL 프로젝트, 파일 이동, `.def`) 구현·빌드 완료(커밋 전일 수 있음 - git status 확인). **남은 시험은 사용자 요청으로 나중에**(문서 6장)
  - **(완료, 2026-11-05, PC A) DLL 넘침 비교** - 문서 6-1장. 워커 4 방 옮기기 8회씩: 넘침 중앙값 직전 빌드 16(3–52) vs DLL 18(0–44), DLL이 많은 라운드 8번 중 3번 → DLL이 크게 늘린다는 근거 없음, 작은 차이는 판별 못 함. 16회 모두 정상 종료·순서 검사 통과. 정상 클라 넘침 자체는 16회 중 15회 발생(README_ERRORS 2-7, 컨텐츠 제약으로 다룸). 임시 폴더 빌드 경고는 프로젝트당 1건인 `MSB8029`(코드 경고 아님). 아래는 시험 절차 메모
  - **(2026-11-05, PC A) 워커 4 채팅 저하(4-10) 재현 시도 - 재현 안 됨, 원인은 미확인**: PC A는 워커 4가 워커 2보다 높다(1467만 vs 1014만/s). 묶음을 강제로 줄이면 모으기와 처리량이 함께 떨어져 방향은 확인(64 → 2: 1474만 → 280만/s), PC B 낮은 상태와 크기는 안 맞는다. 추가 확인은 PC B에서만 가능. 워커 6·8에서 정상 클라 넘침 증가(워커 8 265–1445). 다음은 `/O2` 재측정(어느 PC로 할지 사용자 결정) → 문서 검토 → README
  - **다른 기기에서 이어서 시험(2026-11-05 사용자 예정)**: 세 sln(`networkLibProject`, `EchoClient`, `LoadClient`)을 Release x64로 빌드(`NetworkLib.dll`이 서버 exe 옆에 생기는지 확인). 비교용 직전 빌드는 `git worktree add --detach <경로> d0c93d9`로 만들어 Release 빌드. 방 옮기기 넘침 비교: `server.cfg`를 임시로 `workerCnt = 4`, `chatRoomCnt = 16`으로 바꾸고 `.\tools\echo_test.ps1 -Name 이름 -RunSec 15 -ClientCfgs LoadClient/client_chatMove.cfg [-ServerExe 직전빌드exe]`를 두 빌드 번갈아 8회씩 돌려 `[stat]` 마지막 줄의 `넘침끊김`을 비교, 끝나면 cfg 원복. 숫자는 기기가 다르면 PC B 값과 섞지 않는다. 항목은 문서 6장 "나중에 할 시험"
  - 2단계 검증: Release·Debug 경고 0, 규칙 위반 0, 포트 점유 시 `서버 시작 실패: bind err=10048`, 회귀 3종(에코 4,017MB, 비정상 6.5만, 받지 않는 클라 넘침 188, 정상 끊김 0), Debug 서버 + 비정상 헤더 Assert 없음, 섞인 클라 2회 통과(최대 지연 26–50ms). GQCS 실패는 재현 시험 없음
  - **`/O2` 복원 완료(2026-11-05, 미커밋일 수 있음 - git status 확인)**: 이전까지 서버 Release는 `/Od`였다(2026-10 디버깅용으로 넣고 안 되돌림, 클라에는 없음). **이날 이전의 서버 처리량·CPU 숫자는 모두 `/Od` 값이고 `/O2` 숫자와 섞지 않는다.** 결과는 문서 5장: 정확성 시험 통과, 에코 CPU −8–14%, 채팅 워커 2 +4%, **채팅 워커 4는 −31%**(1268만 → 878만/s, WSASend당 120 → 29개, README_ERRORS 4-10, 원인 미확인). 포트폴리오 수치 재측정 필요
- 2026-11-03 PC B에서 한 것: 기본값 `mergeSends = 1`(2-2), 방 옮기기 순서 검사(3), 방향 결정(아래 5번 앞 설명)
- (아래 "이번 세션에 한 것"은 2026-11-02 기록)
- **이번 세션에 한 것**
  - 코딩 규칙 적용: `CODING_STANDARD.md`(포프 C++ 표준 + 프로젝트 예외), `Assert.h`·`Memory.h`(MemCpy/New/Delete/UniquePtr), 라이브러리·채팅 서버 전면 이름·서식 정리
  - 코딩 규칙 훅: `.claude/settings.json` PostToolUse → `tools/check_cpp_style.ps1`(BOM, `#pragma once`, 같은 줄 중괄호, 공백 들여쓰기, 금지 함수). 새 C++ 파일을 Write로 만들면 BOM 경고가 뜨니 BOM을 붙일 것
  - 송신 링 단순화(사용자 결정): **링 32KB 고정 배열(`RingBuffer<N>` 템플릿), 링이 가득 차면 끊기**. `sendLimitKB`·`mSendingBytes`·`Reset` 삭제. 아래 "송신 링 단순화" 항목에 검증 숫자
  - 결정: 받는 속도가 느린 클라(제로 윈도우 공격, 나쁜 회선)는 **L7 시간 기준으로 빨리 끊는다**(TCP는 window 0이면 안 끊고 재전송 한계도 수십 초)
- **다음에 할 일 (순서대로)**
  1. (완료, 2026-11-03) **실행 중 setter 호출 막기** - 사용자 선택 **A안 `Assert(!mbRunning)`**(B안 false 거부는 채택 안 함). setter 5개 맨 앞에 Assert, 위에 "Start 전에만 - 실행 중 바꾸면 경쟁·완료 이중 처리·세션 미해제·배열 넘침" 주석. 릴리즈에서는 검사가 없다(`__assume`)는 점은 사용자에게 설명함
     - 검증: Release·Debug 빌드 경고 0, 회귀 3종(정상 끊김 0, 비정상 9.0만 끊김, 받지 않는 클라 넘침 192), Debug 서버 에코 10초 Assert 없음
     - 이날 에코 처리량이 전날보다 낮음(15초 약 3.0GB, 전날 4.0GB) - 수정 전(HEAD) 빌드와 번갈아 5쌍: 중앙값 2,967 vs 2,955MB로 차이 없음 → PC 환경 탓(`crosvm`, VS Code 업데이트 등 백그라운드). **날짜가 다른 처리량 숫자는 비교하지 말 것**
  2. (완료, 2026-11-03, PC A) **송신 정체 감시 구현** - `SetSendStallSec`/`server.cfg sendStallSec = 10`, 감시 스레드 `runSendStallWatch`(1초 순회, 끊기 요청만, `Stop`에서 먼저 멈춤), 통계 `송신정체끊김`, `mWsaSendQPC`를 `mSendLock` 안에서 쓰고 읽음. EchoClient `rcvBuf` + `client_sendStall.cfg`. 시험(기준 3초): `sendBufZero = 1`에서 정체 클라 64세션 3초 뒤 모두 끊김·넘침 0, 정상 클라는 모든 시험에서 끊김 0·정체 0. **발견**: 커널 송신 버퍼에 들어간 데이터는 판정이 못 본다(서버 기본 버퍼에선 16KB가 다 들어가 정체 0) - 문서 5장. 무응답·하트비트는 컨텐츠(채팅 데모) 몫. 설계 결정 요약(4판): 라이브러리는 **송신 정체만** 감시 스레드(1초 순회)로 끊는다(기준 `sendStallSec` 10초, 0이면 끔). 송신 시각은 기존 `mWsaSendQPC`를 `mSendLock` 안에서 쓴다. **무응답·하트비트는 컨텐츠 몫**(형식·판정 모두, 시각은 `timeGetTime`) → 채팅 데모 때. 다음은 문서 4장 구현 범위 1–5번 구현. 아래 줄은 1판 당시 메모 - 추천: ① 감시 스레드 1초 순회 ② 수신은 `mLastRecvTick`(GetTickCount64), 송신은 `mWsaSendQPC` 쓰기를 `mSendLock` 안으로 옮김(지금은 락 밖이라 감시가 이전 송신 시각을 볼 수 있음) ③ 하트비트 = 길이 0 패킷, 라이브러리가 삼킴(EchoClient 생성기 0–4096 → 1–4096 필요) ④ 송신 정체 10초·무응답 30초, server.cfg. 배경은 `portfolio/design/send_queue.md` 5장 `[정함]` 항목. 판정 후보 `mbSendFlag && now - mWsaSendQPC > N초`. 남은 결정: 검사 방법(1초 주기 순회 스레드 vs B의 "주기 순회 없음"을 살릴 방법), N 값, 하트비트 간격. `mWsaSendQPC`는 락 밖에서 쓰므로 다른 스레드가 읽으려면 atomic 또는 `mSendLock`. **설계 논의는 문서로**(선택지·추천·`의견:`/`결정:` 줄, 5개 이내씩)
  2-1. (완료, 2026-11-03, PC A) **채팅 무응답·입장 제한·하트비트** - `portfolio/design/timeout.md` 6장. `Heartbeat = 4`, `ChatServer` 감시 스레드(`SetTimeout`, `chatIdleSec = 30`·`chatJoinSec = 10`), LoadClient `heartbeatSec`·`chatIdle`·`chatNoJoin` + 시험 cfg 3개. 시험(skip 끔): 침묵 192·입장 안 함 320 끊김, 하트비트 침묵 0, 정상 0. 시험 중 부호 없는 시각 뺄셈 버그 수정(README_ERRORS 2-8)
  2-2. (완료, 2026-11-03, PC B) **skip 켬 채팅 기아 문제** - README_ERRORS 2-9. PC B 재현 측정(`실험폴더/LoadClient_starve_2026_11_03/`): 수신마다 합치기 · skip이 11.7만/s·최대 지연 1.3초로 최악, **묶음 단위 · skip이 38.1만/s·최대 18–31ms로 최고**, 끊김은 12회 모두 0. 사용자 결정으로 **기본값 `mergeSends = 1`**. 에코는 차이 없음(126만 vs 123만/s). 반복 상한(Continue)은 재현 안 돼 보류. 아래 줄들은 측정 전 PC A 메모
     - 2026-11-03 PC A 분석(사용자와 논의): 원인은 둘이 겹친 것으로 본다. (1) **수신마다 합치기(`mergeSends = 0`, 기본값)라 채팅에서 받은 패킷당 WSASend 1.01번**(WSASend당 메시지 16) - skip 끔은 송신 완료 대기 중 우연히 133개씩 합쳐져 0.15번. PC B 11-01 측정(묶음 단위 · skip 최고, 2-7 때문에 기본값 보류)과 같은 결론. (2) skip 즉시 완료 반복문(`postRecv`·`postSend`의 `while`)에서 커널이 비지 않으면 워커가 못 빠져나옴(수신 즉시 완료 99.4%)
     - 대책 후보(합의된 방향, 구현 전 측정 먼저): ① 기본값 `mergeSends = 1` ② **반복 상한 + Continue**(사용자 제안 - 즉시 완료가 N바퀴 이어지면 그 몫을 반납하지 않고 PQCS로 `RecvContinue`/`SendContinue`를 넣어 큐 뒤로, 워커가 꺼내면 postRecv/postSend 후 반납. 카운트는 Continue가 들고 있어 세션 유지. 시작값 N = 4, `server.cfg` `skipLoopLimit`) ③ 2-7 조기 보내기(링 32KB에서도 남았는지 확인)
     - **다음 측정(사용자: 항상 쓰는 PC B에서)**: 기아 재현 조건 - 채팅 256세션(`client_chat.cfg` + `heartbeatSec = 1` 임시 복사본) + `client_chatIdleHb.cfg` 64세션, `chatRoomCnt = 16`, `chatIdleSec = 5`, `chatJoinSec = 5`, 워커 2, 20초. 4조합 `mergeSends` 0/1 × skip 켬/끔을 같은 실행 안에서 번갈아 3회. 볼 것: WSASend당 메시지(`패킷/송신완료`), 수신패킷/s·전달/s, LoadClient 최대 지연(`max=`), `무응답끊김`(기아), `넘침끊김`(2-7). PC A 결과(수신마다 합치기): skip 켬 무응답 335·최대 6초·11.3만/s, skip 끔 0·0.06초·32.6만/s - **PC B 숫자와 섞지 말 것**
     - `load_sweep.ps1`은 채팅 무응답 설정(`chatIdleSec` 등)과 하트비트 클라를 아직 모른다 - 이 PC에서는 `echo_test.ps1`을 직접 돌리고 server.cfg를 임시로 바꿨다가 되돌렸다
  3. (완료, 2026-11-03, PC B) **방 옮기기 중 순서 검사** - `portfolio/verification/room_move.md`. 사용자 개선안: 프로토콜에 `Leave`/`LeaveOk` 추가, 옮기는 길은 Leave → LeaveOk → Join → JoinOk 하나(방에 있는데 Join, 방에 없는데 Leave는 잘못된 패킷), LeaveOk는 이전 방 락 안에서 넣는다. 퇴장하면 입장 제한 시간을 퇴장 시각부터 다시 센다(감시는 `bJoined`를 시각보다 먼저 읽도록 바꿈). LoadClient `roomMoveBatches` + `client_chatMove.cfg`, 검사는 "LeaveOk와 JoinOk 사이에 Chat이 오면 버그". 결과: 4조건 약 12만 번 이동 모두 통과, 일부러 넣은 버그(방에서 안 빼기)는 1초 안에 잡음. cpp-reviewer 리뷰 반영: JoinOk/LeaveOk에 방 순번 `Seq`를 실어 입장·퇴장 직후 빠진 메시지까지 검사 - 입장 쪽 버그(JoinOk 뒤 1ms 늦게 구성원 추가)도 바로 잡음. 워커 4 · 묶음 단위의 정상 클라 넘침은 이동 시험의 부하 때문(이동 켬 23/31/69, 끔 0/0/0, README_ERRORS 2-7 - 사용자: 스트레스 테스트·컨텐츠 제약으로 다룸)
  4. 포트폴리오 초안 최종 확인(사용자) - 5장 끝에 "뒤의 변경(2026-11-02)" 단락 추가됨
     - (2026-11-04, PC B) 11-01 이후 송신 경로 결과는 **사용자 결정으로 별도 새 문서** `portfolio/cases/broadcast.md`(1 측정 환경 → 2 기준값: 방송은 메시지마다 WSASend → 3 받는 사람마다 모아 보내기 9.6–9.9배 → 4 묶음 끝으로 넓히기(2×2) → 5 채팅 기아와 기본값 변경 → 6 남은 문제: 넘침 → 7 한계). 옛 초안은 에코 이야기로 두고 10장 다음 단계에 후속 문서 링크만. 연결 감시·순서 검사·섞인 시험은 `portfolio/`의 각 문서 그대로 - README에서 연결. **사용자 검토 대기**
  4-1. (2026-11-04, PC B) **`mIoCount` 크기·위치 측정과 결정** - 사용자가 `NetworkLib.h`를 읽으며 제안
     - 크기: `int8_t`가 더 빠를까 → `tools/atomic_bench`(같은 변수에 ++/-- 반복): 경합 없으면 int8/16/32/64 모두 5.2ns, 6스레드 경합이면 모두 93ns. 기계어도 같은 `lock xadd`(크기만 다름). MSVC `std::atomic<int32_t>`의 ++/--는 내부에서 `_InterlockedIncrement`/`_InterlockedDecrement`, CAS는 `_InterlockedCompareExchange` - 기계어가 Interlocked 함수와 같음. **결정: `int32_t` 유지**(`int8_t`는 동시 사용 스레드가 127을 넘으면 0으로 넘쳐 쓰는 세션이 해제된다)
     - 위치: A 지금(앞쪽) / B 맨 끝 + `alignas(8)` / C 맨 끝 + `alignas(64)`. 처리량(채팅 방 16 워커 4, 3회): 1141만 / 1157만 / 1168만(C가 +2.4%, A와 범위 안 겹침), 에코는 차이 없음. 연산 하나의 시간(프로파일러, 1분씩): 증가·감소는 측정 바닥(약 25ns)에 묻혀 구분 안 됨, CAS 40–47ns로 조건마다 순위가 뒤집힘 - 차이 없음. **사용자 결정: C로 변경**(`Session` 맨 끝에 `alignas(64) std::atomic<int32_t> mIoCount`)
     - 발견: 비용은 크기·위치가 아니라 **CAS 횟수** - 채팅에서 `SendPacket` → `findSession`이 받는 사람마다 조건부 증가(초당 약 720만, 약 0.3코어). 줄이려면 "방 전원에게 보내기"처럼 세션을 한 번에 찾는 API(설계 논의 필요, 미착수)
     - 측정 방법 교훈: rdtsc로 직접 쟀다가 사용자 지적으로 `_lib/ProfilerLib`로 다시 잼(작업 방식에 규칙 추가). 수십 ns 연산은 프로파일러의 QPC 비용(약 25ns 바닥)에 묻힌다 - 크기 비교는 벤치마크, 위치 비교는 처리량이 더 맞는 지표였다
  4-2. (2026-11-04, PC B) **라이브러리의 `std::atomic` → `volatile LONG`/`LONG64` + Interlocked 함수**(사용자 결정 - atomic 사용법이 어색함, 범위는 라이브러리 전체, 채팅 서버는 그대로). 기계어는 같다(`std::atomic<int32_t>` ++ = `_InterlockedIncrement` = `lock xadd`). 쓰기는 전부 Interlocked(일반 대입 금지 - `volatile` 대입은 메모리 순서 보장이 약해 끊기 경쟁 재확인이 깨질 수 있음), 읽기는 그냥 읽기, bool 플래그는 `volatile LONG` 0/1(`mb` 유지), 워커별 카운터는 `AddSingleWriter`(단일 쓰기 예외). 조건부 증가는 `InterlockedCompareExchange` 반복. `std::atomic`용 `AddSingleWriter`는 `ChatServer.h`로 옮김. **컴파일러가 잘못된 쓰기를 막지 않으므로 훅이 `m` 접두어 volatile 멤버의 `=`·`++`·`--`·`+=`를 잡는다**(CODING_STANDARD, cpp-reviewer에도 추가)
     - 검증: Release·Debug 경고 0, 회귀 3종(에코 4.0GB, 비정상 6.7만 끊김, 받지 않는 클라 넘침 192, 정상 끊김 0), Debug 에코 Assert 없음, 채팅 방 16 워커 4 전달 1126만/s, 방 옮기기 1만 번 순서 검사 통과
  4-3. (2026-11-04, PC B) **Interlocked 전환 뒤 안정성 시험 - 통과**(사용자 요청: 바뀐 코드가 안정적으로 도는지 먼저)
     - 에코 회귀 3종: 정상 4.0GB / 비정상 헤더 6.4만 끊김 + 정상 2.96GB / 받지 않는 클라 넘침 192 + 정상 3.66GB - 정상 끊김 0, 세션 0, 정상 종료
     - 섞인 클라 합격 시험(`mixed_test.ps1`, 워커 2, 3회): 정상·하트비트 끊김 0, 최대 지연 21ms, 무응답 256·입장초과 256·넘침 1600·비정상헤더 8만
     - 방 옮기기(방 16, 15초): 워커 2 4.5만 번·워커 4 4.3만 번 이동, 순서 검사 통과, 넘침 0
     - 처리량(`78fdf1c`와 번갈아): 에코 같음(132만 vs 133만, 129만 vs 128만/s). 채팅 방 16 워커 2는 수정 후 +25%, **워커 4는 수정 후가 두 상태를 오감**(대부분 WSASend당 40–48개·약 900만/s, 가끔 143개·1146만/s, 수정 전은 안정적으로 약 175개·약 1070만/s) - README_ERRORS 4-10. 원인 확인은 미착수(위치 변경과 Interlocked를 따로 빌드해 나누면 됨)
     - 시행착오는 README_ERRORS 1-21–1-25, 4-8–4-10에 기록
  4-4. (2026-11-04, PC B) **cpp-reviewer 라이브러리 전체 리뷰 반영** - 확실 2개·의심 2개·주석·규칙 지적 중 **2번(묶음 단위 + skip 즉시 완료 반복의 송신 지연)은 고치지 않음**(사용자 결정, README_ERRORS 2-10). 고친 것: ① 수락 대기 수를 세션 생성 뒤에 내림(`acceptSession` 분리 - 종료 직전 수락 세션 누수) ② 수신 끝(0바이트·실패)이면 `requestDisconnect`(감시 끈 경우 반만 닫은 클라가 슬롯을 잡는 문제) ③ `mListenSock = INVALID_SOCKET`을 Stop 대기 루프 뒤로 ④ 줄인 주석 중 "왜" 되살림(`requestDisconnect`, `postSend`, `releaseSession`), 링 크기·`OnAccept`·`mbSendFlag` 주석 정정, `SocketUtil.cpp` 옛 실험 주석 삭제 ⑤ `CountIoResult` 출력 매개변수를 포인터 + `out` 접두어로
     - 검증: Release·Debug 경고 0, 규칙 훅 위반 0, 에코 회귀 3종 정상 끊김 0(받지 않는 클라 넘침 124–192), Debug 서버 에코 + 비정상 헤더 Assert 없음, 섞인 클라 2회 통과, 방 옮기기 순서 검사 통과
     - **처리량 비교는 못 함**: 측정 중 `crosvm` 때문에 PC가 계속 멈춰 수정 전·후 모두 회차마다 떨어짐(README_ERRORS 1-24 추가). 방 옮기기 워커 4의 정상 클라 넘침은 수정 전 빌드도 같은 정도(0–268)라 회귀 아님. PC가 조용할 때 `3a44ff9`와 번갈아 에코·채팅 처리량을 다시 잴 것
  4-5. (2026-11-05, PC B) **정적 라이브러리화 준비 - 설정 전달을 `NetConfig` 구조체로**(사용자 결정 B안, 분리 자체는 아직). 라이브러리는 `Start(const NetConfig&)`만 받고 파일 형식을 모른다. cfg → 구조체는 라이브러리 밖 `NetConfigLoader`의 `LoadNetConfig`(서버 exe들이 함께 씀). `Set*` 6개와 `Assert(!mbRunning)`, 개별 멤버(`mbMergeSends` 등)를 없애고 `mConfig` 하나로. 컨텐츠 설정(`SetChat`, `SetTimeout`)은 그대로. 키가 빠졌을 때의 `skipOnSuccess` 기본값을 0에서 1로 통일(`server.cfg`는 이미 1), `sendStallSec` 기본값은 0(끔) 유지
     - 검증: Release·Debug 경고 0, cfg 3가지(필수 키만, 전부 변경, 필수 키 누락)로 적용 값 확인, 에코 회귀 3종(정상 끊김 0, 비정상 6.9만, 받지 않는 클라 넘침 192), Debug 서버 + 비정상 헤더 Assert 없음, 섞인 클라 2회 통과(최대 지연 48–53ms). 처리량 비교는 안 함
     - **서식**: Ctrl+K, D가 쓰는 `.clang-format`을 코딩 규칙에 맞춤(탭, `int& number`, include 정렬 끔, 한 줄 함수 금지, 초기화 리스트 쉼표 앞). 줄 끝 주석은 탭 대신 공백으로 세로 정렬된다. 정리 뒤 규칙 위반 0
     - `RingBuffer` 생성자(사용자 추가)는 위치 3개만 초기화하고 `mBuffer`는 두어 쓴 만큼만 읽는다. 단 `vector<Session>(n)`의 값 초기화가 `Start`에서 세션 전체를 한 번 0으로 채운다(세션당 약 48KB)
  5. (완료, 2026-11-03, PC A) **섞인 클라 + 포화 부하 합격 시험 - 합격** - **정리 문서 `portfolio/verification/mixed_clients.md`**(목적·조건·클라 6종·결과·해석·한계, 다시 돌리는 명령)
     - 만든 것: LoadClient `chatNoRecv`(JoinOk 뒤 recv 안 함, 채팅도 안 보냄, 하트비트로 무응답 회피) + `rcvBuf`(SO_RCVBUF), `client_chatNoRecv.cfg`(64세션, 하트비트 1초, 수신 버퍼 1KB), 시험 스크립트 **`tools/mixed_test.ps1`**(경로 하드코딩 없음, server.cfg 임시 변경·복원, 정상 채팅 cfg에 하트비트를 줄바꿈부터 덧붙인 임시 복사본, 클라 6종 동시, 회차별 표·CSV). 원본 `실험폴더/mixed_clients_2026_11_03/raw_runs.csv`
     - 조건: 채팅 방 16, 기본값(GQCSEx + skip + `mergeSends = 1`), 워커 2, 무응답 5초·입장 5초·송신 정체 3초, 25초 × 3회. **서버 CPU 1.97–1.99/2(포화)**, 전달 493만–511만/s, WSASend당 메시지 186–191
     - 결과(3회 모두 같음): **정상 채팅 256 끊김 0, 하트비트만 64 끊김 0, 정상 클라 최대 지연 35–42ms**. 침묵 → 무응답끊김 256, 입장 안 함 → 입장시간초과 256, 받지 않는 채팅 → 넘침끊김 1600(재연결 반복), 비정상 헤더 → 라이브러리 `비정상패킷` 4.5만–4.7만(헤더 길이 검사에서 먼저 걸려 채팅 `잘못된패킷`은 0). 세션 0 복귀, 정상 종료, 통계 줄 빠짐 없음
     - 송신 정체는 0 - 받지 않는 채팅 클라에 쌓이는 방송이 커널 버퍼(~64KB) + 링 32KB를 금방 넘어 넘침이 먼저 끊는다(`portfolio/design/timeout.md` 5-1과 같은 구간 분석). 송신 정체 판정은 5장(EchoClient + `sendBufZero`)에서 따로 확인됨
     - 아래는 시작 전 계획 메모
     - **방향 결정(사용자, 2026-11-03)**: "서버가 어디서 무너지나"를 찾는 큰 스트레스 테스트는 하지 않는다. 끊김 정책 중 넘침(링 가득)·송신 정체·무응답은 "클라가 느리다"를 시간·양으로 추정하므로 **서버가 바쁘면 정상 클라를 끊는 오판**이 날 수 있다(실제로 2-9 기아, 2-7 넘침). 그래서 확인할 것은 "부하가 걸려도 정책이 정상 클라를 끊지 않는가" 하나. 한계 숫자는 서버·클라가 같은 PC라 PC 한계일 뿐이라, 지금까지 잰 값에 그 전제를 붙여 쓴다
     - 순서 다음: 4번 포트폴리오 갱신 → README 첫 화면(5번 "그 뒤"). 암호화(ECDH + AES-GCM)는 그 뒤
     - **조건(제안, 시작 전에 사용자와 확인)**: 서버는 채팅 모드(`chatRoomCnt = 16` 이상), 기본값(GQCSEx + skip + `mergeSends = 1`), **워커 2**(포화에 가깝게), 시험이 20–30초 안에 끝나도록 `chatIdleSec = 5`, `chatJoinSec = 5`, `sendStallSec = 3`. 20–30초 × 3회
     - **섞을 클라와 기대 결과**
       - LoadClient `client_chat.cfg`(+ `heartbeatSec = 1` 임시 복사본, 정상 256세션) → 끊김 0
       - LoadClient `client_chatIdleHb.cfg`(하트비트만 64) → 끊김 0
       - LoadClient `client_chatIdle.cfg`(침묵 64) → `무응답끊김`으로 끊김
       - LoadClient `client_chatNoJoin.cfg`(입장 안 함 64) → `입장시간초과`로 끊김
       - EchoClient `client_badPacket.cfg` → `잘못된패킷`으로 끊김(채팅 서버에는 종류 바이트가 무작위라 잘못된 패킷)
       - **받지 않는 채팅 클라가 아직 없다**: EchoClient `noRecv`/`sendStall`은 에코 형식이라 채팅 서버가 잘못된 패킷으로 먼저 끊는다 → LoadClient에 채팅 `chatNoRecv`(입장 후 recv를 안 함, `rcvBuf`도 작게) 모드를 만들어야 넘침·송신 정체를 채팅에서 시험할 수 있다. 기대: `넘침끊김` 또는 `송신정체끊김`
     - **합격 기준**: 정상·하트비트 클라 `unexpectedClose = 0`, 문제 클라는 각자 맞는 통계로 끊김, 세션 0 복귀, 정상 종료. 메시지 최대 지연도 함께 본다(2-9처럼 수 초가 되면 오판의 전조)
     - 도구: `load_sweep.ps1`은 여러 종류 클라를 섞지 못한다. `실험폴더/LoadClient_starve_2026_11_03/starve_test.ps1`(server.cfg 임시 변경·복원, 클라 cfg 임시 복사, 로그 파싱)을 고쳐 쓰면 된다. **cfg에 줄을 덧붙일 때 줄바꿈 먼저**(README_ERRORS 1-20)
     - 결과 숫자는 PC A 값 - PC B 값과 섞지 않는다
- **주의**
  - 서버 송신 링은 이제 "가득 차면 끊기"라 **EchoClient 한도(15,360B) × 2 ≤ 링(32,768B)** 관계가 정상 에코 클라를 지키는 근거다(README_ERRORS 2-6 끝). 링을 줄이거나 클라 한도를 늘리지 말 것
  - LoadClient의 `MAX_BATCH_BYTES` 등은 측정 조건 유지를 위해 예전 16KB 링 기준 값 그대로
  - `load_sweep`의 `+lim<N>` 모드는 지웠다(한도가 상수). 한도를 다시 실험하려면 `SEND_RING_SIZE`를 바꿔 빌드

## 다른 기기에서 이어서 작업할 때 (2026-10-29 인수인계 - 일부는 위 11-02 내용으로 대체됨)
- **지금 하던 일**: 포트폴리오 초안 `portfolio/cases/send_path.md`를 사용자와 한 장씩 검토하며 고치는 중(아래 "다음 할 일" -1번). 사용자가 직접 지운 부분이 있으니 되살리지 말 것. 반영한 것: 1장 요약 3줄, 2장 측정 방법을 문장으로, 3장 비용 분해 표에 단위·계산식(`호출 1번 시간 × 패킷당 호출 횟수`)·전 항목, 5장을 "① 묶이는 이유 → ② 4KB×1에 묶임이 없는 이유 → ③ 대조 조건" 순서로 재작성
  - 2026-10-29: 3장 검산 추가(GQCS·GQCSEx 둘 다), 6장 재작성(원인 확인·재현 조건·수정·검증), 7장 신설(모아 보내기 - 타임라인·줄인 코드·수정 전후 처리량), 8~10장 갱신. **용어: "송신 묶임/묶기" → "모아 보내기"**(사용자 결정 - 서버가 여러 메시지를 WSASend 1번에 싣는 것. 클라가 N개를 한 번에 보내는 "묶음"은 그대로). **"완료 통지 skip 방식"**(사용자 결정) = `FILE_SKIP_COMPLETION_PORT_ON_SUCCESS` - 즉시 성공한 I/O의 완료 통지를 큐에 넣지 않고 건 스레드가 그 자리에서 처리. 정의 뒤에는 줄여서 "skip". 문서에 범위를 `~`로 쓰지 말 것(한 줄에 두 개면 GitHub 취소선) - `–` 사용
  - 2026-10-29 오후: **포트폴리오 형식으로 재구성**(사용자 요청) - 머리말 요약 + 핵심 결과 표 → 1 측정 환경(PC A/B) → **2 송신 경로 구조(신설: Session 필드, 흐름도)** → 3 GQCSEx → 4 WSASend 비용 → 5 skip −82%(기존 구조 postSend/finishSend 코드 추가) → 6 넘침 버그(판정식 수정 전후 코드 추가) → 7 모아 보내기 → 8 측정을 믿기 위해 한 일 → 9 한계와 다음 단계. 위 줄의 장 번호는 재구성 전 기준. 3장 표의 `<br>`은 열 분리로 없앰. 코드 조각 원칙: 실제 함수·변수 이름, 결론에 필요한 줄만, 15–25줄 이내
  - 2026-10-31: 전체 검토 1차 반영 - 5장 "오차 수준" → "−82%에 비하면 무시할 만큼 작다" + 8장에 PC A(0%)·B(−6–14%) 차이 명시, 8장을 8-1/8-2/8-3 소제목으로 분리, 8-3 끝에 기본값 결정(GQCSEx + skip) 추가, 핵심 결과 표 행 순서 교정, 10장 open-loop 한계·다음 단계 정리, 단위·표 머리 등 잔실수
  - 2026-10-31: **두괄식으로 전면 수정**(사용자 요청) - 머리말·각 장·소절 첫 줄에 결론(굵게), 문단은 "결론 → 이유·근거" 순서. 앞으로 이 문서를 고칠 때도 이 형식 유지
  - 2026-10-31: **장 순서 재배치**(사용자 승인 - 제목·머리말의 −82%로 바로 들어가도록 skip을 앞으로) - 1 측정 → 2 송신 경로 구조 → 3 WSASend 비용 → 4 skip −82% → 5 넘침 버그 → 6 모아 보내기 → 7 skip 다시 켜기(7-1 서버 일, 7-2 지연) → 8 GQCS vs GQCSEx 모아 보내기 전후(8-1 PC A 76%, 8-2 PC B 2×2, 8-3 기본값 결정) → 9 측정 → 10 한계. GQCS/GQCSEx 정의와 비용 분해 계산법은 1장으로 옮김. 위 줄들의 장 번호는 재배치 전 기준
  - 2026-10-31: **모아 보내기 적용 범위를 좁힘**(사용자 지적 - 에코 전용). 지금 구조는 `tRecvSession`(수신 처리 중인 세션 하나)에게 가는 응답만 모으고, 다른 세션 송신은 `sendRaw`에서 바로 `postSend` → skip 기본값이면 브로드캐스트는 메시지마다 WSASend. 머리말·핵심 결과에 "요청-응답(에코)" 조건, 6장 "적용 범위" 절(postSend 호출 3곳: processRecv 끝=설계 / sendRaw 다른 세션=없음 / onSendComplete=우연), 7장·8-3에 "에코로만 측정", 10장 한계·다음 단계에 일반화 설계. 구현·측정 후 6장 확장 예정(사용자 결정: 지금은 범위만 좁히기)
  - **이름 변경(사용자 결정, 2026-11-03)**: `flushPerBatch` → **`mergeSends`**(송신 합치기 범위, `SetMergeSends`, `mbMergeSends`, `load_sweep` 모드 `+fb` → `+ms`). 0 = 수신 완료 하나마다(= 수신 단위), 1 = 꺼낸 완료를 다 처리한 뒤 한꺼번에(= 묶음 단위). 아래·예전 기록의 `flushPerBatch`는 이 이름으로 읽을 것
  - **용어(사용자 결정, 2026-11-01)**: 보내는 시점을 **"수신 단위"**(수신 완료 1건 끝에 flushSends) / **"묶음 단위"**(워커가 꺼낸 완료 묶음 끝에 flushSends, `flushPerBatch = 1`)로 부른다. 표에는 `수신 단위 · skip`, `묶음 단위 · skip`, `수신 단위 · skip 끔`처럼 skip 켬/끔을 붙이고, 문서에서는 "묶음 단위 모아 보내기"처럼 쓴다. A/B/C 같은 기호는 쓰지 않는다(표를 오가야 해서)
  - 남은 검토: 반영본을 사용자가 다시 읽고 확인
- (끝남) 그다음이던 송신 큐 재설계 범위 논의 → 2026-11-02 `portfolio/design/send_queue.md`에서 정함(세션별 바이트 링 유지, 공유 없음, 링 32KB 가득 차면 끊기, 느린 클라는 시간 기준)
- 버그 2-6(정상 EchoClient 간헐 끊김)은 해결됨 - 2026-10-29 판정식 수정, 2026-11-02 링 32KB 단순화 후에도 지연 주입 9회 모두 0. 지금 테스트에서 `unexpectedClose`가 나오면 새 문제로 볼 것
- **기기가 바뀌면 확인할 것**
  - 이 파일의 모든 측정 숫자는 이전 노트북(Core Ultra 5 125U, 14 논리 프로세서, 루프백) 값이다. 다른 기기에서 다시 재면 절대값이 달라지므로 **같은 기기 안에서 비교한 값끼리만** 섞는다
  - MSBuild 경로(VS 2022 Community), 전원 모드 GUID(최고 성능/균형 조정)는 이전 노트북 기준 - 새 기기에서 경로와 `powercfg /list`로 확인
  - `load_sweep.ps1`의 회차별 CSV는 `%TEMP%`(기기별)에 생긴다. 지난 원본은 `실험폴더/`에 보관돼 있다

## 이 PC 기준값 (2026-10-29, i5-8600 6코어 6스레드, 하이브리드 아님, 루프백)
- 빌드: VS 2022 Community MSBuild 경로 그대로 동작. 서버·EchoClient·LoadClient Release x64 빌드 OK
- 에코 테스트(기존 코드): 정상 에코 15초 2.9~3.2GB, 비정상 헤더 6.8만 끊김, 받지 않는 클라 넘침 163 - 정상 클라 끊김 0, 세션 0 복귀, 정상 종료
- **송신 완료까지 17us**(워커 2, 에코만) - 이전 노트북 2.2~2.6ms의 1/100. 그래서 "완료 대기 중 우연한 송신 묶임"과 버그 2-6이 이 PC 기본 설정에선 거의 안 보인다. 서버 CPU 1.2/2(포화 아님)
- 버그 2-6 재현: 기본 설정 정상 에코 21회 중 1회. 워커 1 + EchoClient + LoadClient(송신 완료까지 10~11ms)에서도 수정 전 빌드 **16회 중 1회**뿐(처음 3회 중 1회는 우연, LoadClient 2개 20초도 4회 중 0회) → 반복 횟수로는 수정 효과를 보일 수 없다
- 이 PC 숫자는 이전 노트북 숫자와 섞지 않는다

## 작업 방식 (사용자 선호)
- 답변과 코드 주석은 한국어. 주석은 기존 코드처럼 "왜"를 짧게, 줄 끝 `//` 주석 위주.
- **코드 동작 설명은 흐름으로**(사용자 결정, 2026-11-13): 따로 떨어진 목록이 아니라 컨텐츠 입장에서 클라 한 명이 겪는 순서대로 이어서 쓴다. 장 제목은 "무슨 일이 일어난다 → 어느 콜백"(예: ① 클라가 처음 접속한다 → `OnAccept` ② 로그인 메시지를 보낸다 → `OnRecv` → 종류로 분기해 `onLoginReq` ③ … ④ 떠난다 → `OnRelease`), 장 안에서는 하는 일을 번호 단계로. 앞 장이 끝나 있다는 전제(예: `OnAccept`가 끝난 뒤라 `Player`가 준비됨)를 이어 주고, 단계마다 어느 스레드·어떤 락인지 적는다. 코드 링크는 장 제목이나 단계 옆에
- **커밋 전에 커밋 문구를 먼저 보여주고 승인받은 뒤 커밋**한다. 성격이 다른 변경은 커밋을 나눈다(chore/feat/refactor 등 접두어 + 한국어).
- push는 요청할 때만.
- 끝에 `Co-Authored-By` 줄을 붙인다(시스템 안내의 형식을 따름).
- 변경 후에는 빌드 + 에코 테스트로 확인하고 결과를 숫자로 보고한다.
- **설계 논의는 문서로**(2026-11-02): 선택지·장단점·추천과 항목별 `의견:`/`결정:` 줄을 쓰고, 사용자가 읽고 고친 뒤 그 내용을 반영해 다음 판을 만든다. 표는 화면 칸 너비(한글 2칸)에 맞춰 `|`를 정렬한다
- **설계 문서는 처음부터 Word(.docx)로 만든다**(사용자 결정, 2026-11-15): 마크다운으로 쓰고 나중에 바꾸지 않는다. **테스트 케이스 문서도 Word로 만든다**(사용자 결정, 같은 날 - `portfolio/verification/game_test_cases.docx`가 첫 예. 지금은 같은 이름의 md도 있다). python-docx로 만들고(맑은 고딕, 코드는 Consolas, 표 머리 색), 항목마다 `의견`/`결정` 적는 칸(왼쪽 이름 칸 + 오른쪽 빈 칸, 2행 표)을 둔다 - 글자만 있는 빈 줄은 적을 자리로 보이지 않는다(사용자 지적). 다음 판은 사용자가 적은 Word를 읽고(`python-docx`로 문단·표 추출) 반영해 만든다. 만든 뒤에는 Word로 PDF를 만들어 쪽 그림으로 확인한다(`Word.Application` COM + `pymupdf` - 이 PC에 LibreOffice·pdftoppm 없음). 링크는 `file:///` 경로로 알린다
- **함수·구간 반환 시간 측정은 `_lib/ProfilerLib`로**(사용자 결정, 2026-11-04): `ProfileScope ioScope("태그");`로 감싸고, `server.cfg` `profile = 1`, `profileResetSec`/`profileSaveSec`로 구간을 정하면 서버 작업 폴더에 `<exe이름>_<시각>_Profiler.txt`(스레드별 태그 횟수·평균·최소·최대)가 저장된다. rdtsc 같은 별도 계측 코드를 새로 만들지 않는다. 측정용 `ProfileScope`는 측정이 끝나면 지운다(커밋하지 않음). 처리량 비교는 프로파일러를 끄고 따로 잰다(README_ERRORS 4-6)
- **표 작성 규칙**(사용자 결정, 2026-11-04): 마크다운 표로 쓴다(GitHub·미리보기에서 칸 표로 똑같이 읽히게 - 코드 블록 글자 표는 쓰지 않는다). 원문도 칸 너비를 한글 2칸 기준으로 맞추고 **표 한 줄은 80칸 안**(넘으면 에디터에서 줄이 접혀 열이 어긋나 보인다) - 칸에는 짧은 결론만, 긴 설명은 표 아래 글머리표로, 열이 많으면 표를 나눈다. 다 쓴 뒤 `.\tools\align_md_tables.ps1 -Path 문서.md`로 정렬
- **C++ 리뷰 에이전트 `cpp-reviewer`**(2026-11-02, `.claude/agents/cpp-reviewer.md`): 코드를 고치지 않고 변경을 따로 검토만 한다(읽기와 `git diff`만). 이 파일의 함정 목록·README_ERRORS를 기준으로 본다. 사용자가 요청할 때 쓴다("cpp-reviewer로 리뷰해줘"). 함정 목록이 바뀌면 에이전트 파일의 "꼭 확인할 것"도 같이 고친다
- **코딩 규칙 훅**(2026-11-02, `.claude/settings.json` PostToolUse `Edit|Write`): 범위 안(`networkLibProject/`, `_lib/ProtocolLib/`)의 .h/.cpp를 고치면 `tools/check_cpp_style.ps1`이 기계로 볼 수 있는 규칙(BOM, `#pragma once`, 같은 줄 여는 중괄호, 공백 들여쓰기, `memcpy`/`memset`/`ZeroMemory`/`new`/`delete`/`std::make_unique`/`NULL`, `volatile` 멤버의 일반 쓰기 - 2026-11-04 추가)을 검사해 위반을 Claude에게 돌려준다. 파일은 고치지 않는다. 전체 검사는 `.\tools\check_cpp_style.ps1`(인자 없이). 이름 규칙은 cpp-reviewer 몫. Write로 새 C++ 파일을 만들면 BOM이 없어 바로 경고가 나온다 - BOM을 붙이면 된다
- **포트폴리오에 쓸 설계·실험 문서는 `portfolio/`**(사용자 결정, 2026-11-02): `portfolio/design/send_queue.md`(송신 큐 재설계, 진행 중), `portfolio/design/wsasend_cost.md`(WSASend 비용 실험), `portfolio/design/timeout.md`(연결 감시: 송신 정체·무응답·하트비트), `portfolio/verification/room_move.md`(방 옮기기 순서 검사), `portfolio/verification/mixed_clients.md`(섞인 클라 합격 시험), `portfolio/design/lock_logic.md`(락별 목적과 락 안의 로직). 예전 실험 기록·초안은 `networkLibProject/docs/`에 그대로 둔다
- **문서 배치(사용자 결정, 2026-11-06, `portfolio/design/portfolio_structure.md`)**: `portfolio/README.md`가 읽는 순서 안내, `architecture.md` 구조, `cases/` 사례, `verification/` 검증, `design/` 결정 기록. `README.md`는 약 100줄로 유지하고 상세는 여기로 보낸다. 지원처는 **게임 서버**, `CLAUDE.md`는 공개 유지. 실험의 `.exe`는 추적하지 않는다(옛 실험은 해당 커밋에서 빌드). 옛 프로젝트(`FloodClient*`, `ParserProject`, `ProfileProject`)는 `실험폴더/projects/`로 옮김. 이 파일의 옛 기록에 나오는 문서 이름은 옛 이름일 수 있다

## 구조
| 경로 | 내용 |
|---|---|
| `_lib/NetworkLib/` | 네트워크 라이브러리 DLL, 내보내기는 `NetworkLib.def`. 공개 함수를 바꾸면 `.def`도 고친다. 서버는 공개 헤더(`NetworkLib.h`, `NetConfig.h`, `NetStat.h`)만 include하고, `Session.h`·`IoOverlapped.h`·`WorkerCounter.h`는 `NetworkLib.cpp`만 쓴다(`portfolio/design/source_layout.md`) |
| `networkLibProject/` | 서버 exe(`main.cpp`, `EchoServer`, `ChatServer`, `NetConfigLoader`). `networkLibProject.sln`, DLL도 이 폴더 `x64/` 아래에 빌드된다 |
| `EchoClient/` | 에코 **정확성** 검증 클라이언트(무작위로 잘라 보내기, 바이트 검증, 비정상/넘침 모드). `EchoClient.sln` |
| `LoadClient/` | **처리량** 측정용 부하 클라이언트(32B 메시지 `overSend`개 일괄 송신 → 전부 오면 다음 묶음, 순번만 확인). `LoadClient.sln` |
| `_lib/` | 공용: `ProtocolLib/PacketHeader.h`(서버·클라 공용 헤더), `StatLib`(`StatUtil` CPU·1초 타이머, `LatencyHistogram`, 서버·클라가 소스로 포함), `ParserLib`(cfg), `ProfilerLib`, `ExeNameLib` |
| `tools/` | `echo_test.ps1`(자동 에코 테스트), `send_ctrlc.ps1`(백그라운드 서버에 Ctrl+C), `load_sweep.ps1`(조건별 처리량 측정), `mixed_test.ps1`(채팅 서버에 정상·문제 클라 6종을 섞어 끊김 판정 합격 시험), `notify_discord.ps1`(디스코드 알림 훅) |
| `portfolio/` | 포트폴리오 문서 - 구조, 사례, 검증, 설계 기록 (`portfolio/README.md`에 읽는 순서) |
| `networkLibProject/docs/`, `실험폴더/` | 옛 실험 기록과 수치 원본 |

## 빌드와 테스트
```powershell
$msbuild = "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe"
& $msbuild networkLibProject\networkLibProject.sln /p:Configuration=Release /p:Platform=x64 /m /v:minimal /nologo
& $msbuild EchoClient\EchoClient.sln /p:Configuration=Release /p:Platform=x64 /m /v:minimal /nologo

# 서버 + 클라 → N초 → 클라 종료(세션 0 확인) → Ctrl+C로 서버 정상 종료 확인
.\tools\echo_test.ps1 -Name 이름 -RunSec 20
# 정상 + 비정상 패킷 클라 동시 실행
.\tools\echo_test.ps1 -Name 이름 -RunSec 15 -ClientCfgs client.cfg,client_badPacket.cfg
# "폴더/cfg"면 그 폴더의 클라 실행 - 부하(LoadClient) + 정상 에코(EchoClient)
.\tools\echo_test.ps1 -Name 이름 -RunSec 15 -ClientCfgs LoadClient/client.cfg,client.cfg
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
- `Start`/`Stop`은 `mStartStopLock`으로 감싼다(TOCTOU 방지). **워커나 콜백 안에서 `Stop()`을 부르면 데드락**(자기 자신을 join) → 워커는 `mbRunning=false`로 요청만 하고 main이 `Stop()`.
- AcceptEx로 받은 소켓은 `SO_UPDATE_ACCEPT_CONTEXT`를 해야 `getpeername`/`shutdown`이 동작한다.
- 수신 링(16KB) > 최대 패킷(4098B)을 `static_assert`로 보장 → 완성 패킷을 매번 다 꺼내므로 수신 링이 가득 찰 수 없다.
- 비정상 길이 헤더는 패킷 경계를 잃은 것이므로 페이로드를 기다리지 말고 즉시 끊는다.
- EchoClient: 목표량을 넘긴 뒤 **패킷 경계에서** 송신을 멈춰야 한다. 패킷 중간에서 멈추면 서버가 마지막 패킷을 에코하지 않아 연결이 끝나지 않는다.
- EchoClient: 생성기는 상태가 있어 되돌릴 수 없으므로, `send`가 일부만 보낸 나머지는 `mPending`에 보관한다.
- EchoClient의 `MAX_INFLIGHT`는 서버 송신 링(16384)에서 **제어 패킷 자리 1KB를 뺀 15360**. 링 크기 그대로 쓰면 핑이 링에 끼어 있을 때 에코가 못 들어가 넘침으로 끊긴다(핑 도입 후 `unexpectedClose=2`로 발견).
- 서버 핑은 송신 링이 가득 차면 **연결을 끊지 않고 그 핑만 버린다**(`건너뜀` 통계). 측정 때문에 정상 연결이 끊기면 안 된다. `Data`만 넘침 시 끊는다.
- EchoClient `maxInflight`는 **최대 패킷(4099B) 이상**이어야 한다. 작으면 큰 패킷을 중간까지 보내고 멈추는데 서버는 미완성 패킷을 에코하지 않아 교착(퐁도 경계에서만 나가서 같이 막힘). `init`에서 거부.
- **`OnRelease` 재진입**: 카운트를 0으로 만든 스레드가 바로 해제하면, 컨텐츠 스레드의 `SendPacket`/`Disconnect` 안에서 `OnRelease`가 불려 컨텐츠 락 데드락이 날 수 있다 → 0이 되면 `PostQueuedCompletionStatus`로 해제 작업(`eIoType::Release`)을 넣어 **항상 워커에서** 해제. 넣기가 실패하면 그 자리에서 해제하지 말 것(방 락 재잠금 데드락 경로, README_ERRORS 2-11) - 지금은 `crashOnIocpFailure`(`stderr` 로그 + `__debugbreak`)로 멈춘다. GQCS·GQCSEx·종료 신호 PQCS 실패도 같다. 라이브러리가 직접 출력하는 유일한 곳이다.
- **`SendPacket`은 아무 스레드에서나, `WSASend`는 워커에서만**(사용자 결정, 2026-11-07, `portfolio/design/external_send.md`): 워커는 링에 넣고 목록에 올린 뒤 `flushSends`가 보낸다(`mergeSends = 1`이면 묶음 끝, 0이면 수신 1건 끝, 수락·해제 콜백의 송신은 설정과 상관없이 묶음 끝). 이 객체의 워커가 아닌 스레드(`tWorkerOwner != this` - 워커 밖 스레드와 다른 라이브러리 객체의 워커 모두, README_ERRORS 2-13)는 `addSendTarget`을 쓰면 안 된다 - `flushSends`가 없어 `mbSendTarget`이 1로 남아 그 세션의 송신·해제·`Stop`까지 멈춘다. 대신 `requestSendToWorker`가 `mSendLock` 안에서 `mbSendFlag`를 잡고 `mSendOv`를 0바이트 송신 완료로 PQCS에 넣는다. 플래그는 CAS로 락 밖에서 잡으면 안 된다(`postSend`와 `mSendOv` 이중 사용, 정체 감시 오판). 워커가 아닌 스레드는 `Stop` 전에 멈춘다
- **수락 처리 중 카운트 0 문제**: 첫 수신 전에는 카운트가 0이라 `OnAccept` 안의 `Disconnect`가 실패하고(예전엔 `SendPacket`도), 수신이 바로 실패하면 `OnRelease`가 `OnAccept`보다 먼저 올 수 있다 → 수락 처리 몫으로 카운트를 잡고 `OnAccept` → `postRecv` → 반납.
- **끊기 경쟁**: `CancelIoEx`만으로는 부족(취소 직전 성공 완료가 수신을 다시 건다) → `mbDisconnect` 플래그로 새 I/O를 막고, `WSARecv`/`WSASend`를 건 **뒤에 플래그를 다시 확인**해 걸린 I/O를 취소한다. `postRecv`/`postSend`는 호출하는 쪽이 카운트를 잡고 있어야 한다(건 뒤 세션을 다시 읽으므로).
- **파생 클래스 소멸자에서 `Stop()`** 필수: 기반 소멸자 시점엔 파생 부분이 없어 `OnRelease`가 순수 가상 호출이 된다.
- `SendPacket`은 헤더와 페이로드를 따로 push하므로 `GetFreeSize()`로 둘 다 들어갈 때만 넣는다(하나만 들어가면 스트림이 깨짐).

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
  - **다른 PC로 클라를 옮기는 측정은 하지 않는다**(사용자, 2026-10-29): 예전에 시도했더니 재전송률이 너무 높아 오히려 전송량이 떨어져 비교가 안 됐다(`실험폴더/다른컴퓨터서버에 연결쏟기.pcapng`). 유선 랜이 확보되기 전에는 다시 제안하지 말 것
- 애로사항: `powercfg /getoverlayscheme`는 이 버전(25H2)에서 "매개 변수가 잘못되었습니다" - 현재 모드는 레지스트리 `HKLM\SYSTEM\CurrentControlSet\Control\Power\User\PowerSchemes`의 `ActiveOverlayAcPowerScheme`로 읽는다. 설정은 `powercfg /overlaysetactive <GUID>`(최고 성능 `ded574b5-45a0-4f42-8737-46345c09c238`, 균형 `00000000-0000-0000-0000-000000000000`)

## 코어당 처리량 감소 원인 (2026-10-25, `94adb1c` 서버 카운터, `f2f6fbd` load_sweep 클럭, `load_sweep.ps1 -Tag best_perf_ab`)
- 서버에 패킷/수신완료, 바이트/송신완료, 완료/s 카운터 추가. `load_sweep.ps1`이 측정 중 코어별 실효 클럭을 WMI로 기록(성능 카운터 이름은 현지화돼서 `Get-Counter` 영어 경로가 안 됨 → `Win32_PerfFormattedData_Counters_ProcessorInformation`)
- 결과(문서 6-1절): 자연 배칭 감소 가설 **기각**(워커가 늘수록 수신당 패킷 11.8 → 14.9), 클럭 저하 **일부 확인**(바쁜 코어 3.6 → 2.56GHz, 감소분의 ~44%). 나머지 ~56%는 **코어 종류 차이로 추정** - 유저 모드 경합은 구조상 작다(슬롯 스택은 연결 유지로 안 쓰임, 카운터·히스토그램은 워커별, 세션 락은 세션 256개에 분산, 유저 모드 자체가 CPU의 ~10%). 커널 공유 자원(IOCP 포트 하나, 루프백 TCP)은 미측정
- **결정: 코어 고정(affinity) 실험은 하지 않는다**(사용자, 2026-10-25) - 결론이 바뀔 가능성이 낮다. 다시 제안하지 말 것
- 이 PC 논리 프로세서: 0·1·10·11 성능 코어(1.3GHz 기준), 2~9 효율 코어(0.8GHz), 12·13 저전력 효율 코어(0.7GHz)
- 개선점 발견: 송신 완료당 ~240B(메시지 7~8개) - `OnRecv` 안의 `SendPacket`마다 즉시 `postSend`라 16개가 송신 2번으로 나뉜다

## GQCS vs GQCSEx (2026-10-25, 원본 `실험폴더/LoadClient_gqcsex_2026_10_25/`)
- 사용자 결정(A안): 비교용 **별도 라이브러리 `NetworkLibEx`**(NetworkLib 복사본, workerLoop만 GQCSEx). `EchoServerT<Lib>` 템플릿, `server.cfg` `gqcsEx`/`gqcsBatch`. 두 cpp는 클래스 이름·workerLoop 말고 같게 유지(`diff`로 확인) → **2026-11-01 하나로 합침**(아래 "라이브러리 합치기")
- 확정 측정(프로파일러 끔, 5회): w2 52.1만 vs 54.4만(+4.4%, 3/5), w4 77.2만 vs 89.1만(+15.4%, 5/5)
- 프로파일러 `Profile::SetEnabled` 추가 - 켜 둔 채 비교하면 기록 비용이 GQCS 쪽에만 더 붙는 혼입 변수(README_ERRORS 4-6)
- 패킷당 비용 분해(w4, ns): 총 4,917 → 4,257. GQCS 158 → 53(차이의 16%), **WSASend 4,176 → 3,671(76%)**. WSASend 1회 비용은 같고(31.4 vs 32.0µs) 호출/패킷 0.133 → 0.115, B/송신 242 → 280 → **GQCSEx는 송신 완료를 늦게 처리해 송신이 더 묶인 것**이 주 원인
- WSASend 반환: 100% 즉시 성공(w2 23.8µs, w4 36.3µs), 즉시 성공해도 완료 통지는 온다. WSARecv는 95~97% 대기(2~3µs)
- 송신 버퍼 0(SO_SNDBUF=0, `sendBufZero`): 즉시 성공 0%가 되지만 반환 시간·CPU 같고 처리량 −5~−14% → 비동기 송신 이득 없음, "송신완료까지 2~4ms"는 ACK가 아니라 IOCP 큐 대기

## 메시지 크기 비교 (2026-10-25, `load_sweep.ps1 -Tag msgsize`, 원본 `실험폴더/LoadClient_msgsize_2026_10_25/raw_runs.csv`)
- LoadClient `msgSize` 설정 추가(11~4099B), 묶음 한도 7,680B(README_ERRORS 2-5). 조건 `"4:1:모드:크기x개수"`
- 워커 4, 묶음 4KB(32×128 / 256×16 / 1K×4 / 4099×1) + 기준 32×16, GQCS·GQCSEx64 × 송신버퍼0 유무, 3회 중앙값, 전부 close 0
- MB/s: 32×16 25~29 → 32×128 140~161 → 256×16 182~213 → 1K×4 184~223 → 4099×1 305~317. WSASend 1회 33~41µs로 크기와 거의 무관 → **처리량은 송신 1회에 실리는 바이트로 정해진다**
- 송신 버퍼 0: 모든 크기에서 −2~−6%, 반환 시간 +1µs. 송신 1회 2~4KB에서는 복사 생략 이득 없음
- GQCSEx 이득: +10~15%, **4099×1에서만 0%** - 묶음이 메시지 1개라 송신 묶임이 생길 수 없고 B/송신도 같음(3711 vs 3690) → "GQCSEx 이득은 송신 완료가 늦게 처리돼 송신이 묶인 결과"라는 앞 결론을 지지
- 클라 CPU 3.7~3.98/4로 서버(4.05/4)와 함께 거의 포화 - 4099×1은 클라 한계가 섞였을 수 있음

## 즉시 완료 통지 생략 (FILE_SKIP_COMPLETION_PORT_ON_SUCCESS) (2026-10-26, `load_sweep.ps1 -Tag skip`, 원본 `실험폴더/LoadClient_skip_2026_10_26/raw_runs.csv`)
- 구현: `server.cfg` `skipOnSuccess`, 양 라이브러리 동일. 즉시 완료면 `postRecv`/`postSend`가 그 자리에서 처리(`processRecv`/`finishSend`) → 몫 반납 → 반복문으로 다시 건다. IFS 아니면 start가 끔, 소켓 설정 실패 시 연결 거부. 통계 `큐완료/s` 추가
- 정확성: skip 켜고 GQCS·GQCSEx × (에코, 비정상 헤더, 받지 않는 클라) 6개 통과
- 결과(워커 4, 3회): **32B×16·256B×16은 −82%**(78만 → 14만/s). B/송신 242 → 31 = **메시지 1개마다 WSASend 1번**, 서버 CPU/패킷 5.2 → 30µs. 4099×1은 +2.0%(GQCS)/+0.4%(Ex), 큐완료/패킷 2.2 → 1.07로 절반인데 서버 CPU/패킷은 그대로(53.8 → 53.6µs) → **완료 통지 절감 자체는 측정 오차 수준**
- 부수 효과: 송신완료까지 2.2ms → 41µs, 4099×1 부하 세션 RTT p50 4.1 → 2.7ms
- 결론: 지금 처리량은 **송신 완료가 IOCP 큐에서 ~2.5ms 기다리는 동안 우연히 생긴 송신 묶임**에 기대고 있다. 지연에 기댄 묶음이라 불안정 → 명시적 송신 묶기(수신 처리 끝에서 postSend 한 번)가 필요. 그 뒤라면 skip이 지연 개선 + 손해 없음일 가능성
- **skip 코드는 제거함(사용자 결정, 2026-10-26, `4311347`)**. `processRecv`/`finishSend` 분리와 `큐완료/s` 통계는 유지. 제거 후 빌드 OK, 32B×16 B/송신 242·75.6만/s로 원래대로. 필요하면 `09a49f6`에서 다시 가져온다

## 다음 할 일 (RTT 이어서)
-1. **2026-10-25~26 작업 포트폴리오 정리 (검토 중)** - 초안: `portfolio/cases/send_path.md`(사용자 결정: docs 초안, 이야기 하나로). 전체 검토·두괄식·장 재배치 반영(2026-10-31), 사용자 최종 확인 대기. README에는 나중에 요약만
0. **송신 큐 재설계** - 사용자 결정(2026-10-26): EchoClient 한도를 줄이는 임시 조치는 **하지 않고** 서버를 고친다
   - **2026-10-29 원인 확인·범위 결정** (README_ERRORS 2-6): 넘침 순간 링이 전부 "이미 WSASend에 넘긴 바이트"였음. 두 단계로 나눈다
     - **1단계(완료, 2026-10-29)**: 넘침 기준을 "아직 WSASend에 안 넘긴 바이트(`링 사용량 - mSendPosted`)"로, 송신 링 2배(32KB, 넘긴 몫 + 안 넘긴 몫), 명시적 송신 묶기(수신 처리 중엔 쌓기만, `processRecv` 끝에서 `postSend` 1번). 수신 처리 밖(컨텐츠·핑 스레드)의 `SendPacket`은 지금처럼 즉시 `postSend`
     - **2단계(나중)**: 버퍼 조각 송신 큐는 직렬화 버퍼·참조 카운트(3번)와 함께 - 브로드캐스트 공유가 필요해질 때
     - `Session::mSendPosted`(지금 걸린 WSASend에 넘긴 바이트)는 진단용으로 넣었고 1단계에서 그대로 쓴다
     - 검증: 워커 1 + EchoClient + LoadClient 반복에서 정상 클라 끊김 0, 받지 않는 클라 넘침 끊김 유지, 처리량 비교
     - **1단계 결과** (i5-8600, 원본 `실험폴더/LoadClient_batch_2026_10_29/raw_runs.csv`)
       - 정확성: 회귀 3종 통과(받지 않는 클라 넘침끊김 192 유지). 재현 조건 수정 전후 횟수 비교는 **정정**: 처음엔 "수정 전 3회 중 1회 → 수정 후 9회 중 0회"로 적었으나, 수정 전 빌드를 번갈아 더 돌리니 수정 전 16회 중 1회 / 수정 후 18회 중 0회 → 버그가 드물어 근거가 안 됨(원본 `실험폴더/LoadClient_batch_2026_10_29/repro_ab.csv`). 근거는 진단 기록((나)=0)과 판정식 구조
       - **결정적 재현 시험**(`server.cfg` `testSendCompleteDelayMs`, 테스트 전용 - 송신 완료 처리 전 워커 Sleep): 워커 2, 정상 EchoClient만, 15초, 지연 1/2/5ms × 3회, 수정 전 빌드에 같은 훅만 덧대 번갈아 → 수정 전 정상 클라 끊김 244~386회/15초, **수정 후 9회 모두 0**. 수정 후 + 지연 2ms + 받지 않는 클라: 넘침끊김 30(느린 클라는 계속 잡힘). 지연 0 기본 설정 회귀 3.3GB·끊김 0. 원본 `실험폴더/LoadClient_batch_2026_10_29/inject_ab.csv`
       - 수정 전 빌드에 훅 덧대는 법: `git worktree add --detach <경로> 2a91e23` → 현재 `NetworkLib*.h/.cpp`, `main.cpp`의 훅 diff를 **Git Bash에서** `git diff > p.patch`, `git apply`(PowerShell `>`는 UTF-16이라 패치가 깨짐)
       - 처리량: `load_sweep.ps1 -Tag batch_ab`, 수정 전 빌드(`2a91e23`)와 같은 실행 안에서 번갈아 3회 중앙값, 탐침 포함
         | 워커 | 크기 | 수정 전 | 수정 후 | B/송신 | CPU us/패킷 |
         |---|---|---|---|---|---|
         | 2 | 32×16 | 73.3만/s | **122.7만/s (+67%)** | 229 → 467 | 1.94 → 1.31 |
         | 4 | 32×16 | 74.2만/s | **115.9만/s (+56%)** | 209 → 465 | 2.64 → 1.56 |
         | 2 | 4099×1 | 5.83만/s | 5.64만/s (−3%, 범위 겹침) | 3607 → 3591 | 22.53 → 22.52 |
         | 4 | 4099×1 | 5.76만/s | 5.56만/s (−3%, 범위 겹침) | 3601 → 3564 | 26.37 → 25.35 |
       - 해석: 32×16은 수신 1건(메시지 ~15개) = WSASend 1번으로 묶여 B/송신 2배, 완료/패킷 0.21 → 0.135. 4099×1은 묶을 메시지가 없어 변화 없음(대조군) → 이득은 묶음에서 온 것. 이 PC는 서버(1.6/2, 1.8/4)도 클라(2.3/4)도 포화가 아니라 처리량보다 CPU/패킷이 더 믿을 만한 지표
       - 부작용: 탐침 p50 3.6 → 4.1ms (처리량이 늘어 같은 PC의 부하가 커진 영향으로 추정, 미확인)
       - 도구: `echo_test.ps1 -ServerExe`, `load_sweep.ps1`의 모드 `+base`(`-BaseServerExe`로 수정 전 빌드와 번갈아 비교)
   - **완료 통지 skip 방식 재비교 (2026-10-29, 포트폴리오 7–8장)**: skip 코드 복원(`server.cfg` `skipOnSuccess`, 기본 0, `load_sweep +skip`). 모아 보내기 위에서는 송신 1번 바이트가 유지돼(약 480B) −82% 사라짐
     - 사용자 예상(측정 전 기록): 다른 워커가 깨어나 캐시를 다시 채우는 비용이 사라져 10% 이상, 워커 2·4에서 더 클 것, 논페이지드 풀 감소
     - 결과(PC B, 5회, 원본 `실험폴더/LoadClient_skip_2026_10_29/main_runs.csv`, `profile_runs.csv`): 서버 CPU/패킷 −3.7~14.2%, 워커 1(서버 포화) 처리량 +7.4%(32B, 범위 안 겹침)/+3.0%(4KB). 문맥 전환/패킷(PC 전체) 워커 4에서 −19~30%, 워커 1은 0~3%. 효과 크기 워커 4 > 1 > 2(워커 2가 가장 작은 이유 미확인). 워커 1 비용 분해: 줄어든 128ns/패킷 중 GQCS 44%, 완료 분기 로직 42%. 포화 시 송신 완료 대기 6.1ms → 14us. 논페이지드 풀은 PC 전체 값이라 차이 안 보임
     - `load_sweep.ps1`에 PC 전체 문맥 전환/패킷, 논페이지드 풀 열 추가(스레드별 WMI 조회는 8초씩 걸려 불가). 클럭 샘플러는 시간 기준 종료로 변경
     - **핑 대기 측정**(서버 통계 `핑표시걸림%`, `핑송신대기(us)`, 원본 `ping_wait_runs.csv`): 워커 1 skip 끔 → 핑 88~95%가 송신 중 표시에 걸려 중앙값 2.7ms 대기, skip 켬 → 0.2%·0.1us. 왕복 시간 차이 6.5ms 중 약 40%만 설명(나머지는 퐁의 수신 완료 큐 대기 감소로 추정, 미측정)
     - **GQCS/GQCSEx × skip 2×2**(사용자 예상: Ex가 빠르다, GQCS 지연이 더 길다, Ex+skip이 최선 - 측정 전 기록, 원본 `ex_skip_runs.csv`): 워커 1(포화) GQCS 56.7만 / +skip +5.8% / Ex +14.0% / **Ex+skip +22.5%**(효과가 곱해짐), 왕복 p50 10.5 / 3.7 / 9.1 / 3.3ms. 워커 4(여유)는 넷 다 비슷, Ex는 한 번에 1.1~1.3개만 꺼내 CPU/패킷 오히려 +6%. 예상 셋 다 "포화일 때 맞음"
     - open-loop 모드(`LoadClient` `ratePerSession`) 추가했으나 이 PC에서는 클라 지연 바닥(약 10ms)과 클라 CPU 포화로 수용량 측정 불가 - 데모 부하 테스트용으로 남김. open-loop에서는 수신 1건에 메시지 3~4개(세션당 1000/s)라 모아 보내기 효과가 작다
     - **기본값 결정(사용자, 2026-10-29): GQCSEx(64) + skip** - `server.cfg` `gqcsEx = 1`, `skipOnSuccess = 1`. 포화 시 처리량 +22.5%·지연 최저, 여유 시 CPU/패킷 약 3% 손해(꺼낼 게 1~2개뿐이라 GQCSEx 호출이 더 비싼 것으로 추정, 미측정). 새 기본값 회귀 3종 통과(에코 3.0GB, 비정상 7.1만 끊김, 받지 않는 클라 넘침 192, 정상 클라 끊김 0)
     - **주의**: `echo_test.ps1`은 `server.cfg` 기본값을 쓰므로 이제 GQCSEx + skip으로 돈다. 이 날짜 이전 기록의 측정은 GQCS·skip 끔 기준이다. `load_sweep.ps1`은 조건마다 `gqcsEx`/`skipOnSuccess`를 명시적으로 덮어쓴다
   - 버그: 정상 EchoClient가 가끔 넘침으로 끊김(15초 테스트 6~7회 중 1~2회 `unexpectedClose=1`, README_ERRORS 2-6). 송신 링은 **송신 완료를 처리해야** 비워지는데 완료가 큐에서 ~2ms 기다리는 동안 이미 클라에 도착한 데이터가 자리를 차지 → 서버가 "느린 클라"와 "내 완료 처리가 밀린 것"을 구분 못 하고 정상 클라를 끊음. 부하가 클수록 더 끊김
   - 방향(논의 후 확정): 고정 16KB 송신 링 → **송신 큐**(버퍼 조각 연결, 세션당 한도 넉넉히), 넘침 기준을 "아직 WSASend에 넘기지 않은 바이트" 또는 "가장 오래된 송신이 N초 넘게 완료 안 됨"(시간 기준)으로. 버퍼를 완료 전에 지우면 안 됨(overlapped 규칙 - 대기로 넘어가면 커널이 직접 읽음)
   - 1번(명시적 송신 묶기)과 같은 송신 경로라 **함께 설계**. 다음 할 일 3번(직렬화 버퍼 + 참조 카운트)과도 연결 - 범위를 먼저 사용자와 논의
   - 완료 확인: EchoClient 한도를 원래(15,360B) 이상으로 두고 정상 클라 끊김 0을 반복 테스트로 확인 → 포트폴리오 서사("부하 시 정상 클라가 끊기는 버그 → 넘침 기준이 완료 지연까지 셈 → 송신 큐로 해결")
1. **명시적 송신 묶기**: 수신 처리 중 `SendPacket`은 링에만 넣고 수신 처리 끝에서 `postSend` 한 번 → 묶임을 설계로 보장. 묶기 ± skip × GQCS/GQCSEx를 32B×16, 256B×16, 4099×1로 비교(기대: 묶기 + skip = 처리량 유지 + 송신완료 지연 제거). 컨텐츠 스레드·브로드캐스트처럼 수신 처리 밖의 `SendPacket`은 어떻게 묶을지 논의 필요
   - 포트폴리오 서사: "완료 통지 최적화를 했더니 −82% → 원인 추적 → 처리량이 우연한 송신 묶임에 기대고 있었음 → 설계로 묶기"
2. LoadClient에 목표 전송률(open-loop) 옵션 → 부하 대비 지연 곡선(포화 전 지연과 한계점). 함정(Nagle 등) 논의
2. 하트비트 타임아웃(퐁이 안 오면 끊기)
   - **핑 역할 재배치(사용자 동의, 2026-11-01)**: RTT 측정은 **클라**로(탐침·LoadClient·데모 클라가 Ping → 서버가 라이브러리 수준에서 Pong 반사, 지금도 동작), 연결 감시는 **서버**가 세션별 마지막 수신 시각으로(서버 송신 없이 시각 기록만), 서버 내부 진단(핑표시걸림·핑송신대기, 핑 스레드·히스토그램)은 **끌 수 있는 옵션**으로(`pingIntervalMs = 0` 기본). 잃는 것: 서버 내부 분해 지표(7-2의 근거), 한곳에 모인 분포, 클라 값은 신뢰 불가(표시용만)
   - 시점: 모아 보내기 실험(묶음 끝 경계, 침투형 리스트)이 끝난 뒤 하트비트와 함께 - 지금 바꾸면 비교표의 "부하 RTT"(서버 핑) 열이 끊긴다
   - **서버 핑 제거 완료(2026-11-01, 사용자 결정 - 코드 정리 단계에서 앞당김, 미커밋)**: 두 라이브러리에서 핑 스레드(`pingLoop`/`sendPingSlice`), `onPong`, 워커별 RTT 히스토그램, `setPing`/`getRttCounts`, `Session::mPingSeq/mPongSeq/mPingPushQpc`, 핫패스의 핑 대기 측정(`sendRaw`·`postSend`), `NetStat` 핑 항목 삭제. `main`의 RTT 출력·`printRttTotal`, `server.cfg`의 `pingIntervalMs`/`pingSampleMod` 삭제. **남긴 것**: 클라가 보낸 Ping → Pong 반사(`processRecv`), 헤더의 Ping/Pong 종류. 들어온 Pong은 버린다
     - **지연 지표 교체**: LoadClient가 **메시지 왕복 지연**을 잰다 - `LoadSession::mLastReqQpc`(묶음을 송신 버퍼에 넣은 시각, 세션당 하나), 자기 메시지가 돌아올 때마다 워커별 `LatencyHistogram`에 기록, 매초 `지연(us) p50= p99= max=` 출력(closed-loop만). `load_sweep`은 서버 `rtt(us)` 대신 이 값을 `MsgP50us`/`MsgP99us`(표 `메시지p50/p99(ms)`, 클라가 여럿이면 가장 나쁜 값)로 읽고, 핑걸림%·핑대기us 열은 삭제. **이 날 이전 CSV의 `LoadP50us`(서버 핑 RTT)와는 다른 지표** - 섞어 비교하지 않는다
     - 검증: 빌드 경고 0, 에코 회귀 3종(3.3GB, 비정상 7.5만 끊김, 받지 않는 클라 넘침 192, 정상 끊김 0), 짧은 sweep(8초, 워커 2, GQCSEx + skip): 탐침 Pong 반사 정상(탐침 p50 4.1ms), 채팅 방 1/4/16 순서 검사 통과·끊김 0. 메시지 p50 에코 3.2ms / 방 4 6.7ms / **방 16 22ms**(예전 서버 핑 RTT 13.7ms보다 큼 - 핑은 실제 방송 경로를 안 거쳤다)
     - 남은 일: 하트비트 타임아웃(세션별 마지막 수신 시각) - 별도 작업
   - **측정을 전부 클라로 + 헤더 2바이트 + 탐침 제거(2026-11-01, 사용자 결정, 미커밋)**: 서버는 메시지 처리만 하고 지연에 관한 어떤 측정·반사도 하지 않는다. 헤더 `kind`(Data/Ping/Pong)와 `PingPayload` 삭제 → **`PacketHeader`는 `len` 2바이트**. 라이브러리의 Pong 반사·제어 패킷 넘침 예외 삭제(`sendRaw(session, payload, len)`, 넣지 못하면 바로 넘침 끊기), `OnRecv`는 헤더 길이 검사만 통과하면 전부. LoadClient 퐁 응답·`pong=` 출력 삭제. EchoClient 탐침·`idle` 모드, 제어 대기열(`mCtrlQueue`), 패킷 단위 수신 파서 삭제 - 헤더까지 에코 스트림이라 받은 바이트를 그대로 생성기와 비교. `client_probe.cfg`·`client_idle.cfg` 삭제, `load_sweep`의 탐침 실행·`탐침p50/p99` 열 삭제(실행 줄은 메시지 p50). 서버·EchoClient 프로젝트에서 안 쓰는 `LatencyHistogram.h` 항목 제거
     - **이후 측정은 헤더 2바이트**: 같은 `msgSize`여도 페이로드가 1바이트 크다. 이 날 이전 CSV의 `ProbeP50us`/`ProbeP99us`(탐침)는 이제 없다
     - 검증: 세 프로젝트 빌드 경고 0. 에코 15초 3.9GB(EchoClient 검증이 가벼워져 3.3GB → 3.9GB), 비정상 헤더 6.5만 끊김 + 정상 2.9GB, 받지 않는 클라 넘침 192 + 정상 3.6GB - 정상 클라 끊김 0, 세션 0, 정상 종료. 짧은 sweep(8초 1회, 워커 2, GQCSEx + skip, 32×16): 에코 137.6만/s 메시지 p50 2.9ms, 방 1/4/16 전달 134만/256만/305만/s, 메시지 p50 3.1/6.4/21.2ms, 순서 검사 통과·끊김·넘침 0
   - **라이브러리 합치기(2026-11-01, 사용자 결정 - ①안, 미커밋)**: `NetworkLibEx.*` 삭제. 두 파일은 `workerLoop`만 달랐으므로 `NetworkLib` 하나에 `workerLoopGqcs`/`workerLoopGqcsEx`를 두고 `start`(`initIocp`)에서 `mGqcsEx`로 워커 함수를 고른다(완료마다 분기 없음). 완료 분기는 `handleCompletion`으로 공용. `setGqcsEx(on, batch)`(라이브러리 기본 GQCSEx 64), main의 `gqcsEx` 기본도 1. `EchoServerT<Lib>`/`ChatServerT<Lib>` → 일반 클래스 `EchoServer`/`ChatServer`. main의 통계 함수 템플릿은 채팅 통계 출력 때문에 유지. cfg 키·`load_sweep` 모드(`gqcs`/`ex<N>`)는 그대로
     - GQCS를 지우지 않은 이유: GQCSEx 기본값의 근거가 GQCS 비교라 같은 코드로 다시 잴 수 있게. `gqcsBatch = 1`인 GQCSEx는 다른 API라 대신할 수 없다
     - 검증: 빌드 경고 0, 회귀 3종(에코 3.9GB, 비정상 6.2만 끊김 + 정상 3.0GB, 받지 않는 클라 넘침 192 + 정상 3.7GB, 정상 끊김 0). 합치기 전 빌드(`7895b34`, worktree `D:\_Arkchimy_base`)와 번갈아 3회(워커 2, skip, 32×16, 10초, 원본 `%TEMP%\arkchimy_echo_test\load_sweep_merge_ab.csv`): GQCS 133.1만 vs 전 128.4만(범위 겹침, 완료/GQCS 1.0 = GQCS 경로 확인), GQCSEx 132.0만 vs 131.0만, 방 16 전달 301.7만 vs 298.0만 - 차이 없음
   - **이름 정리(2026-11-02, 사용자와 5개씩 결정, 진행 중)**: 지난 기록·CSV·포트폴리오에는 옛 이름이 남아 있다. 옛 → 새
     - 모아 보내기: `tInBatch` → `tDeferSend`, `tFlushList` → `tSendTargets`, `Session::mFlushQueued` → `mIsSendTarget`, `queueFlush` → `addSendTarget` (`flushSends` 유지)
     - 송신 상태: `Session::mSendPosted` → `mSendingBytes`, `mSendStartQpc` → `mWsaSendQpc`, `sendRaw` → `marshalToSendBuf`(사용자 선택 - 나중에 직렬화 버퍼를 만들면 컨텐츠 쪽 이름은 `serialize`로 해 겹치지 않게) (`mSendFlag`, `SEND_PENDING_LIMIT` 유지)
     - 세션 수명: `acquireSession` → `tryIncIoCount` (`mIoCount`, `decIoCount`, `findSession`, `mDisconnect` 유지)
     - 수락·슬롯: `SlotOf` → `IdxOf`, `mPendingAccept` → `mPendingAcceptCnt` (`AcceptOv`, `mAcceptOvs`, `MyOverlapped`, `mIdx`, `mFreeIdx`, `mFreeLock` 유지)
     - 내부 함수: `processRecv` → `completeRecv`, `finishSend` → `completeSend` (성공한 수신·송신 마무리 짝, `initIocp`/`stopWorkers` 유지)
     - 통계: `recvDataCnt`/`sendDataCnt` → `recvPacketCnt`/`sendPacketCnt`(워커별 `recvData`/`sendData` → `recvPacket`/`sendPacket`, 지운 Data 종류에서 남은 말), `sendLatencyTicks` → `sendWaitTicks` (`sendSync…` 유지). 출력 문구는 그대로라 `load_sweep` 파싱 영향 없음
     - 이름 정리 끝 (2026-11-02)
     - **코딩 규칙 적용(2026-11-02, `CODING_STANDARD.md`)** 으로 다시 바뀐 이름 - public 메서드 PascalCase: `start`/`stop`/`isRunning`/`getStat` → `Start`/`Stop`/`IsRunning`/`GetStat`, `setX` → `SetX`, `getSkipOnSuccess`/`getGqcsEx` → `IsSkipOnSuccess`/`IsGqcsEx`, `IdxOf` → `GetIdx`. bool `mb`/`b`/`tb`: `mRunning`·`mDisconnect`·`mIsSendTarget`·`mSendFlag` → `mbRunning`·`mbDisconnect`·`mbSendTarget`·`mbSendFlag`, `tDeferSend` → `tbDeferSend`. 약어 대문자: `mId`·`mIp`·`mIocp`·`mWsaSendQpc` → `mID`·`mIP`·`mIOCP`·`mWsaSendQPC`, `initIocp` → `initIOCP`. 동사-목적어: `workerLoopGqcs`/`workerLoopGqcsEx` → `runWorkerLoopGQCS`/`runWorkerLoopGqcsEx`, `qpcNow` → `getNowQPC`, `myCounter` → `getMyCounter`. 구조체 멤버 PascalCase: `NetStat`·`WorkerCounter`·`ChatStat`(`acceptCnt` → `AcceptCnt` 등, 워커별 카운터는 NetStat과 같은 이름), `PacketHeader::len` → `Len`, `ChatMsg`의 `roomSeq`/`senderKey`/`senderSeq` → `RoomSeq`/`SenderKey`/`SenderSeq`, `AcceptOv::mAcceptSocket` → `AcceptSocket`. 함수 있는 구조체는 클래스: `Session`(`friend class NetworkLib`), `WsadataRAII`(`mOk` → `IsOk()`). 자유 함수 PascalCase: `AddSingleWriter`, `CountIoResult`, `CreateListenSocket`, `CloseReset`, `IsIfsTcpSocket`. `RingBuffer` 메서드 PascalCase(`Push`, `GetReadBufs` 등). 컨텐츠 `setChat`/`getChatStat` → `SetChat`/`GetChatStat`. null이 안 되는 `Session*` 매개변수는 `Session&`
     - 검증: 빌드 경고 0, 회귀 3종(에코 4.2GB, 비정상 6.8만 끊김 + 정상 3.1GB, 받지 않는 클라 넘침 192 + 정상 3.8GB, 정상 끊김 0), 채팅 방 16 수신 단위·묶음 단위 순서 검사 통과·끊김 0
   - **송신 링 단순화(2026-11-02, 사용자 결정)**: 한도 = 링 = 32KB 상수, **링이 가득 차면 끊기**(완료 대기 몫도 셈). `RingBuffer<N>` 템플릿 고정 배열(세션 안에 바로, `RingBuffer.cpp` 삭제), `mSendingBytes`·`SetSendPendingLimit`·`RingBuffer::Reset`·`server.cfg` `sendLimitKB`·`load_sweep` `+lim<N>` 삭제. 리뷰 지적(Reset 순서, 한도 상한·int 넘침, 적용 한도 로그)은 코드째 없어짐 - main은 `송신 링 = 32KB` 출력
     - 근거: skip 기본값이면 완료 대기 몫이 커널 송신 버퍼가 찰 때만 남는다. 주변 세션으로 쌓이는 양은 버퍼 크기로 못 푼다(컨텐츠 정책 몫). 받는 속도가 느린 클라는 시간 기준으로 따로(아래 할 일)
     - 검증: 4개 빌드 경고 0, 회귀 3종(에코 4.0GB, 비정상 6.5만 끊김 + 정상 3.2GB, 받지 않는 클라 넘침 192 + 정상 4.0GB, 정상 끊김 0). 지연 주입(워커 2, GQCS, skip 끔, 1/2/5ms × 3회) 정상 끊김 9회 모두 0 - EchoClient 한도 15,360B × 2 < 32,768B라 구조로 보장. 채팅 방 16(워커 2·4 × 수신 단위·묶음 단위, skip, 3회, `-Tag ring32_chat`) 끊김 0, 묶음 단위 · skip 전달 798만/1131만/s
     - LoadClient의 묶음 한도 상수(`MAX_BATCH_BYTES` 등)는 측정 조건을 바꾸지 않으려고 예전 16KB 링 기준 값 유지
   - **할 일: 받는 속도가 느린 클라 판정(시간 기준)** - **필요하다고 정함(사용자, 2026-11-02)**: 제로 윈도우 공격이나 통신 상태가 나쁜 연결을 TCP에 맡기면 타임아웃이 너무 길어(window 0은 아예 안 끊음) L7에서 빨리 끊는다. 판정 `mbSendFlag && now - mWsaSendQPC > N초`(메시지별 시각 불필요). 남은 결정: 검사 방법(1초마다 세션을 도는 스레드에서 하트비트와 같이 보는 안을 제안 - 설계 문서 B의 "주기 순회 없음"과 함께 다시 정함), N 값. `mWsaSendQPC`는 지금 락 밖에서 써서 다른 스레드가 읽으려면 atomic 또는 `mSendLock`
   - **할 일: 실행 중 setter 막기**(리뷰 지적) - 남은 setter 5개. 추천: 실행 중이면 `false`로 거부 + `mStartStopLock`(우리 `Assert`는 릴리즈에서 `__assume`이라 부적합). 미정
   - **할 일: 방 옮기기 중 순서 검사**: LoadClient는 한 번만 입장해 방 이동을 시험하지 않는다. "JoinOk(새 방) 뒤에는 이전 방 Chat이 오지 않는다"는 코드로만 따진 결론(이전 방 `leave`와 방송이 같은 방 락, JoinOk는 `leave` 뒤 새 방 락 안에서 링에 넣음) - LoadClient에 주기적 방 이동 모드 + 검사를 넣어 확인

## 다음 할 일: 모아 보내기 일반화 (2026-10-31 설계 논의)
- 문제: 6장 모아 보내기는 수신 처리 중인 세션에게 가는 응답만 모은다(에코 전용). 채팅처럼 `OnRecv(A)`에서 B·C에게 보내면 `sendRaw`의 `session != tRecvSession` 경로로 바로 `postSend` → skip 기본값에서 메시지마다 WSASend(측정 안 함). GQCSEx + skip 기본값 결정도 에코로만 쟀다
- 설계(제안): `thread_local` 목록 `tFlushList` - 작업 단위 안의 `sendRaw`는 링에 넣고 처음 보는 세션만 목록에 올림(`mFlushQueued.exchange(true)`, `acquireSession`으로 몫 잡기). 작업 단위 끝에서 `flushSends()` - 세션마다 플래그를 **postSend 전에** 내리고 `postSend`, 몫 반납. 에코는 목록이 {A}라 지금 구조와 같다(특수한 경우). 컨텐츠 API는 그대로(`SendPacket`만)
  - 경계: 워커는 GQCSEx 묶음 하나(부하에 따라 모이는 양이 저절로 조절, 타이머 없음). 컨텐츠 스레드는 `SendBatchScope` 같은 경계를 직접. 경계가 없는 스레드(`tInBatch == false`, 핑 등)는 지금처럼 바로 `postSend`
  - 다른 안: 전용 송신 스레드(주기 지연, 한가할 때도 기다림), 세션별 타이머(Nagle식) - 채택 안 함
  - 2단계(참조 카운트 버퍼 + WSABUF 모아 보내기, 세션별 키 암호화를 플러시 시점에)와 연결
- 순서: 채팅 데모(방 브로드캐스트)로 **지금 구조의 기준값부터** 측정(skip 켬/끔, B/송신·CPU/패킷·RTT) → 일반화 구현 → 지금 구조 / 완료 1건마다 / GQCSEx 묶음마다 비교, 방 크기 1을 대조 조건
- **사용자 예상(2026-10-31, 측정 전)**: 지금 구조에서 채팅은 skip을 켜면 오히려 성능이 떨어진다(에코에서만 이득). skip에서는 WSASend가 동기로 끝나 송신 중 표시로 모을 창이 없어, 주변 세션이 보낼 때마다 WSASend
  - 맞다면: 방 크기 ≥ 2에서 WSASend 1번당 메시지 ≈ 1, skip 켬의 서버 CPU/전달 > 끔. 방 크기 1에서는 지금처럼 skip 이득
  - 일반화 후에도 남는 것: skip이면 받는 쪽 창은 `mFlushQueued`(처음 올린 워커의 작업 단위)뿐 - 창이 닫힌 뒤 도착분은 새 WSASend. 더 모으려면 의도적 지연(송신 스레드) 필요
- **채팅 데모 설계 결정(사용자, 2026-10-31)**: ① 워커에서 직접 처리 + 방 락(로직 스레드 방식은 나중에 별도 실험) ② 클라가 `Join` 패킷으로 방 입장(컨텐츠 페이로드 앞 타입 1바이트, 라이브러리 헤더엔 타입 없음 원칙 유지) ③ LoadClient에 chat 모드(자기 메시지 기준 closed-loop, 보낸 사람별 순번 검사)
  - 설계(사용자 승인, 2026-10-31): `ChatProtocol.h`(Join/JoinOk/Chat), `ChatServerT<Lib>`, 세션별 방 번호는 슬롯 인덱스 배열 + `slotOf(id)`(같은 세션의 콜백은 동시에 안 불려 락 불필요), `server.cfg` `content = echo|chat`(기본 echo)·`roomCnt`, 라이브러리 카운터 "넣은 Data 패킷 수"(WSASend 1번당 메시지 수 = 넣은 패킷 ÷ 송신 완료), LoadClient `mode = chat`·`roomSize`, 제약 `roomSize × overSend × msgSize ≤ 넘침 한도`
  - **순서 보장(사용자 지적으로 수정)**: 보낸 사람 포함만으로는 부족 - 구성원 목록을 복사해 락 밖에서 보내면 워커 둘의 송신이 섞여 받는 사람마다 순서가 달라진다. → **방 락을 잡은 채 구성원 전원의 링에 넣는다**(지금 구조에선 B-2 때문에 락 안에서 WSASend까지 함 - 일반화하면 락 안은 push만). 서버가 방 락 안에서 **방 순번**을 페이로드에 적고, LoadClient는 방 순번이 처음 받은 것 이후 +1씩인지 검사(모든 받는 사람이 같은 순서·누락 없음). 페이로드 `[Chat][방 순번 8][보낸 사람 키 8][보낸 사람 순번 8][채움]`. 방 락 대기 시간도 측정
  - 측정: 방 크기 1/4/16 × skip 켬/끔(GQCSEx) × 워커 2/4, 클라 CPU 포화 주의(받는 양이 방 크기배)
  - **서버 구현 완료(2026-10-31)**: `_lib/ProtocolLib/ChatProtocol.h`, `networkLibProject/ChatServer.h`(`ChatServerT<Lib>`, 방 락 안에서 roomSeq 매기고 전원 SendPacket, JoinOk도 방 락 안에서), 두 라이브러리에 `SlotOf(id)`·`sendData` 카운터, `main`의 `chatRoomCnt`(0 = 에코, 기본), `[stat]` 끝에 `송신패킷/s`·`패킷/송신완료`(WSASend 1번당 메시지 수), `[chat]` 줄(브로드캐스트/s, 전달/s, 방락대기·보유 us). 빌드 경고 0, 에코 회귀 3종 통과(에코 3.4GB, 비정상 7.8만 끊김, 받지 않는 클라 넘침 168, 정상 클라 끊김 0), `chatRoomCnt = 10` + EchoClient로 잘못된 패킷 끊기·정상 종료 확인. 에코 기본 설정에서 패킷/송신완료 약 2.25
  - **LoadClient chat 모드 완료(2026-10-31)**: cfg `roomSize`(0 = 에코), 방 = 세션 번호(`mSlot`, 재연결해도 유지) / roomSize. 연결 → Join → JoinOk 후 첫 묶음, 자기 메시지(senderKey) overSend개가 다 오면 다음 묶음, 방 순번 +1·자기 메시지 순번 검사(어긋나면 `__debugbreak`). 출력에 `수신메시지/s`, `수신/보냄`(≈ 방 크기), `입장`. 제약: msgSize ≥ 28, roomSize × overSend × msgSize ≤ 15360, closed-loop만. 샘플 `LoadClient/client_chat.cfg`(방 16)
  - **overSend는 16으로 고정해 측정(사용자 결정)**. 32B × 16이면 방 크기 30까지 넘침 한도 안
  - 첫 실행(측정 아님 - 1회 10초, 워커 2, GQCSEx + skip 기본, 256세션, 서버 chatRoomCnt 256으로 임시 변경): 방 1/4/16 모두 순서 검사 통과·끊김 0·세션 0 복귀
    | 방 크기 | 전달/s | 패킷/송신완료 | 서버 CPU | 방락보유(us) |
    |---|---|---|---|---|
    | 1 | 143만 | 16.00 | 1.72/2 | 0.20 |
    | 4 | 40만 | 1.31 | 2.03/2 | 18.8 |
    | 16 | 32만 | 1.06 | 1.97/2 | 93.9 |
    - 예상대로 방 크기 ≥ 4에서 WSASend 1번당 메시지 ≈ 1(B-2 + skip). 방 락 보유가 방 크기에 비례해 커짐(락 안의 WSASend). skip 끔 비교는 아직 - load_sweep에 content·roomSize 조건 추가 후 측정
  - **채팅 기준값 측정 완료(2026-11-01)**: `load_sweep.ps1` 모드에 `+room<N>`(서버 chatRoomCnt 256, 클라 roomSize N, `-creplace`) 추가, 채팅 표(전달/s, 패킷/송신, CPU us/전달, 방락대기·보유, 클라받은/s). 원본 `실험폴더/LoadClient_chat_2026_11_01/raw_runs.csv`(3회, 15초, GQCSEx 64, overSend 16, 256세션 + 탐침), 끊긴 첫 실행 `interrupted_run_rounds1-2.csv`
    | 워커 | 방 | skip 켬: 전달/s · 패킷/송신 · CPU us/전달 | skip 끔: 전달/s · 패킷/송신 · CPU us/전달 | 끔/켬 전달 |
    |---|---|---|---|---|
    | 2 | 1 | 116만 · 15.1 · 1.46 | 117만 · 14.5 · 1.48 | 1.0× |
    | 2 | 4 | 36만 · 1.29 · 5.60 | 238만 · 31.5 · 0.76 | 6.7× |
    | 2 | 16 | 29만 · 1.06 · 6.81 | 521만 · 115 · 0.39 | 18× |
    | 4 | 1 | 112만 · 15.0 · 1.67 | 112만 · 14.4 · 1.79 | 1.0× |
    | 4 | 4 | 48만 · 1.29 · 6.34 | 233만 · 9.5 · 1.32 | 4.9× |
    | 4 | 16 | 43만 · 1.06 · 7.38 | 741만 · 125 · 0.41 | 17× |
    - 사용자 예상 셋 다 맞음: 방 ≥ 4에서 skip 켬은 WSASend 1번당 약 1개, CPU/전달은 skip 켬이 4.8–18배, 방 1에서는 skip이 같거나 약간 이득(CPU −1%/−7%)
    - skip 끔이 많이 모이는 이유는 다시 "우연한 모아 보내기": 송신 완료까지 방 16에서 4.8–6.6ms(skip 켬은 7–9us) 동안 다른 구성원 메시지가 쌓여 한 번에 115–125개
    - skip 켬의 부작용: 방 락 보유 105–130us(방 16, 락 안의 WSASend 16번), 부하 RTT p50 78–117ms(skip 끔 7.6–10.5ms)
    - 서버: 워커 2는 포화(~2/2), 워커 4는 3.0–3.2/4. 클라는 최대 2.37/4로 포화 아님 → 숫자는 서버 쪽
    - 측정 이상(README_ERRORS 1-16): 방 16에서 36회 중 4회 서버 통계 스레드가 수 초씩 멈춤 - 클라 수신량으로 교차 확인, 중앙값 영향 없음
    - 다음 목표: 모아 보내기 일반화로 **skip을 켠 채 skip 끔 수준의 모으기**(완료 지연이라는 우연 대신 설계로) + 방 락 안 WSASend 제거
  - **모아 보내기 일반화 1단계 구현(2026-11-01, 미커밋)**: 사용자가 구조를 여러 번 반박 → 코드 예제(`scratchpad/demo_flush.cpp`, 수정 전 9번 vs 수정 후 5번)로 납득. `tRecvSession` → `tInBatch` + `tFlushList`(워커 시작 때 `reserve(maxSession)` - 세션은 목록에 최대 한 번이라 재할당 없음, 사용자 우려 반영), `Session::mFlushQueued`, `queueFlush`/`flushSends`, `processRecv` 끝의 `postSend(session)` → `flushSends()`. 경계는 **수신 완료 1건**. 침투형 리스트(`Session::mFlushNext`)는 이 구조를 완성한 뒤 바꿔 보기로(사용자 결정)
    - 정확성: 에코 3.3GB·비정상 7.3만 끊김·받지 않는 클라 넘침 192, 채팅 방 1/4/16 순서 검사 통과, 끊김 0
    - 수정 전·후 비교(`load_sweep -Tag flush_ab`, 수정 전 빌드는 `git worktree` `D:\_Arkchimy_base`(6754260)에서 빌드해 `+base`로 번갈아, 3회, 원본 `실험폴더/LoadClient_chat_2026_11_01/flush_ab_runs.csv`)
      | 워커 | 방 | skip 켬 전 → 후: 전달/s · 패킷/송신 · CPU us/전달 · 방락보유 · RTT p50 | skip 끔 전 → 후: 전달/s |
      |---|---|---|---|
      | 2 | 1 | 117만 → 119만 · 15.1 → 15.1 (같음) | 115만 → 114만 |
      | 2 | 4 | 37만 → **236만 (6.5×)** · 1.29 → 15.5 · 5.48 → 0.84 · 20.3 → 0.87us · 23.8 → 6.5ms | 263만 → 267만 |
      | 2 | 16 | 30만 → **284만 (9.6×)** · 1.06 → 15.7 · 6.64 → 0.72 · 100 → 3.1us · 112 → 13.7ms | 535만 → 567만 |
      | 4 | 1 | 111만 → 111만 (같음) | 108만 → 109만 |
      | 4 | 4 | 49만 → **274만 (5.6×)** · 1.29 → 15.7 · 6.45 → 0.92 · 29.9 → 0.87us · 17.4 → 5.6ms | 228만 → 268만 (+18%) |
      | 4 | 16 | 43만 → **426만 (9.9×)** · 1.06 → 16.1 · 7.54 → 0.78 · 130 → 4.2us · 78 → 10.9ms | 714만 → 749만 |
    - 해석: 다른 세션 송신이 메시지마다 WSASend로 이어지던 문제 해결 - skip 켬 CPU/전달 −85–90%, 방 락 안 WSASend 제거로 락 보유 수 µs. 방 1(에코)은 그대로(대조군). skip 끔도 약간 좋아짐(방 4 워커 4 +18% - 패킷/송신 9.5 → 15.7)
    - **남은 차이**: 방 16에서 skip 끔이 여전히 1.8–2배(패킷/송신 116–126 vs 16). 창이 수신 1건(보낸 사람 1명의 묶음 16개)이라 여러 보낸 사람의 메시지가 합쳐지지 않음. 방 4는 skip 켬·끔이 비슷
    - 이상 1회(README_ERRORS 2-7): 수정 전 빌드 skip 끔 방 16 워커 4에서 정상 클라 넘침 끊김 4 - 방송에서는 받는 쪽 미전송 바이트를 클라 한도로 묶을 수 없음
    - 다음 후보: 경계를 **GQCSEx 묶음 끝**으로 넓혀 여러 보낸 사람 합치기(skip 켬으로 skip 끔 수준을 노림), 그 뒤 침투형 리스트
  - **2단계: 경계를 완료 묶음 끝으로(2026-11-01, 미커밋)**: `server.cfg` `flushPerBatch`(0 = 수신 완료 1건 끝, 기본 / 1 = 워커가 꺼낸 완료 묶음 끝). 워커 루프가 묶음 앞뒤로 `tInBatch` 켜고 끝에서 `flushSends`(종료 신호 묶음도 마저 보냄), `processRecv`는 이미 켜져 있으면(`ownBatch == false`) 보내기를 묶음 끝에 맡김. GQCS는 묶음 = 완료 1건. `load_sweep` 모드 `+fb`. 같은 빌드에서 설정만 바꿔 비교(수정 전 빌드 불필요)
    - 정확성(flushPerBatch = 1): 에코 3.0GB·비정상 7.2만 끊김·받지 않는 클라 넘침 192·채팅 방 1/4/16 순서 검사 통과, 끊김 0. 기본값(0) 에코 3.2GB
    - 측정(`-Tag flush_batch`, 3회, 54회 전부 끊김·넘침 0, 원본 `실험폴더/LoadClient_chat_2026_11_01/flush_batch_runs.csv`)
      | 워커 | 방 | 수신 단위 · skip | **묶음 단위 · skip** | 수신 단위 · skip 끔 |
      |---|---|---|---|---|
      | 2 | 16 | 281만 · 15.6 · 0.73 · 13.7/27.4ms | **711만 · 152 · 0.29 · 5.6/9.0ms** | 573만 · 118 · 0.35 · 9.5/17.2ms |
      | 4 | 16 | 433만 · 16.1 · 0.77 · 10.6/23.6ms | **1043만 · 152 · 0.33 · 3.1/7.1ms** | 772만 · 126 · 0.38 · 7.4/13.6ms |
      | 2 | 4 | 237만 · 15.5 · 0.82 · 6.3ms | **279만 · 18.8 · 0.71 · 4.6ms** | 274만 · 32.6 · 0.70 · 5.5ms |
      | 4 | 4 | 278만 · 15.7 · 0.87 · 5.4ms | 281만 · 16.6 · 0.90 · 5.4ms | 275만 · 15.7 · 0.96 · 5.4ms |
      | 2/4 | 1 | 115만/113만 | 114만/113만 | 114만/113만 |
      (전달/s · 패킷/송신 · CPU us/전달 · 부하 RTT p50/p99)
    - 해석: 묶음 하나(완료 64개)에 여러 보낸 사람이 섞여 받는 사람마다 한 번에 합쳐짐 → 방 16에서 skip 켬이 skip 끔보다 전달 +24%/+35%, CPU/전달 −18%/−14%, RTT p50·p99 모두 더 짧음. **설계한 창(묶음 처리 시간)이 우연한 창(송신 완료 대기 약 5ms)보다 넓고 지연은 짧다**. 방 4 워커 4·방 1은 세 방식이 같음(대조)
    - 서버 포화: 워커 2는 2.04/2(포화), 워커 4 묶음 끝은 3.43/4. 클라는 최대 2.41/4로 포화 아님
    - 주의: 창이 넓어진 만큼 받는 쪽 미전송 바이트가 묶음 처리 동안 쌓인다 - LoadClient(보낸 사람마다 묶음 512B, 방 16 최대 약 8KB)에서는 넘침 0이지만, 큰 묶음을 보내는 컨텐츠에서는 한도(16KB)에 닿을 수 있다(README_ERRORS 2-7과 같은 문제)
    - 남은 결정: 기본값을 flushPerBatch = 1로 바꿀지(사용자). 그 뒤 침투형 리스트
  - **2×2 측정(보내는 단위 × skip, 2026-11-01)**: 사용자가 "묶음 단위 · skip 끔"이 빠졌다고 지적 → 네 조합을 같은 실행에서 3회(`-Tag flush_2x2b`, 72회, 원본 `flush_2x2_runs.csv`). 첫 시도는 서버 기동 실패로 1회차에서 멈춤(README_ERRORS 1-17) → `load_sweep`에 대기·재시도 추가 후 재측정
    - 방 16 전달/s (워커 2 / 4): 수신 단위 · skip 285만 / 434만, **묶음 단위 · skip 705만 / 1041만**, 수신 단위 · skip 끔 565만 / 768만, 묶음 단위 · skip 끔 556만 / 784만. WSASend당 메시지 16 / 152 / 118–126 / 117–126
    - **skip 끔에서는 묶음 단위가 효과 없음**: 송신 중 표시가 송신 완료 처리까지(4.6–6.4ms) 켜져 있어, 묶음 끝에 flushSends를 해도 postSend가 그냥 돌아가고 결국 완료 처리 때 나간다 → 우연한 창이 지배. skip 켬에서만 설계한 창(묶음)이 작동하고, 그게 우연한 창보다 넓고 지연도 짧다(방 16 RTT p50 5.8/3.2ms vs 9.7–10/7.4–7.5ms)
    - 3방식 측정(flush_batch)과 값이 일치(묶음 단위 · skip 방 16: 711만→705만, 1043만→1041만)
    - **정상 클라 넘침 끊김**: 워커 4 · 방 16에서 묶음 단위 · skip 끔 1회 206건, 묶음 단위 · skip 2회 각 1건. 커널 버퍼는 여유(즉시 성공 100%, 대기 0) - **서버가 그 받는 사람에게 WSASend를 안 불러 우리 링의 안 넘긴 몫이 16KB를 넘음**. skip 끔은 송신 중 표시(완료 처리 지연), skip 켬은 `mFlushQueued`를 쥔 워커의 긴 묶음 동안 다른 워커들이 여러 바퀴 쌓은 것(설계 허점). 고칠 후보: 조기 보내기(안 넘긴 몫 > 한도/2면 즉시 postSend). README_ERRORS 2-7. 기본값을 묶음 단위로 바꾸기 전에 해결

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
- (10-25~26) 처리량 전용 LoadClient, 측정 환경 고정(전원 모드·회차 순서 회전·중앙값), 워커 수별 처리량과 코어당 효율 저하 원인(자연 배칭 기각, 클럭 ~44%)
- (10-25~26) GQCS vs GQCSEx: 별도 라이브러리로 비교, 프로파일러 혼입 변수 제거, 패킷당 비용 분해로 "이득의 76%는 WSASend 호출 감소 = 송신 묶임"을 밝힘
- (10-25~26) 송신 버퍼 0·메시지 크기 실험: WSASend 비용은 크기가 아니라 호출 횟수가 정함
- (10-25~26) skipOnSuccess: 최적화가 −82%로 역효과 → 4KB×1 대조군으로 "완료 통지 절감 ≈ 0, 손해는 송신 묶임 상실"로 분리 → 처리량이 우연한 묶임에 기대고 있었음을 발견
- (10-26) 정상 클라 간헐 넘침 끊김 발견(넘침 정책이 완료 처리 지연까지 셈) → 송신 큐 재설계로 이어짐
- 측정 해석 실수와 교정 기록(README_ERRORS 4장) 자체도 "측정을 의심하는 습관"의 근거
