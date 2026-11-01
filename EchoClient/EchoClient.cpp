#include "EchoClient.h"
#include "StatUtil.h"
#include <cstring>
#include <iomanip>
#include <iostream>

void EchoClient::PacketStream::init(uint32_t seed)
{
	mRng.seed(seed);
	mSeed = seed;
	mPayloadPos = 0;
	mHeaderLeft = 0;
	mPayloadLeft = 0;
}

// 다음 1바이트. 패킷 경계면 길이를 새로 뽑아 헤더부터 내보낸다
char EchoClient::PacketStream::next()
{
	if (atBoundary())
	{
		// 길이 0(빈 패킷)과 최대 길이도 나오도록 양 끝 포함
		PacketHeader header;
		header.len = static_cast<uint16_t>(std::uniform_int_distribution<int>(0, MAX_PAYLOAD_SIZE)(mRng));
		memcpy(mHeader, &header, PACKET_HEADER_SIZE);
		mHeaderLeft = PACKET_HEADER_SIZE;
		mPayloadLeft = header.len;
	}

	if (mHeaderLeft > 0)
	{
		char c = mHeader[PACKET_HEADER_SIZE - mHeaderLeft];
		--mHeaderLeft;
		return c;
	}
	--mPayloadLeft;
	return static_cast<char>((mSeed + mPayloadPos++) % 251);
}

EchoClient::~EchoClient()
{
	stop();
}

bool EchoClient::init(const EchoClientConfig& cfg)
{
	// 위: 서버 링이 넘치지 않는 한도. 아래: 패킷 1개는 들어가야 한다 - 더 작으면 큰 패킷을 중간까지만 보내고 멈추는데,
	// 서버는 완성 안 된 패킷을 에코하지 않아 한도가 영원히 안 비는 교착이 된다
	if (cfg.maxInflight < MAX_PACKET_SIZE || MAX_INFLIGHT < cfg.maxInflight)
	{
		return false;
	}

	mTargetAddr.sin_family = AF_INET;
	mTargetAddr.sin_port = htons(cfg.port);
	if (inet_pton(AF_INET, cfg.ip.c_str(), &mTargetAddr.sin_addr) != 1)
	{
		return false;
	}

	mThreadCnt = cfg.threadCnt;
	mBytesPerConn = cfg.bytesPerConn;
	mNoRecv = cfg.noRecv;
	mBadPacket = cfg.badPacket;
	mMaxInflight = cfg.maxInflight;

	mRunning = true;
	for (int i = 0; i < mThreadCnt; ++i)
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
	uint32_t seed = rng();	// 연결마다 다른 시드
	session.mSendStream.init(seed);
	session.mRecvStream.init(seed);
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
		bool wantWrite = false;
		if (mBadPacket)
		{
			wantWrite = !session.mBadSent;
		}
		else
		{
			bool pendingLeft = session.mPendingOff < session.mPending.size();
			bool dataRoom = !session.mSendDone && session.mSendPos - session.mRecvPos < mMaxInflight;
			wantWrite = pendingLeft || dataRoom;
		}
		if (wantWrite)
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
				if (mBadPacket)
				{
					++mBadClosed;	// 잘못된 헤더를 보고 서버가 끊음 - 이 모드의 정상 결과
				}
				else
				{
					++mUnexpectedClose;	// 목표량 전에 서버가 끊음
				}
				closeSockets.push_back(s);
				continue;
			}
			if (!mBadPacket && !hasMoreToSend(session) && session.mRecvPos == session.mSendPos)
			{
				++mDoneCount;	// 다 주고받음 - 끊고 새 연결로 재사용 경로를 탄다
				closeSockets.push_back(s);
				continue;
			}
		}
		if (FD_ISSET(s, &writeSet))
		{
			bool ok = mBadPacket ? sendBadHeader(s, session) : sendSome(s, session, rng);
			if (!ok)
			{
				if (mBadPacket)
				{
					++mBadClosed;
				}
				else
				{
					++mUnexpectedClose;
				}
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

bool EchoClient::hasMoreToSend(const EchoSession& session) const
{
	return !session.mSendDone || session.mPendingOff < session.mPending.size();
}

// 보관 중인 바이트가 없으면 무작위 크기(1–MAX_SEND_SIZE)만큼 서버 링 여유 안에서 새로 만든다
// 크기가 패킷과 무관하므로 헤더 중간에서 잘리거나 여러 패킷이 한 번에 나간다
// 논블로킹 send는 일부만 보낼 수 있으므로 못 보낸 나머지는 mPending에 남겨 다음에 이어 보낸다
bool EchoClient::sendSome(SOCKET s, EchoSession& session, std::mt19937& rng)
{
	if (session.mPendingOff == session.mPending.size())
	{
		session.mPending.clear();
		session.mPendingOff = 0;

		int64_t len = std::uniform_int_distribution<int>(1, MAX_SEND_SIZE)(rng);
		int64_t room = mMaxInflight - (session.mSendPos - session.mRecvPos);
		if (!mNoRecv && room < len)
		{
			len = room;
		}

		for (int64_t i = 0; i < len; ++i)
		{
			// 목표량을 넘긴 뒤 패킷 경계에서 멈춘다 - 잘린 패킷을 남기면 서버가 그 패킷을 에코하지 않아 연결이 끝나지 않는다
			if (session.mSendStream.atBoundary() && mBytesPerConn <= session.mSendPos + i)
			{
				session.mSendDone = true;
				break;
			}
			session.mPending.push_back(session.mSendStream.next());
		}
		if (session.mPending.empty())
		{
			return true;
		}
	}

	const char* buf = session.mPending.data() + session.mPendingOff;
	int left = static_cast<int>(session.mPending.size() - session.mPendingOff);
	int sent = send(s, buf, left, 0);
	if (sent == SOCKET_ERROR)
	{
		return WSAGetLastError() == WSAEWOULDBLOCK;	// 송신 버퍼 가득 - 다음 바퀴에 다시
	}
	session.mPendingOff += sent;
	session.mSendPos += sent;
	return true;
}

// 최대 페이로드를 넘는 길이의 헤더만 보낸다 - 서버는 페이로드를 기다리지 않고 헤더만 보고 끊어야 한다
bool EchoClient::sendBadHeader(SOCKET s, EchoSession& session)
{
	PacketHeader header;
	header.len = static_cast<uint16_t>(MAX_PAYLOAD_SIZE + 1);
	int sent = send(s, reinterpret_cast<const char*>(&header), PACKET_HEADER_SIZE, 0);
	if (sent == SOCKET_ERROR)
	{
		return WSAGetLastError() == WSAEWOULDBLOCK;
	}
	session.mBadSent = true;	// 막 연결된 소켓의 빈 송신 버퍼라 2바이트가 나뉘어 나가지 않는다
	return true;
}

// 받을 수 있는 만큼 받아 바이트마다 생성기 기대값과 비교한다 - 헤더까지 에코 스트림의 일부라 패킷으로 나눌 필요가 없다
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
		char expected = session.mRecvStream.next();
		if (buf[i] != expected)
		{
			std::cout << "[EchoClient] 불일치 pos=" << session.mRecvPos + i
				<< " expected=" << static_cast<int>(static_cast<uint8_t>(expected))
				<< " actual=" << static_cast<int>(static_cast<uint8_t>(buf[i]))
				<< " seed=" << session.mRecvStream.mSeed << "\n";
			__debugbreak();
		}
	}
	session.mRecvPos += received;
	mEchoBytes += received;
	return true;
}

// 1초마다 에코 처리량과 이상 지표를 출력한다 - unexpectedClose가 0이 아니면 서버 문제
// cpu 코어 = 프로세스가 쓴 CPU를 코어 개수로 환산 - 워커 스레드 수에 가까우면 클라가 한계라 측정값이 클라에 묶인다
void EchoClient::statLoop()
{
	SetThreadDescription(GetCurrentThread(), L"EchoStat");

	CpuSampler cpu;
	double coreCnt = static_cast<double>(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
	int64_t lastBytes = 0;
	int64_t lastDone = 0;
	while (mRunning)
	{
		Sleep(1000);
		int64_t nowBytes = mEchoBytes.load();
		int64_t nowDone = mDoneCount.load();
		CpuUsage usage = cpu.sample();
		double cores = (usage.processUser + usage.processKernel) * coreCnt / 100.0;
		std::cout << "[EchoClient] EchoKB/s=" << (nowBytes - lastBytes) / 1024
			<< " DoneConn/s=" << nowDone - lastDone
			<< " totalMB=" << nowBytes / (1024 * 1024)
			<< " unexpectedClose=" << mUnexpectedClose.load()
			<< " connectFail=" << mFailCount.load()
			<< " earlyClose=" << mEarlyClose.load()
			<< " badClosed=" << mBadClosed.load()
			<< std::fixed << std::setprecision(2) << " cpu코어=" << cores << "/" << mThreadCnt << "\n";
		lastBytes = nowBytes;
		lastDone = nowDone;
	}
}
