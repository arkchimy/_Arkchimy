#include "SocketUtil.h"
#include "Profiler.h"

void closeReset(SOCKET s)
{
	LINGER linger{};
	linger.l_onoff = 1;
	linger.l_linger = 0;

	// 의심 4: 두 서버 공통 경로. 스레드별로 출력되므로 accept/acceptex 각각의 값을 비교할 수 있음
	{
		ProfileScope scope("CloseReset_SetLinger");
		setsockopt(s, SOL_SOCKET, SO_LINGER, reinterpret_cast<const char*>(&linger), sizeof(linger));
	}

	ProfileScope scope("CloseReset_CloseSocket");
	closesocket(s);
}

bool isIfsTcpSocket()
{
	SOCKET s = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
	if (s == INVALID_SOCKET)
	{
		return false;
	}
	WSAPROTOCOL_INFOW info{};
	int len = sizeof(info);
	bool ifs = getsockopt(s, SOL_SOCKET, SO_PROTOCOL_INFOW, reinterpret_cast<char*>(&info), &len) == 0 &&
		(info.dwServiceFlags1 & XP1_IFS_HANDLES) != 0;
	closesocket(s);
	return ifs;
}
