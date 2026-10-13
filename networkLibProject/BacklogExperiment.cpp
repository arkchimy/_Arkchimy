// backlog 큐가 가득 찬 뒤 Windows가 새 연결을 어떻게 처리하는지 확인하는 실험 (T1-7 변수 통제 근거)
// 서버: listen만 하고 accept는 하지 않는다 → 큐가 차는 순간을 만든다
#include "NetworkLib.h"
#include <cstdio>

// 방화벽 팝업을 피하려고 루프백에만 bind 한다
static SOCKADDR_IN makeLoopbackAddr(USHORT port)
{
	SOCKADDR_IN addr{};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = htons(port);
	return addr;
}

int runBacklogServer(USHORT port, int backlog, bool useHint, int seconds, int acceptDelayMs)
{
	SOCKET listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (listenSock == INVALID_SOCKET)
	{
		printf("socket 실패 err=%d\n", WSAGetLastError());
		return 1;
	}

	SOCKADDR_IN addr = makeLoopbackAddr(port);
	if (bind(listenSock, reinterpret_cast<SOCKADDR*>(&addr), sizeof(addr)) == SOCKET_ERROR)
	{
		printf("bind 실패 err=%d\n", WSAGetLastError());
		closesocket(listenSock);
		return 1;
	}

	int listenArg = useHint ? SOMAXCONN_HINT(backlog) : backlog;
	if (listen(listenSock, listenArg) == SOCKET_ERROR)
	{
		printf("listen 실패 err=%d\n", WSAGetLastError());
		closesocket(listenSock);
		return 1;
	}

	printf("[server] backlog=%d hint=%d → %d초 동안 accept 없이 대기\n"
		, backlog, useHint ? 1 : 0, seconds);

	// acceptDelayMs만큼 기다린 뒤 accept()를 1번만 호출해 큐 슬롯 1개를 비운다
	// (RST 재시도 중인 클라이언트가 이 빈 슬롯을 잡는지 확인하는 실험용)
	if (acceptDelayMs >= 0)
	{
		Sleep(acceptDelayMs);
		SOCKET accepted = accept(listenSock, nullptr, nullptr);
		if (accepted == INVALID_SOCKET)
		{
			printf("[server] accept 실패 err=%d\n", WSAGetLastError());
		}
		else
		{
			printf("[server] accept 성공 → 큐 슬롯 1개 반환\n");
			closesocket(accepted);
		}
	}

	Sleep(seconds * 1000);
	closesocket(listenSock);
	return 0;
}
