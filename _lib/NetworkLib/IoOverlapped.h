#pragma once
#include <WinSock2.h>
#include "JobOverlapped.h"

// AcceptEx 슬롯 - 완료되면 같은 슬롯으로 다시 건다
struct AcceptOv : MyOverlapped
{
	SOCKET AcceptSocket = INVALID_SOCKET;
	char Buffer[64] = {}; // 로컬/원격 주소가 써지는 곳 (수신 데이터는 안 받음)
};
