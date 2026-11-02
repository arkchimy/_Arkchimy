#pragma once
#include <WinSock2.h>

// socket -> bind -> listen까지 공통 처리. 실패 시 INVALID_SOCKET
SOCKET CreateListenSocket(const USHORT port, const int backlog);
