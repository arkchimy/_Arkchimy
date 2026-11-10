#pragma once
#include <WinSock2.h>

// 완료 종류 - 워커가 꺼낸 OVERLAPPED의 Type으로 처리 함수를 고른다
enum class eIoType
{
	Accept,
	Recv,
	Send,
	Release, // PQCS로 넣는 세션 끊는 작업
	Job,     // 컨텐츠가 PostJobComplete로 넣는 작업 완료(DB 등)
	None,
};

// GQCS에서 받은 OVERLAPPED*를 static_cast로 되돌려 Type을 읽는다
struct MyOverlapped : OVERLAPPED
{
	eIoType Type = eIoType::None;
};

// 작업 완료용 OVERLAPPED (공개) - 컨텐츠의 작업 객체가 상속하고, OnJobComplete에서 static_cast로 되돌린다
// 안의 값은 라이브러리가 쓴다(컨텐츠는 건드리지 않는다)
// OnJobComplete가 불리기 전에 지우면 안 된다 - 그때까지 IOCP 큐에 이 주소가 들어 있다
struct JobOverlapped : MyOverlapped
{
};
