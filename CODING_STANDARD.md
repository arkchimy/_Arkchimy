# C++ 코딩 규칙

이 저장소의 C++ 코드는 [포프의 C++ 코딩 표준](https://docs.popekim.com/ko/coding-standards/cpp)을 따른다. 이 파일은 표준을 이 프로젝트에 어떻게 적용하는지(해석, 뺀 것, 더한 것)만 적는다. 표준 본문은 링크를 본다.

- 적용 범위(2026-11-02 사용자 결정, 2026-11-11 폴더 구조 변경): `_lib/NetworkLib/`(네트워크 라이브러리), 서버·클라 공용 헤더(`_lib/ProtocolLib/`), 컨텐츠 폴더의 서버와 프로토콜(`Echo/Server/`, `Echo/Protocol/`처럼 `<컨텐츠>/Server/`, `<컨텐츠>/Protocol/`)
- 범위 밖: 시험 클라(`<컨텐츠>/Client/`) - 서식은 맞춰 쓰지만 훅은 검사하지 않는다
- 범위 밖: `EchoClient/`, `LoadClient/`, `tools/`, `_lib/`의 나머지 라이브러리(ParserLib, ProfilerLib, ExeNameLib, StatLib). 공용 헤더의 이름이 바뀌면 그 참조만 고친다
- POCU C 표준은 C 파일용이라 이 저장소에는 적용하지 않는다(C++ 표준과 이름·널·중괄호 규칙이 서로 다르다)

## 꼭 지키는 것 (요약)

- 이름
  - 클래스·구조체: `PascalCase`
  - 지역 변수·매개변수: `camelCase`
  - 메서드: 동사-목적어. public은 `PascalCase`, 그 외(protected 아닌 private, 파일 안 static 함수)는 `camelCase`
  - bool을 반환하는 조회 메서드: `Is`/`Can`/`Has`/`Should`로 시작(`IsRunning`)
  - 멤버 변수 `m`, bool 멤버 `mb`, bool 변수 `b`, static 변수 `s`(bool이면 `sb`)
  - 상수(`constexpr`, `#define`): `UPPER_SNAKE`
  - 열거형 `e` 접두어 + `enum class`
  - out 매개변수: 포인터 + `out` 접두어, 함수 안에서 `Assert`로 null 검사
  - 약어(첫 글자를 딴 말 - ID, IP, IOCP, GQCS, QPC, WSA, RAII, DB, SQL, UID)는 뒤에 단어가 없으면 전부 대문자(`mID`, `initIOCP`, `GameDB`, `MakeRecallSQL`), 뒤에 단어가 있으면 첫 글자만(`mbGqcsEx`, `mWsaSendQPC`, `DbThread`, `toSqlNumber`) - DB·SQL은 2026-11-14 사용자 결정으로 약어에 넣음
- 구조체는 public 데이터 멤버만, 멤버는 `PascalCase`, 함수 없음. 함수(생성자 포함)가 있으면 클래스로
- 클래스 멤버 변수는 직접 접근하지 않고 getter/setter
- `nullptr`이 올 수 없으면 포인터 대신 참조. 클래스 안에 저장하는 매개변수는 포인터
- `const`는 원칙적으로 모든 곳 - 지역 변수, 값 매개변수, 객체를 바꾸지 않는 멤버 함수
- `switch`에는 항상 `default:` - 오면 안 되는 `default`는 `Assert(false)`
- `override`, `final`(더 상속하지 않는 클래스), `nullptr`, `constexpr`, 범위 기반 `for`
- `auto`는 반복자, 같은 줄에서 타입이 보이는 `New`, 이름을 쓸 수 없는 람다 타입에만
- 변수 가리기(shadowing) 금지, 한 줄에 변수 하나, 컴파일러 경고 0
- 지역 객체를 값으로 돌려주는 함수는 `return`을 하나만(NRVO)
- 서식
  - 여는 중괄호는 항상 새 줄(한 줄짜리 함수·블록도), 한 줄이어도 중괄호
  - 들여쓰기는 탭(Visual Studio 기본)
  - 포인터·참조 기호는 타입 쪽(`int& number`)
  - 외부 헤더 `<>` 먼저(가능하면 알파벳 순), 자체 헤더 `""`, include와 본문 사이 빈 줄 하나, 헤더 첫 줄 `#pragma once`
  - 초기화 리스트: 콜론은 다음 줄, 쉼표는 줄 앞, 한 줄에 멤버 하나
  - 클래스 안 순서: friend → public 메서드 → protected 메서드 → private 메서드 → protected 변수 → private 변수

## 이 프로젝트에서 만든 것 (표준이 "자체 구현"을 요구하는 것)

`_lib/NetworkLib/Assert.h`, `_lib/NetworkLib/Memory.h`

- `Assert(조건, "설명")`: 디버그·릴리즈 모두 검사한다(2026-11-11 사용자 결정). 거짓이면 파일·줄·조건·설명을 `stderr`에 남기고 `gAssertMessage`(덤프에서 볼 값)에 설명을 넣은 뒤 `__debugbreak()`로 멈춘다
  - 설명은 필수 문자열 상수 - "무엇이 깨졌는지"를 한 줄로(`"OnAccept: 이전 세션의 Player가 정리되지 않음"`)
  - 프로그램 오류(불변식)에만 쓰고, 상대 클라·설정값처럼 밖에서 오는 값은 `if`로 검사한다 - 밖에서 오는 값으로 프로세스가 멈추면 안 된다
  - 디버거 없이 멈추면 Visual Studio가 설치된 PC에서는 "디버그하시겠습니까?" 창이 뜬 채 프로세스가 기다릴 수 있다(덤프 처리기를 만들 때 다시 본다)
- `MemCpy`, `MemSet`, `MemMove`: 표준 함수를 감싸고 포인터 `Assert`를 한다. `memcpy`, `memset`, `memmove`, `ZeroMemory`는 직접 쓰지 않는다
- `New<T>(인자...)`, `Delete(포인터)`, `NewArray<T>(개수)`, `DeleteArray(포인터)`: 모든 직접 할당은 이것으로. 지금은 `new`/`delete`를 그대로 부르지만, 할당자를 바꿀 때 이 함수만 고치면 된다
- `UniquePtr<T>` / `MakeUnique<T>(...)`, `UniqueArray<T>` / `MakeUniqueArray<T>(개수)`: `New`/`Delete`를 쓰는 `std::unique_ptr`. `std::make_unique`는 쓰지 않는다

## 해석과 예외 (표준에 없거나 이 프로젝트에 맞게 정한 것)

- **thread_local 변수는 `t` 접두어**(bool이면 `tb`) - 표준에 없는 프로젝트 규칙. 스레드마다 따로라는 뜻이 static과 달라 구분한다(`tWorkerIdx`, `tSendTargets`)
- **콜백·이벤트 처리 함수는 `On` 접두어 허용**(`OnRecv`, `onRecvComplete`, `onConsoleCtrl`) - "무슨 일이 일어났을 때"라는 뜻이 동사-목적어보다 분명하다
- **bool을 돌려주는 동작 함수는 동사 그대로**(`Start`, `Push`, `SendPacket`, `tryIncIoCount`) - `Is`/`Has` 규칙은 상태를 묻는 조회 함수에만 적용한다
- **줄임말(Recv, Buf, Cnt, Idx, Ov, Sock)은 약어 규칙 대상이 아니다** - 첫 글자를 딴 약어만 대문자로 바꾼다. 줄임말은 지금 이름을 유지한다(사용자가 정한 `mIdx`, `AcceptOv` 등)
- **`Session`은 클래스 + `friend class NetworkLib`** - 라이브러리 안에서만 쓰는 상태 묶음이라 getter/setter 대신 friend로 연다. 멤버는 `m` 접두어
- **`const`는 값 매개변수와 지역 변수, 가리키는 대상까지** - 포인터 매개변수 자체에는 붙이지 않는다(`const char* payload`는 쓰고 `const char* const payload`는 쓰지 않음)
- **표준 컨테이너(`std::vector` 등)는 자기 할당자를 쓴다** - `New`/`Delete` 규칙은 직접 할당에만
- **헤더 순서의 예외**: `<WinSock2.h>`는 `<MSWSock.h>`보다 먼저 와야 컴파일되므로 알파벳 순서보다 우선
- **상수 묶음은 익명 `enum : int32_t`**(사용자 결정, 2026-11-02 `CHAT_MAX_PAYLOAD`, 2026-11-04 `RECV_RING_SIZE`/`SEND_RING_SIZE`) - "항상 `enum class`" 규칙의 예외. 타입 이름 없이 값만 상수로 쓰는 묶음에 한한다 - 이름을 붙이면 `e` 접두어 + `enum class` 규칙을 따라야 하고, `enum class`는 정수로 자동 변환되지 않아 템플릿 인자·크기 비교마다 형 변환이 필요하다
- **라이브러리(`NetworkLib`)의 스레드 공유 변수는 `volatile LONG` / `volatile LONG64` + Interlocked 함수**(사용자 결정, 2026-11-04 - `std::atomic` 사용법이 어색하다는 이유로 바꿈)
  - 쓰기는 반드시 `InterlockedIncrement`/`InterlockedDecrement`/`InterlockedExchange`/`InterlockedCompareExchange`(64비트는 `…64`)로 한다. **일반 대입(`=`), `++`, `--`, `+=`는 쓰지 않는다** - 컴파일은 되지만 원자적이지 않고, Interlocked 함수가 주는 앞뒤 메모리 순서 보장도 없다. `std::atomic`과 달리 컴파일러가 막아 주지 않으므로 훅(`check_cpp_style.ps1`)과 cpp-reviewer가 잡는다
  - 읽기는 그냥 읽는다 - x64에서 정렬된 4/8바이트 `volatile` 읽기는 한 번에 일어나고, MSVC(`/volatile:ms`, x64 기본)에서는 뒤따르는 접근이 앞당겨지지 않는다
  - bool 뜻의 플래그도 `volatile LONG`(0/1)로 둔다(Interlocked 함수가 `bool`용으로 없다). 이름은 bool 규칙대로 `mb` 접두어
  - 예외: 쓰는 스레드가 하나뿐인 통계 카운터(`WorkerCounter`)는 `AddSingleWriter`(읽고 쓰기)로 올린다 - lock 접두 명령을 피하려는 것
  - 컨텐츠(채팅 서버)는 `std::atomic` 그대로(범위 밖)
- **패킷 구조체(`PacketHeader`, `ChatMsg` 등)의 멤버도 `PascalCase`** - 서버·클라 공용이라 클라의 참조도 함께 바꾼다
