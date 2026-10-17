#include "AcceptServer.h"
#include "ListenSocket.h"
#include "SocketUtil.h"
#include "Profiler.h"
#include "StatUtil.h"
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
	Profile::SetThreadName("AcceptThread");

	while (mRunning)
	{
		// 비교 기준: 대기 + 커널 내부 소켓 생성 + 반환 (AcceptEx_GQCS + AcceptEx_Socket + AcceptEx_Post에 대응)
		SOCKET client = INVALID_SOCKET;
		{
			ProfileScope scope("Accept_Accept");
			client = accept(mListenSock, nullptr, nullptr);
		}
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
	StatTicker ticker;
	CpuSampler cpu;
	long long last = 0;
	double elapsedSec = 0.0;
	bool started = false;
	while (mRunning)
	{
		double intervalSec = ticker.waitNext();	// 실제 구간 길이 (QPC)
		CpuUsage usage = cpu.sample();
		long long now = mAcceptCount.load();
		started = started || (now > 0);	// 첫 이벤트가 발생한 뒤부터만 경과 시간을 셈
		if (started)
		{
			elapsedSec += intervalSec;
		}
		double tps = static_cast<double>(now - last) / intervalSec;
		double avg = started ? static_cast<double>(now) / elapsedSec : 0.0;
		printf("[AcceptServer] 반환 tps=%.0f avg=%.0f total=%lld | cpu 프로세스=%.1f%% (유저 %.1f 커널 %.1f) 시스템=%.1f%%\n",
			tps, avg, now, usage.processUser + usage.processKernel, usage.processUser, usage.processKernel, usage.system);
		last = now;
	}
}
