#include "LoadClient.h"
#include "StatUtil.h"
#include <cstring>
#include <iomanip>
#include <iostream>

LoadClient::~LoadClient()
{
	stop();
}

bool LoadClient::init(const LoadClientConfig& cfg)
{
	if (cfg.msgSize < MIN_MSG_SIZE || MAX_PACKET_SIZE < cfg.msgSize)
	{
		return false;
	}
	if (cfg.overSend < 1 || MAX_BATCH_BYTES < cfg.overSend * cfg.msgSize)
	{
		return false;
	}
	if (cfg.sessionPerThread < 1 || MAX_SESSION_PER_THREAD < cfg.sessionPerThread)
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
	mSessionPerThread = cfg.sessionPerThread;
	mOverSend = cfg.overSend;
	mMsgSize = cfg.msgSize;
	mPayloadSize = cfg.msgSize - PACKET_HEADER_SIZE;
	mRunning = true;
	for (int i = 0; i < mThreadCnt; ++i)
	{
		mWorkers.emplace_back(&LoadClient::workerLoop, this);
	}
	mStatThread = std::thread(&LoadClient::statLoop, this);
	return true;
}

void LoadClient::stop()
{
	if (!mRunning)
	{
		return;
	}

	mRunning = false;
	for (std::thread& worker : mWorkers)
	{
		worker.join();
	}
	mStatThread.join();
}

void LoadClient::workerLoop()
{
	int idx = mReadyCount++;
	std::wstring threadDesc = L"LoadWorker_" + std::to_wstring(idx);
	SetThreadDescription(GetCurrentThread(), threadDesc.c_str());

	std::unordered_map<SOCKET, LoadSession> sessions;
	std::vector<SOCKET> closeSockets;
	closeSockets.reserve(mSessionPerThread);
	for (int i = 0; i < mSessionPerThread; ++i)
	{
		createSession(sessions);
	}

	while (mReadyCount.load() < mThreadCnt) {}	// 모든 워커가 준비될 때까지 대기

	while (mRunning)
	{
		connectSessions(sessions, closeSockets);
		selectSessions(sessions, closeSockets);
	}

	for (const auto& [s, session] : sessions)
	{
		closesocket(s);
	}
}

void LoadClient::createSession(std::unordered_map<SOCKET, LoadSession>& sessions)
{
	SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
	if (s == INVALID_SOCKET)
	{
		std::cout << "[LoadClient] socket() 실패 err=" << WSAGetLastError() << "\n";
		__debugbreak();
	}

	// close 시 RST로 즉시 정리 (TIME_WAIT 없음)
	LINGER linger{};
	linger.l_onoff = 1;
	linger.l_linger = 0;
	setsockopt(s, SOL_SOCKET, SO_LINGER, reinterpret_cast<const char*>(&linger), sizeof(linger));

	u_long nonBlocking = 1;
	if (ioctlsocket(s, FIONBIO, &nonBlocking) == SOCKET_ERROR)
	{
		std::cout << "[LoadClient] ioctlsocket(FIONBIO) 실패 err=" << WSAGetLastError() << "\n";
		__debugbreak();
	}
	sessions.emplace(s, LoadSession{});
}

void LoadClient::connectSessions(std::unordered_map<SOCKET, LoadSession>& sessions, std::vector<SOCKET>& closeSockets)
{
	for (auto& [s, session] : sessions)
	{
		if (session.mState != SessionState::Disconnect)
		{
			continue;
		}
		int ret = connect(s, reinterpret_cast<sockaddr*>(&mTargetAddr), sizeof(mTargetAddr));
		if (ret == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK)
		{
			++mFailCount;
			closeSockets.push_back(s);
			continue;
		}
		session.mState = SessionState::ConnectWait;
	}

	while (!closeSockets.empty())
	{
		SOCKET s = closeSockets.back();
		closeSockets.pop_back();
		closeSession(sessions, s);
	}
}

void LoadClient::closeSession(std::unordered_map<SOCKET, LoadSession>& sessions, SOCKET s)
{
	if (sessions[s].mState == SessionState::Connected)
	{
		--mConnectedCount;
	}
	sessions.erase(s);	// 닫힌 핸들 값을 새 socket()이 다시 받을 수 있으므로 insert보다 먼저
	closesocket(s);
	createSession(sessions);
}

// 메시지 = [헤더 len=msgSize-3, Data][순번 8바이트][채움] - 묶음 전체를 send 한 번으로 보낸다
void LoadClient::queueBatch(LoadSession& session)
{
	char msg[MAX_PACKET_SIZE] = {};
	PacketHeader header;
	header.len = static_cast<uint16_t>(mPayloadSize);
	header.kind = ePacketKind::Data;
	memcpy(msg, &header, PACKET_HEADER_SIZE);
	for (int i = 0; i < mOverSend; ++i)
	{
		uint64_t seq = session.mSendSeq++;
		memcpy(msg + PACKET_HEADER_SIZE, &seq, sizeof(seq));
		session.mSendBuf.insert(session.mSendBuf.end(), msg, msg + mMsgSize);
	}
	session.mWaiting = mOverSend;
}

// ConnectWait: writeSet(연결 성공) + exceptSet(실패)
// Connected: readSet은 항상, writeSet은 보낼 바이트가 있을 때만 - 없는데 넣으면 select가 계속 즉시 반환해 바쁜 루프가 된다
void LoadClient::selectSessions(std::unordered_map<SOCKET, LoadSession>& sessions, std::vector<SOCKET>& closeSockets)
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
		FD_SET(s, &readSet);
		if (session.mSendOff < session.mSendBuf.size())
		{
			FD_SET(s, &writeSet);
		}
	}

	if (readSet.fd_count == 0 && writeSet.fd_count == 0)
	{
		return;	// 빈 set으로 select를 부르면 WSAEINVAL로 바로 실패
	}

	timeval timeout{};
	timeout.tv_usec = 100 * 1000;	// 반응이 없어도 mRunning을 확인할 수 있도록
	if (select(0, &readSet, &writeSet, &exceptSet, &timeout) <= 0)
	{
		return;
	}

	for (auto& [s, session] : sessions)
	{
		if (session.mState == SessionState::ConnectWait)
		{
			if (FD_ISSET(s, &writeSet))
			{
				session.mState = SessionState::Connected;
				++mConnectedCount;
				queueBatch(session);	// 첫 묶음 - 다음 바퀴의 writeSet으로 나간다
			}
			else if (FD_ISSET(s, &exceptSet))
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

		if (FD_ISSET(s, &readSet) && !recvMessages(s, session))
		{
			++mUnexpectedClose;
			closeSockets.push_back(s);
			continue;
		}
		if (FD_ISSET(s, &writeSet) && !sendPending(s, session))
		{
			++mUnexpectedClose;
			closeSockets.push_back(s);
		}
	}

	while (!closeSockets.empty())
	{
		SOCKET s = closeSockets.back();
		closeSockets.pop_back();
		closeSession(sessions, s);
	}
}

// 논블로킹 send는 일부만 보낼 수 있으므로 보낸 만큼만 위치를 옮기고, 다 보내면 버퍼를 비운다
bool LoadClient::sendPending(SOCKET s, LoadSession& session)
{
	const char* buf = session.mSendBuf.data() + session.mSendOff;
	int left = static_cast<int>(session.mSendBuf.size() - session.mSendOff);
	int sent = send(s, buf, left, 0);
	if (sent == SOCKET_ERROR)
	{
		return WSAGetLastError() == WSAEWOULDBLOCK;	// 송신 버퍼 가득 - 다음 바퀴에 다시
	}
	session.mSendOff += sent;
	if (session.mSendOff == session.mSendBuf.size())
	{
		session.mSendBuf.clear();
		session.mSendOff = 0;
	}
	return true;
}

// 받은 바이트를 패킷 단위로 처리한다
// - Data: 순번이 보낸 순서대로인지 확인 (유실/중복/순서 바뀜 검출). 묶음이 다 돌아오면 다음 묶음
// - Ping: 같은 페이로드의 퐁을 송신 버퍼 뒤에 붙인다
bool LoadClient::recvMessages(SOCKET s, LoadSession& session)
{
	char buf[16384];
	int received = recv(s, buf, sizeof(buf), 0);
	if (received == SOCKET_ERROR)
	{
		return WSAGetLastError() == WSAEWOULDBLOCK;
	}
	if (received == 0)
	{
		return false;
	}
	session.mRecvBuf.insert(session.mRecvBuf.end(), buf, buf + received);

	int64_t echoCount = 0;
	size_t pos = 0;
	while (session.mRecvBuf.size() - pos >= PACKET_HEADER_SIZE)
	{
		PacketHeader header;
		memcpy(&header, session.mRecvBuf.data() + pos, PACKET_HEADER_SIZE);
		size_t packetSize = PACKET_HEADER_SIZE + header.len;
		if (session.mRecvBuf.size() - pos < packetSize)
		{
			break;	// 페이로드가 덜 옴
		}
		const char* payload = session.mRecvBuf.data() + pos + PACKET_HEADER_SIZE;

		if (header.kind == ePacketKind::Data && header.len == mPayloadSize)
		{
			uint64_t seq = 0;
			memcpy(&seq, payload, sizeof(seq));
			if (seq != session.mRecvSeq || session.mWaiting == 0)
			{
				std::cout << "[LoadClient] 순번 어긋남 expected=" << session.mRecvSeq << " actual=" << seq
					<< " waiting=" << session.mWaiting << "\n";
				__debugbreak();
			}
			++session.mRecvSeq;
			++echoCount;
			if (--session.mWaiting == 0)
			{
				++mBatchCount;
				queueBatch(session);	// 묶음이 전부 돌아옴 - 다음 묶음
			}
		}
		else if (header.kind == ePacketKind::Ping && header.len == sizeof(PingPayload))
		{
			PacketHeader pong;
			pong.len = header.len;
			pong.kind = ePacketKind::Pong;
			const char* h = reinterpret_cast<const char*>(&pong);
			session.mSendBuf.insert(session.mSendBuf.end(), h, h + PACKET_HEADER_SIZE);
			session.mSendBuf.insert(session.mSendBuf.end(), payload, payload + header.len);
			++mPongCount;
		}
		else
		{
			std::cout << "[LoadClient] 예상 밖 헤더 kind=" << static_cast<int>(header.kind) << " len=" << header.len << "\n";
			__debugbreak();
		}
		pos += packetSize;
	}
	session.mRecvBuf.erase(session.mRecvBuf.begin(), session.mRecvBuf.begin() + pos);	// 잘린 패킷만 남긴다
	mEchoMsgCount += echoCount;
	return true;
}

// 1초마다 메시지 처리량을 출력한다 - cpu코어가 워커 수에 가까우면 클라가 한계라 처리량이 클라에 묶인다
void LoadClient::statLoop()
{
	SetThreadDescription(GetCurrentThread(), L"LoadStat");

	StatTicker ticker;
	CpuSampler cpu;
	double coreCnt = static_cast<double>(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
	int64_t lastMsg = 0;
	int64_t lastBatch = 0;
	while (mRunning)
	{
		double intervalSec = ticker.waitNext();
		CpuUsage usage = cpu.sample();
		int64_t nowMsg = mEchoMsgCount.load();
		int64_t nowBatch = mBatchCount.load();
		double cores = (usage.processUser + usage.processKernel) * coreCnt / 100.0;
		std::cout << std::fixed << std::setprecision(0)
			<< "[LoadClient] 메시지/s=" << (nowMsg - lastMsg) / intervalSec
			<< " MB/s=" << (nowMsg - lastMsg) * mMsgSize / intervalSec / 1e6
			<< " 묶음/s=" << (nowBatch - lastBatch) / intervalSec
			<< " 연결=" << mConnectedCount.load()
			<< " unexpectedClose=" << mUnexpectedClose.load()
			<< " connectFail=" << mFailCount.load()
			<< " pong=" << mPongCount.load()
			<< std::setprecision(2) << " cpu코어=" << cores << "/" << mThreadCnt << "\n";
		lastMsg = nowMsg;
		lastBatch = nowBatch;
	}
}
