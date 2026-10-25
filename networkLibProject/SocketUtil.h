#pragma once
#include <WinSock2.h>

// SO_LINGER(0)로 즉시 RST를 보내고 닫는다 - TIME_WAIT 없이 바로 정리
void closeReset(SOCKET s);
