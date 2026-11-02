#include <cstdio>
#include "ListenSocket.h"

SOCKET CreateListenSocket(const USHORT port, const int backlog)
{
	const SOCKET listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (listenSock == INVALID_SOCKET)
	{
		printf("socket 실패 err=%d\n", WSAGetLastError());
		return INVALID_SOCKET;
	}

	SOCKADDR_IN addr{};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons(port);

	if (bind(listenSock, reinterpret_cast<SOCKADDR*>(&addr), sizeof(addr)) == SOCKET_ERROR)
	{
		printf("bind 실패 err=%d\n", WSAGetLastError());
		closesocket(listenSock);
		return INVALID_SOCKET;
	}

	if (listen(listenSock, SOMAXCONN_HINT(backlog)) == SOCKET_ERROR)
	{
		printf("listen 실패 err=%d\n", WSAGetLastError());
		closesocket(listenSock);
		return INVALID_SOCKET;
	}

	return listenSock;
}
