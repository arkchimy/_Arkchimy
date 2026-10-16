#include "FloodClient.h"
#include "Profiler.h"
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
	int idx = mReadyCount++;

	// 이름 지정은 대기 전에 (ThreadProfile 생성/등록 비용이 측정 구간에 섞이지 않게)
	// 스레드 이름은 ConnectEx 버전(FloodClient)과 같게 맞춤
	char threadName[MAX_THREAD_NAME_LEN];
	sprintf_s(threadName, MAX_THREAD_NAME_LEN, "FloodWorker_%d", idx);
	Profile::SetThreadName(threadName);

	// 디버거(스레드 창)/덤프에서 보이는 이름 (와이드 문자열)
	wchar_t threadDesc[MAX_THREAD_NAME_LEN];
	swprintf_s(threadDesc, MAX_THREAD_NAME_LEN, L"FloodWorker_%d", idx);
	SetThreadDescription(GetCurrentThread(), threadDesc);

	while (mReadyCount.load() < mThreadCnt) {}	// 모든 워커가 준비될 때까지 대기 - 마지막 워커가 증가시키는 순간 전부 통과

	// 태그 이름 규칙: ConnectEx 버전과 공통 단계는 같은 이름 → 두 결과 파일을 같은 행끼리 비교
	while (mRunning)
	{
		// 시도 1건 전체 (ConnectEx 버전의 Flood_GQCS + Flood_Complete에 대응)
		ProfileScope loopScope("Flood_Loop");

		// 공통
		SOCKET s = INVALID_SOCKET;
		{
			ProfileScope scope("Flood_Socket");
			s = socket(AF_INET, SOCK_STREAM, 0);
		}

		if (s == INVALID_SOCKET)
		{
			continue;
		}

		//u_long on = 1;
		//ioctlsocket(s, FIONBIO, &on);	// 논블로킹 전환 - connect()가 SYN만 보내고 즉시 반환되게 함

		// 블로킹 전용: 암묵적 bind + SYN 송신 + 핸드셰이크 대기 + 반환이 한 호출 안에 있음
		// (ConnectEx 버전의 Flood_Bind + Flood_IocpRegister + Flood_ConnectEx + Flood_GQCS + Flood_UpdateContext에 대응)
		{
			ProfileScope scope("Flood_Connect");
			connect(s, reinterpret_cast<sockaddr*>(&mTargetAddr), sizeof(mTargetAddr));	// handshake 완료 여부는 확인 안 함
		}

		LINGER linger{};
		linger.l_onoff = 1;
		linger.l_linger = 0;

		// 공통
		{
			ProfileScope scope("Flood_SetLinger");
			setsockopt(s, SOL_SOCKET, SO_LINGER, reinterpret_cast<const char*>(&linger), sizeof(linger));
		}

		// 공통
		{
			ProfileScope scope("Flood_CloseSocket");
			closesocket(s);	// SYN-SENT 상태에서 즉시 닫음 - 혹시 그 사이 handshake가 끝났더라도 TIME_WAIT 없이 RST로 정리
		}

		++mAttemptCount;
	}
}

void FloodClient::statLoop()
{
	// 디버거(스레드 창)/덤프에서 보이는 이름
	SetThreadDescription(GetCurrentThread(), L"FloodStat");

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
