#include "ListenSocket.h"
#include "Assert.h"

SOCKET CreateListenSocket(const USHORT port, const int backlog, const char** outStep, int32_t* outErr)
{
	Assert(outStep != nullptr);
	Assert(outErr != nullptr);

	const SOCKET listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (listenSock == INVALID_SOCKET)
	{
		*outStep = "socket";
		*outErr = WSAGetLastError();
		return INVALID_SOCKET;
	}

	SOCKADDR_IN addr{};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons(port);

	if (bind(listenSock, reinterpret_cast<SOCKADDR*>(&addr), sizeof(addr)) == SOCKET_ERROR)
	{
		*outStep = "bind";
		*outErr = WSAGetLastError(); // closesocket가 덮어쓰기 전에 읽는다
		closesocket(listenSock);
		return INVALID_SOCKET;
	}

	if (listen(listenSock, SOMAXCONN_HINT(backlog)) == SOCKET_ERROR)
	{
		*outStep = "listen";
		*outErr = WSAGetLastError();
		closesocket(listenSock);
		return INVALID_SOCKET;
	}

	return listenSock;
}
