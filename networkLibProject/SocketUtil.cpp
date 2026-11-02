#include "SocketUtil.h"	// WinSock2.h가 windows.h보다 먼저 와야 해서 Profiler.h보다 앞
#include "Profiler.h"

void CloseReset(const SOCKET sock)
{
	LINGER linger{};
	linger.l_onoff = 1;
	linger.l_linger = 0;

	// 의심 4: 두 서버 공통 경로. 스레드별로 출력되므로 accept/acceptex 각각의 값을 비교할 수 있음
	{
		ProfileScope setLingerScope("CloseReset_SetLinger");
		setsockopt(sock, SOL_SOCKET, SO_LINGER, reinterpret_cast<const char*>(&linger), sizeof(linger));
	}

	ProfileScope closeScope("CloseReset_CloseSocket");
	closesocket(sock);
}

bool IsIfsTcpSocket()
{
	const SOCKET sock = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
	if (sock == INVALID_SOCKET)
	{
		return false;
	}
	WSAPROTOCOL_INFOW info{};
	int len = sizeof(info);
	const bool bIfs = getsockopt(sock, SOL_SOCKET, SO_PROTOCOL_INFOW, reinterpret_cast<char*>(&info), &len) == 0 &&
		(info.dwServiceFlags1 & XP1_IFS_HANDLES) != 0;
	closesocket(sock);
	return bIfs;
}
