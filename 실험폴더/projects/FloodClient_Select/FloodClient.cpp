#include "FloodClient.h"
#include "Profiler.h"
#include <iostream>

FloodClient::~FloodClient()
{
	stop();
}

bool FloodClient::init(const std::string& ip, USHORT port, int threadCnt, int selectTimeoutMs)
{
	mTargetAddr.sin_family = AF_INET;
	mTargetAddr.sin_port = htons(port);
	if (inet_pton(AF_INET, ip.c_str(), &mTargetAddr.sin_addr) != 1)
	{
		return false;
	}

	mThreadCnt = threadCnt;
	mSelectTimeoutMs = selectTimeoutMs;
	mRunning = true;
	for (int i = 0; i < threadCnt; ++i)
	{
		mWorkers.emplace_back(&FloodClient::workerLoop, this);
	}
	mStatThread = std::thread(&FloodClient::statLoop, this);
	return true;
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

void FloodClient::workerLoop()
{
	int idx = mReadyCount++;

	// 이름 지정은 대기 전에 (ThreadProfile 생성/등록 비용이 측정 구간에 섞이지 않게)
	std::string threadName = "FloodWorker_" + std::to_string(idx);
	Profile::SetThreadName(threadName.c_str());

	// 디버거(스레드 창)/덤프에서 보이는 이름 (와이드 문자열 필요)
	std::wstring threadDesc = L"FloodWorker_" + std::to_wstring(idx);
	SetThreadDescription(GetCurrentThread(), threadDesc.c_str());

	// 스레드마다 별개의 세션 64개와 정리 목록
	std::unordered_map<SOCKET, SessionState> sessions;
	std::vector<SOCKET> closeSockets;
	closeSockets.reserve(SESSION_PER_THREAD);
	for (int i = 0; i < SESSION_PER_THREAD; ++i)
	{
		createSession(sessions);
	}

	while (mReadyCount.load() < mThreadCnt) {}	// 모든 워커가 준비될 때까지 대기

	while (mRunning)
	{
		ProfileScope loopScope("Flood_Loop");	// connect 요청 + select 한 바퀴
		connectSessions(sessions, closeSockets);
		selectSessions(sessions, closeSockets);
	}

	// 종료: 남은 소켓 정리
	for (const auto& [s, state] : sessions)
	{
		closesocket(s);
	}
}

void FloodClient::createSession(std::unordered_map<SOCKET, SessionState>& sessions)
{
	SOCKET s = INVALID_SOCKET;
	{
		ProfileScope scope("Flood_Socket");	// 임시: 클라 루프 지연 원인 확인용
		s = socket(AF_INET, SOCK_STREAM, 0);
	}
	if (s == INVALID_SOCKET)
	{
		// 발생 상황을 직접 보기 위해 즉시 중단
		std::cout << "[FloodClient] socket() 실패 err=" << WSAGetLastError() << " sessions=" << sessions.size() << "\n";
		__debugbreak();
	}

	// close 시 RST로 즉시 정리 (TIME_WAIT 없음)
	LINGER linger{};
	linger.l_onoff = 1;
	linger.l_linger = 0;
	if (setsockopt(s, SOL_SOCKET, SO_LINGER, reinterpret_cast<const char*>(&linger), sizeof(linger)) == SOCKET_ERROR)
	{
		std::cout << "[FloodClient] setsockopt(SO_LINGER) 실패 err=" << WSAGetLastError() << "\n";
		__debugbreak();
	}

	// connect가 핸드셰이크를 기다리지 않고 바로 반환하도록
	u_long nonBlocking = 1;
	if (ioctlsocket(s, FIONBIO, &nonBlocking) == SOCKET_ERROR)
	{
		std::cout << "[FloodClient] ioctlsocket(FIONBIO) 실패 err=" << WSAGetLastError() << "\n";
		__debugbreak();
	}

	sessions.emplace(s, SessionState::Disconnect);
}

void FloodClient::connectSessions(std::unordered_map<SOCKET, SessionState>& sessions, std::vector<SOCKET>& closeSockets)
{
	for (auto& [s, state] : sessions)
	{
		if (state != SessionState::Disconnect)
		{
			continue;
		}

		int ret = 0;
		{
			ProfileScope scope("Flood_Connect");	// 임시: 클라 루프 지연 원인 확인용
			ret = connect(s, reinterpret_cast<sockaddr*>(&mTargetAddr), sizeof(mTargetAddr));
		}

		// 논블로킹 connect는 정상이면 SOCKET_ERROR + WSAEWOULDBLOCK으로 바로 반환
		if (ret == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK)
		{
			++mFailCount;
			closeSockets.push_back(s);
			continue;
		}
		state = SessionState::ConnectWait;
	}

	// 순회 중 map을 고치면 반복자가 깨지므로 순회 후 뒤에서부터 정리
	while (!closeSockets.empty())
	{
		SOCKET s = closeSockets.back();
		closeSockets.pop_back();
		closeSession(sessions, s);
	}
}

void FloodClient::selectSessions(std::unordered_map<SOCKET, SessionState>& sessions, std::vector<SOCKET>& closeSockets)
{
	fd_set writeSet;
	fd_set exceptSet;
	FD_ZERO(&writeSet);
	FD_ZERO(&exceptSet);

	for (const auto& [s, state] : sessions)
	{
		if (state != SessionState::ConnectWait)
		{
			continue;
		}
		FD_SET(s, &writeSet);	// 핸드셰이크 성공 시 반응
		FD_SET(s, &exceptSet);	// 실패 시 반응 (Windows)
	}

	// 빈 set으로 select를 부르면 WSAEINVAL로 바로 실패
	if (writeSet.fd_count == 0)
	{
		return;
	}

	// 반응이 하나도 없어도 mRunning을 확인할 수 있도록 지정 시간마다 깨어남 (0이면 즉시 반환)
	timeval timeout{};
	timeout.tv_sec = mSelectTimeoutMs / 1000;
	timeout.tv_usec = (mSelectTimeoutMs % 1000) * 1000;
	int readyCnt = 0;
	{
		ProfileScope scope("Flood_Select");	// 임시: 클라 루프 지연 원인 확인용
		readyCnt = select(0, nullptr, &writeSet, &exceptSet, &timeout);
	}
	if (readyCnt <= 0)
	{
		return;	// 0: 타임아웃, SOCKET_ERROR: 다음 루프에서 재시도
	}

	// select는 set을 "반응 온 소켓만" 남기도록 덮어씀
	for (const auto& [s, state] : sessions)
	{
		if (state != SessionState::ConnectWait)
		{
			continue;
		}

		if (FD_ISSET(s, &writeSet))
		{
			++mConnectCount;
			closeSockets.push_back(s);
			continue;
		}
		if (FD_ISSET(s, &exceptSet))
		{
			++mFailCount;
			closeSockets.push_back(s);
		}
	}

	// 순회 중 map을 고치면 반복자가 깨지므로 순회 후 뒤에서부터 정리
	while (!closeSockets.empty())
	{
		SOCKET s = closeSockets.back();
		closeSockets.pop_back();
		closeSession(sessions, s);
	}
}

void FloodClient::closeSession(std::unordered_map<SOCKET, SessionState>& sessions, SOCKET s)
{
	sessions.erase(s);	// 닫힌 핸들 값을 새 socket()이 다시 받을 수 있으므로 insert보다 먼저
	{
		ProfileScope scope("Flood_CloseSocket");	// 임시: 클라 루프 지연 원인 확인용
		closesocket(s);	// linger(0)가 걸려 있어 RST로 즉시 정리
	}
	createSession(sessions);
}

void FloodClient::statLoop()
{
	// 디버거(스레드 창)/덤프에서 보이는 이름
	SetThreadDescription(GetCurrentThread(), L"FloodStat");

	int64_t lastConnect = 0;
	int64_t lastFail = 0;
	while (mRunning)
	{
		Sleep(1000);
		int64_t nowConnect = mConnectCount.load();
		int64_t nowFail = mFailCount.load();
		std::cout << "[FloodClient] ConnectTPS=" << nowConnect - lastConnect
			<< " FailTPS=" << nowFail - lastFail
			<< " total=" << nowConnect
			<< " fail=" << nowFail << "\n";
		lastConnect = nowConnect;
		lastFail = nowFail;
	}
}
