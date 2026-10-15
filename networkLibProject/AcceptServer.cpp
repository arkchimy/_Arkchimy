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
		++mAcceptCount;	// 성공/실패 상관없이 accept 반환 수를 센다
		if (client == INVALID_SOCKET)
		{
			continue;	// 종료 시엔 stop()이 mRunning을 먼저 내리므로 while 조건에서 빠져나감
		}

		closeReset(client);
	}
}

void AcceptServer::statLoop()
{
	long long last = 0;
	long long elapsedSec = 0;
	bool started = false;
	while (mRunning)
	{
		Sleep(1000);
		long long now = mAcceptCount.load();
		started = started || (now > 0);	// 첫 이벤트가 발생한 뒤부터만 경과 시간을 셈
		if (started)
		{
			++elapsedSec;
		}
		long long avg = started ? now / elapsedSec : 0;
		printf("[AcceptServer] 반환 tps=%lld avg=%lld total=%lld\n", now - last, avg, now);
		last = now;
	}
}
