#include "FloodClient.h"
#include <cstdio>

FloodClient::~FloodClient()
{
	stop();
}

void FloodClient::stop()
{
	if (!mRunning)
	{
		return;
	}

	mRunning = false;
	for (auto& worker : mWorkers)
	{
		worker.join();
	}
	mStatThread.join();
}

bool FloodClient::init(const std::string& ip, USHORT port, int threadCnt)
{
	mTargetAddr.sin_family = AF_INET;
	mTargetAddr.sin_port = htons(port);
	if (inet_pton(AF_INET, ip.c_str(), &mTargetAddr.sin_addr) != 1)
	{
		return false;
	}

	mThreadCnt = threadCnt;
	mRunning = true;
	for (int i = 0; i < threadCnt; ++i)
	{
		mWorkers.emplace_back(&FloodClient::workerLoop, this);
	}
	mStatThread = std::thread(&FloodClient::statLoop, this);
	return true;
}

void FloodClient::workerLoop()
{
	++mReadyCount;
	while (mReadyCount.load() < mThreadCnt) {}	// 모든 워커가 준비될 때까지 대기 - 마지막 워커가 증가시키는 순간 전부 통과

	while (mRunning)
	{
		SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
		if (s == INVALID_SOCKET)
		{
			continue;
		}

		//u_long on = 1;
		//ioctlsocket(s, FIONBIO, &on);	// 논블로킹 전환 - connect()가 SYN만 보내고 즉시 반환되게 함

		connect(s, reinterpret_cast<sockaddr*>(&mTargetAddr), sizeof(mTargetAddr));	// handshake 완료 여부는 확인 안 함

		LINGER linger{};
		linger.l_onoff = 1;
		linger.l_linger = 0;
		setsockopt(s, SOL_SOCKET, SO_LINGER, reinterpret_cast<const char*>(&linger), sizeof(linger));
		closesocket(s);	// SYN-SENT 상태에서 즉시 닫음 - 혹시 그 사이 handshake가 끝났더라도 TIME_WAIT 없이 RST로 정리

		++mAttemptCount;
	}
}

void FloodClient::statLoop()
{
	long long last = 0;
	long long elapsedSec = 0;
	bool started = false;
	while (mRunning)
	{
		Sleep(1000);
		long long now = mAttemptCount.load();
		started = started || (now > 0);	// 첫 이벤트가 발생한 뒤부터만 경과 시간을 셈
		if (started)
		{
			++elapsedSec;
		}
		long long avg = started ? now / elapsedSec : 0;
		printf("[FloodClient] 연결시도 tps=%lld avg=%lld total=%lld\n", now - last, avg, now);
		last = now;
	}
}
