#include "AcceptServer.h"
#include "ListenSocket.h"
#include "SocketUtil.h"
#include <cstdio>

bool AcceptServer::init(USHORT port, int backlog)
{
	mListenSock = createListenSocket(port, backlog);
	if (mListenSock == INVALID_SOCKET)
	{
		return false;
	}

	mRunning = true;
	mAcceptThread = std::thread(&AcceptServer::acceptLoop, this);
	mStatThread = std::thread(&AcceptServer::statLoop, this);
	return true;
}

void AcceptServer::stop()
{
	if (!mRunning)
	{
		return;
	}

	mRunning = false;
	closesocket(mListenSock);	// 블로킹 accept()를 깨우기 위해 리슨 소켓을 닫음
	mAcceptThread.join();
	mStatThread.join();	// statLoop는 sleep_for 끝나는 시점에 mRunning 보고 빠져나감
}

AcceptServer::~AcceptServer()
{
	stop();
}

void AcceptServer::acceptLoop()
{
	while (mRunning)
	{
		SOCKET client = accept(mListenSock, nullptr, nullptr);
		if (client == INVALID_SOCKET)
		{
			break;	// 종료 시 리슨 소켓을 닫으면 accept가 에러로 깨어남
		}

		closeReset(client);
		++mAcceptCount;
	}
}

void AcceptServer::statLoop()
{
	long long last = 0;
	while (mRunning)
	{
		Sleep(1000);
		long long now = mAcceptCount.load();
		printf("[AcceptServer] TPS=%lld\n", now - last);
		last = now;
	}
}
