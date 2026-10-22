#include "EchoClient.h"
#include <iostream>

namespace
{
	// 스트림의 pos번째 바이트 - 송신과 검증이 같은 식을 쓴다
	char patternByte(uint32_t seed, int64_t pos)
	{
		return static_cast<char>((seed + pos) % 251);
	}
}

EchoClient::~EchoClient()
{
	stop();
}

bool EchoClient::init(const std::string& ip, USHORT port, int threadCnt, int64_t bytesPerConn, bool noRecv)
{
	mTargetAddr.sin_family = AF_INET;
	mTargetAddr.sin_port = htons(port);
	if (inet_pton(AF_INET, ip.c_str(), &mTargetAddr.sin_addr) != 1)
	{
		return false;
	}

	mThreadCnt = threadCnt;
	mBytesPerConn = bytesPerConn;
	mNoRecv = noRecv;
	mRunning = true;
	for (int i = 0; i < threadCnt; ++i)
	{
		mWorkers.emplace_back(&EchoClient::workerLoop, this);
	}
	mStatThread = std::thread(&EchoClient::statLoop, this);
	return true;
}

void EchoClient::stop()
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

void EchoClient::workerLoop()
{
	int idx = mReadyCount++;

	// 디버거(스레드 창)/덤프에서 보이는 이름 (와이드 문자열 필요)
	std::wstring threadDesc = L"EchoWorker_" + std::to_wstring(idx);
	SetThreadDescription(GetCurrentThread(), threadDesc.c_str());

	// 스레드마다 별도 생성기 - 시드와 송신 크기용
	std::mt19937 rng(std::random_device{}());

	// 스레드마다 별개의 세션 64개와 정리 목록
	std::unordered_map<SOCKET, EchoSession> sessions;
	std::vector<SOCKET> closeSockets;
	closeSockets.reserve(SESSION_PER_THREAD);
	for (int i = 0; i < SESSION_PER_THREAD; ++i)
	{
		createSession(sessions, rng);
	}

	while (mReadyCount.load() < mThreadCnt) {}	// 모든 워커가 준비될 때까지 대기

	while (mRunning)
	{
		connectSessions(sessions, closeSockets, rng);
		selectSessions(sessions, closeSockets, rng);
		if (mNoRecv)
		{
			floodSessions(sessions, closeSockets, rng);
		}
	}

	// 종료: 남은 소켓 정리
	for (const auto& [s, session] : sessions)
	{
		closesocket(s);
	}
}

void EchoClient::createSession(std::unordered_map<SOCKET, EchoSession>& sessions, std::mt19937& rng)
{
	SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
	if (s == INVALID_SOCKET)
	{
		// 발생 상황을 직접 보기 위해 즉시 중단
		std::cout << "[EchoClient] socket() 실패 err=" << WSAGetLastError() << " sessions=" << sessions.size() << "\n";
		__debugbreak();
	}

	// close 시 RST로 즉시 정리 (TIME_WAIT 없음)
	LINGER linger{};
	linger.l_onoff = 1;
	linger.l_linger = 0;
	if (setsockopt(s, SOL_SOCKET, SO_LINGER, reinterpret_cast<const char*>(&linger), sizeof(linger)) == SOCKET_ERROR)
	{
		std::cout << "[EchoClient] setsockopt(SO_LINGER) 실패 err=" << WSAGetLastError() << "\n";
		__debugbreak();
	}

	// connect / send / recv가 기다리지 않고 바로 반환하도록
	u_long nonBlocking = 1;
	if (ioctlsocket(s, FIONBIO, &nonBlocking) == SOCKET_ERROR)
	{
		std::cout << "[EchoClient] ioctlsocket(FIONBIO) 실패 err=" << WSAGetLastError() << "\n";
		__debugbreak();
	}

	EchoSession session;
	session.mSeed = rng();	// 연결마다 다른 시드
	sessions.emplace(s, session);
}

void EchoClient::connectSessions(std::unordered_map<SOCKET, EchoSession>& sessions, std::vector<SOCKET>& closeSockets, std::mt19937& rng)
{
	for (auto& [s, session] : sessions)
	{
		if (session.mState != SessionState::Disconnect)
		{
			continue;
		}

		int ret = connect(s, reinterpret_cast<sockaddr*>(&mTargetAddr), sizeof(mTargetAddr));

		// 논블로킹 connect는 정상이면 SOCKET_ERROR + WSAEWOULDBLOCK으로 바로 반환
		if (ret == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK)
		{
			++mFailCount;
			closeSockets.push_back(s);
			continue;
		}
		session.mState = SessionState::ConnectWait;
	}

	// 순회 중 map을 고치면 반복자가 깨지므로 순회 후 뒤에서부터 정리
	while (!closeSockets.empty())
	{
		SOCKET s = closeSockets.back();
		closeSockets.pop_back();
		closeSession(sessions, s, rng);
	}
}

void EchoClient::closeSession(std::unordered_map<SOCKET, EchoSession>& sessions, SOCKET s, std::mt19937& rng)
{
	sessions.erase(s);	// 닫힌 핸들 값을 새 socket()이 다시 받을 수 있으므로 insert보다 먼저
	closesocket(s);	// linger(0)가 걸려 있어 RST로 즉시 정리
	createSession(sessions, rng);
}

// ConnectWait: writeSet(연결 성공) + exceptSet(실패)
// Connected: readSet은 항상, writeSet은 보낼 여유가 있을 때만 - 여유가 없는데 넣으면 select가 계속 즉시 반환해 바쁜 루프가 된다
void EchoClient::selectSessions(std::unordered_map<SOCKET, EchoSession>& sessions, std::vector<SOCKET>& closeSockets, std::mt19937& rng)
{
	fd_set readSet;
	fd_set writeSet;
	fd_set exceptSet;
	FD_ZERO(&readSet);
	FD_ZERO(&writeSet);
	FD_ZERO(&exceptSet);

	for (const auto& [s, session] : sessions)
	{
		if (session.mState == SessionState::ConnectWait)
		{
			FD_SET(s, &writeSet);
			FD_SET(s, &exceptSet);
			continue;
		}
		if (session.mState != SessionState::Connected)
		{
			continue;
		}
		if (mNoRecv)
		{
			continue;	// 받지 않는 모드: 연결된 소켓은 floodSessions가 select 없이 직접 보낸다
		}

		FD_SET(s, &readSet);
		bool remain = session.mSendPos < mBytesPerConn;
		bool roomInServer = session.mSendPos - session.mRecvPos < MAX_INFLIGHT;
		if (remain && roomInServer)
		{
			FD_SET(s, &writeSet);
		}
	}

	// 빈 set으로 select를 부르면 WSAEINVAL로 바로 실패
	if (readSet.fd_count == 0 && writeSet.fd_count == 0)
	{
		return;
	}

	// 반응이 없어도 mRunning을 확인할 수 있도록 100ms마다 깨어남
	timeval timeout{};
	timeout.tv_usec = 100 * 1000;
	int readyCnt = select(0, &readSet, &writeSet, &exceptSet, &timeout);
	if (readyCnt <= 0)
	{
		return;	// 0: 타임아웃, SOCKET_ERROR: 다음 루프에서 재시도
	}

	for (auto& [s, session] : sessions)
	{
		if (session.mState == SessionState::ConnectWait)
		{
			if (FD_ISSET(s, &writeSet))
			{
				session.mState = SessionState::Connected;	// 송신은 다음 바퀴부터
				continue;
			}
			if (FD_ISSET(s, &exceptSet))
			{
				++mFailCount;
				closeSockets.push_back(s);
			}
			continue;
		}
		if (session.mState != SessionState::Connected)
		{
			continue;
		}
		if (mNoRecv)
		{
			continue;
		}

		if (FD_ISSET(s, &readSet))
		{
			if (!recvAndVerify(s, session))
			{
				++mUnexpectedClose;	// 목표량 전에 서버가 끊음
				closeSockets.push_back(s);
				continue;
			}
			if (session.mRecvPos == mBytesPerConn)
			{
				++mDoneCount;	// 다 주고받음 - 끊고 새 연결로 재사용 경로를 탄다
				closeSockets.push_back(s);
				continue;
			}
		}
		if (FD_ISSET(s, &writeSet))
		{
			if (!sendSome(s, session, rng))
			{
				++mUnexpectedClose;
				closeSockets.push_back(s);
			}
		}
	}

	// 순회 중 map을 고치면 반복자가 깨지므로 순회 후 뒤에서부터 정리
	while (!closeSockets.empty())
	{
		SOCKET s = closeSockets.back();
		closeSockets.pop_back();
		closeSession(sessions, s, rng);
	}
}

// 받지 않는 모드: 연결된 소켓마다 보낼 수 있는 만큼 보낸다. 서버가 RST로 끊으면 send가 실패한다
// select를 쓰지 않는 이유: 받지 않으니 readSet은 항상 반응하고, 송신 버퍼가 찬 상태의 RST가 writeSet으로 알려지는지 불확실
void EchoClient::floodSessions(std::unordered_map<SOCKET, EchoSession>& sessions, std::vector<SOCKET>& closeSockets, std::mt19937& rng)
{
	for (auto& [s, session] : sessions)
	{
		if (session.mState != SessionState::Connected)
		{
			continue;
		}
		if (!sendSome(s, session, rng))
		{
			if (session.mSendPos < MAX_INFLIGHT)
			{
				++mEarlyClose;
			}
			else
			{
				++mUnexpectedClose;	// 이 모드에서는 서버가 넘침으로 끊은 수
			}
			closeSockets.push_back(s);
		}
	}

	// 순회 중 map을 고치면 반복자가 깨지므로 순회 후 뒤에서부터 정리
	while (!closeSockets.empty())
	{
		SOCKET s = closeSockets.back();
		closeSockets.pop_back();
		closeSession(sessions, s, rng);
	}
	Sleep(10);	// 송신 버퍼가 차 있을 때 바쁜 루프 방지
}

// 무작위 크기(1~MAX_SEND_SIZE)를 서버 링 여유와 남은 총량 안에서 보낸다
// 논블로킹 send는 일부만 보낼 수 있으므로 실제로 보낸 만큼만 위치를 옮긴다
bool EchoClient::sendSome(SOCKET s, EchoSession& session, std::mt19937& rng)
{
	int64_t len = std::uniform_int_distribution<int>(1, MAX_SEND_SIZE)(rng);
	int64_t room = MAX_INFLIGHT - (session.mSendPos - session.mRecvPos);
	int64_t remain = mBytesPerConn - session.mSendPos;
	if (!mNoRecv && room < len)
	{
		len = room;
	}
	if (remain < len)
	{
		len = remain;
	}

	char buf[MAX_SEND_SIZE];
	for (int64_t i = 0; i < len; ++i)
	{
		buf[i] = patternByte(session.mSeed, session.mSendPos + i);
	}

	int sent = send(s, buf, static_cast<int>(len), 0);
	if (sent == SOCKET_ERROR)
	{
		return WSAGetLastError() == WSAEWOULDBLOCK;	// 송신 버퍼 가득 - 다음 바퀴에 다시
	}
	session.mSendPos += sent;
	return true;
}

// 받을 수 있는 만큼 받아 바이트마다 기대값과 비교한다
// 0바이트나 에러 = 서버가 끊음 → false. 내용이 틀리면 서버 버그 → 즉시 중단
bool EchoClient::recvAndVerify(SOCKET s, EchoSession& session)
{
	char buf[MAX_INFLIGHT];
	int received = recv(s, buf, sizeof(buf), 0);
	if (received == SOCKET_ERROR)
	{
		return WSAGetLastError() == WSAEWOULDBLOCK;
	}
	if (received == 0)
	{
		return false;
	}

	// 보낸 것보다 많이 돌아오면 중복
	if (session.mSendPos < session.mRecvPos + received)
	{
		std::cout << "[EchoClient] 보낸 것보다 많이 받음 sendPos=" << session.mSendPos
			<< " recvPos=" << session.mRecvPos << " received=" << received << "\n";
		__debugbreak();
	}

	for (int i = 0; i < received; ++i)
	{
		char expected = patternByte(session.mSeed, session.mRecvPos + i);
		if (buf[i] != expected)
		{
			std::cout << "[EchoClient] 불일치 pos=" << session.mRecvPos + i
				<< " expected=" << static_cast<int>(static_cast<uint8_t>(expected))
				<< " actual=" << static_cast<int>(static_cast<uint8_t>(buf[i]))
				<< " seed=" << session.mSeed << "\n";
			__debugbreak();
		}
	}
	session.mRecvPos += received;
	mEchoBytes += received;
	return true;
}

// 1초마다 에코 처리량과 이상 지표를 출력한다 - unexpectedClose가 0이 아니면 서버 문제
void EchoClient::statLoop()
{
	SetThreadDescription(GetCurrentThread(), L"EchoStat");

	int64_t lastBytes = 0;
	int64_t lastDone = 0;
	while (mRunning)
	{
		Sleep(1000);
		int64_t nowBytes = mEchoBytes.load();
		int64_t nowDone = mDoneCount.load();
		std::cout << "[EchoClient] EchoKB/s=" << (nowBytes - lastBytes) / 1024
			<< " DoneConn/s=" << nowDone - lastDone
			<< " totalMB=" << nowBytes / (1024 * 1024)
			<< " unexpectedClose=" << mUnexpectedClose.load()
			<< " connectFail=" << mFailCount.load()
			<< " earlyClose=" << mEarlyClose.load() << "\n";
		lastBytes = nowBytes;
		lastDone = nowDone;
	}
}
