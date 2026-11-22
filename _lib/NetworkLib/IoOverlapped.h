#pragma once
#include <WinSock2.h>

// 완료 종류 - 워커가 꺼낸 OVERLAPPED의 Type으로 처리 함수를 고른다
enum class eIoType
{
	Accept,
	Recv,
	Send,
	Release,      // PQCS로 넣는 세션 끊는 작업
	RecvContinue, // PQCS로 넣는 수신 이어 하기 - 즉시 완료가 상한(SkipLoopLimit)만큼 이어져 차례를 큐 뒤로 넘긴 것. mRecvOv를 다시 쓴다
	None,
};

// GQCS에서 받은 OVERLAPPED*를 static_cast로 되돌려 Type을 읽는다
struct MyOverlapped : OVERLAPPED
{
	eIoType Type = eIoType::None;
};

// AcceptEx 슬롯 - 완료되면 같은 슬롯으로 다시 건다
struct AcceptOv : MyOverlapped
{
	SOCKET AcceptSocket = INVALID_SOCKET;
	char Buffer[64] = {}; // 로컬/원격 주소가 써지는 곳 (수신 데이터는 안 받음)
};
