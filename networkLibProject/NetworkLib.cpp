#include "NetworkLib.h"
#include <cstdio>
#include "../_lib/ParserLib/ParserLib.h"

WsadataRAII::WsadataRAII()
{
	WSADATA wsa;
	// WSAStartup은 WSAGetLastError가 아니라 반환값으로 에러를 돌려준다
	int err = WSAStartup(MAKEWORD(2, 2), &wsa);
	if (err != 0)
	{
		printf("WSAStartup 실패 err=%d\n", err);
		return;
	}
	mOk = true;
}

WsadataRAII::~WsadataRAII()
{
	if (mOk)
    {
        WSACleanup();
    }
}

// 리슨 소켓 생성 → bind → listen
bool NetworkLib::initListenSocket(USHORT port, int backlog)
{
	mListenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (mListenSock == INVALID_SOCKET)
	{
		printf("socket 실패 err=%d\n", WSAGetLastError());
		return false;
	}

	SOCKADDR_IN addr{};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons(port);

	if (bind(mListenSock, reinterpret_cast<SOCKADDR*>(&addr), sizeof(addr)) == SOCKET_ERROR)
	{
		printf("bind 실패 err=%d\n", WSAGetLastError());
		closesocket(mListenSock);
		mListenSock = INVALID_SOCKET;
		return false;
	}

	if (listen(mListenSock, SOMAXCONN_HINT(backlog)) == SOCKET_ERROR)
	{
		printf("listen 실패 err=%d\n", WSAGetLastError());
		closesocket(mListenSock);
		mListenSock = INVALID_SOCKET;
		return false;
	}
	return true;
}
