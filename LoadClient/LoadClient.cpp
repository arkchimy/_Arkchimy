#include "LoadClient.h"
#include "StatUtil.h"
#include <cstring>
#include <iomanip>
#include <iostream>
#include <random>
#include <timeapi.h>
#pragma comment(lib, "winmm.lib")

static thread_local int tWorkerIdx = -1;	// 워커 스레드 번호 - 자기 지연 히스토그램에 쓴다

static int64_t qpcNow()
{
	LARGE_INTEGER t;
	QueryPerformanceCounter(&t);
	return t.QuadPart;
}

// 채팅의 보낸 사람 키 - 연결마다 새로 뽑는다. 같은 방에서 겹치지만 않으면 되므로 64비트 무작위로 충분
static uint64_t nextSenderKey()
{
	thread_local std::mt19937_64 rng(std::random_device{}());
	return rng();
}

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
	if (cfg.ratePerSession < 0)
	{
		return false;
	}
	if (cfg.roomSize < 0)
	{
		return false;
	}
	if (cfg.roomSize > 0 &&
		(cfg.msgSize < CHAT_MIN_MSG_SIZE || PACKET_HEADER_SIZE + CHAT_MAX_PAYLOAD < cfg.msgSize ||
			MAX_ROOM_BATCH_BYTES < cfg.roomSize * cfg.overSend * cfg.msgSize || cfg.ratePerSession > 0))
	{
		return false;	// 채팅은 closed-loop만, 서버의 최대 길이 정책 안 - 방 전원의 묶음이 받는 세션 하나의 서버 넘침 한도 안에 들어가야 한다
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
	mRatePerSession = cfg.ratePerSession;
	mRoomSize = cfg.roomSize;
	LARGE_INTEGER freq;
	QueryPerformanceFrequency(&freq);
	mQpcFreq = freq.QuadPart;
	if (mRatePerSession > 0)
	{
		timeBeginPeriod(1);	// open-loop는 select를 1ms마다 깨워 쌓는다 - 기본 해상도(15.6ms)면 몰아서 보내게 된다
	}
	mLatencyHists.clear();	// 워커가 뜨기 전에 만든다 - 워커는 자기 번호의 히스토그램에 쓴다
	for (int i = 0; i < mThreadCnt; ++i)
	{
		mLatencyHists.push_back(std::make_unique<LatencyHistogram>());
	}
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
	if (mRatePerSession > 0)
	{
		timeEndPeriod(1);
	}
}

void LoadClient::workerLoop()
{
	int idx = mReadyCount++;
	tWorkerIdx = idx;
	std::wstring threadDesc = L"LoadWorker_" + std::to_wstring(idx);
	SetThreadDescription(GetCurrentThread(), threadDesc.c_str());

	std::unordered_map<SOCKET, LoadSession> sessions;
	std::vector<SOCKET> closeSockets;
	closeSockets.reserve(mSessionPerThread);
	for (int i = 0; i < mSessionPerThread; ++i)
	{
		createSession(sessions, idx * mSessionPerThread + i);
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

void LoadClient::createSession(std::unordered_map<SOCKET, LoadSession>& sessions, int slot)
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
	LoadSession session;
	session.mSlot = slot;
	session.mSenderKey = nextSenderKey();
	sessions.emplace(s, std::move(session));
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
	const LoadSession& old = sessions[s];
	if (old.mState == SessionState::Connected)
	{
		--mConnectedCount;
	}
	if (old.mJoined)
	{
		--mJoinedCount;
	}
	int slot = old.mSlot;	// 다시 연결해도 같은 방에 들어가도록 세션 번호를 넘긴다
	sessions.erase(s);	// 닫힌 핸들 값을 새 socket()이 다시 받을 수 있으므로 insert보다 먼저
	closesocket(s);
	createSession(sessions, slot);
}

// 메시지 = [헤더 len=msgSize-3, Data][순번 8바이트][채움] - 묶음 전체를 send 한 번으로 보낸다
void LoadClient::queueBatch(LoadSession& session)
{
	queueMessages(session, mOverSend);
	session.mWaiting = mOverSend;
	session.mLastReqQpc = qpcNow();	// 왕복 지연의 시작 - 실제 send는 다음 select 바퀴라 클라 루프 한 바퀴가 섞인다
}

// 자기 메시지(에코면 Data, 채팅이면 senderKey가 같은 Chat)가 하나 돌아올 때마다 부른다 - closed-loop에서만 의미가 있다
// 묶음의 첫 메시지와 마지막 메시지가 각각 기록되므로, 모아 보내기로 늦게 나간 메시지도 분포에 드러난다
void LoadClient::recordLatency(const LoadSession& session)
{
	int64_t us = (qpcNow() - session.mLastReqQpc) * 1'000'000 / mQpcFreq;
	mLatencyHists[tWorkerIdx]->record(us > 0 ? static_cast<uint64_t>(us) : 0);
}

void LoadClient::queueMessages(LoadSession& session, int64_t count)
{
	char msg[MAX_PACKET_SIZE] = {};
	PacketHeader header;
	header.len = static_cast<uint16_t>(mPayloadSize);
	memcpy(msg, &header, PACKET_HEADER_SIZE);
	for (int64_t i = 0; i < count; ++i)
	{
		uint64_t seq = session.mSendSeq++;
		if (mRoomSize > 0)
		{
			ChatMsg chat{ eChatType::Chat, 0, session.mSenderKey, seq };	// 방 순번(0)은 서버가 방 락 안에서 채운다
			memcpy(msg + PACKET_HEADER_SIZE, &chat, sizeof(chat));
		}
		else
		{
			memcpy(msg + PACKET_HEADER_SIZE, &seq, sizeof(seq));
		}
		session.mSendBuf.insert(session.mSendBuf.end(), msg, msg + mMsgSize);
	}
}

// 채팅: 연결 직후 방에 들어간다 - JoinOk가 와야 첫 묶음을 보낸다
void LoadClient::queueJoin(LoadSession& session)
{
	PacketHeader header;
	header.len = static_cast<uint16_t>(sizeof(ChatJoin));
	ChatJoin join{ eChatType::Join, static_cast<uint32_t>(session.mSlot / mRoomSize) };
	const char* h = reinterpret_cast<const char*>(&header);
	const char* j = reinterpret_cast<const char*>(&join);
	session.mSendBuf.insert(session.mSendBuf.end(), h, h + PACKET_HEADER_SIZE);
	session.mSendBuf.insert(session.mSendBuf.end(), j, j + sizeof(join));
}

// open-loop: 연결 후 경과 시간 × 속도 = 지금까지 보냈어야 할 수. 모자란 만큼 쌓는다
// 서버가 못 받아 못 보낸 바이트가 한도를 넘으면 그 몫은 쌓지 않고 건너뛴다 - 메모리가 한없이 늘지 않게, 그리고 건너뜀 = 수용량 초과 신호
void LoadClient::queueOpenLoop(LoadSession& session, int64_t nowQpc)
{
	int64_t due = (nowQpc - session.mStartQpc) * mRatePerSession / mQpcFreq;
	int64_t count = due - session.mDue;
	if (count <= 0)
	{
		return;
	}
	session.mDue = due;
	size_t backlog = session.mSendBuf.size() - session.mSendOff;
	if (backlog + static_cast<size_t>(count) * mMsgSize > MAX_CLIENT_BACKLOG)
	{
		mSkippedMsgCount += count;
		return;
	}
	queueMessages(session, count);
	mQueuedMsgCount += count;
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

	if (mRatePerSession > 0)
	{
		LARGE_INTEGER now;
		QueryPerformanceCounter(&now);
		for (auto& [s, session] : sessions)
		{
			if (session.mState == SessionState::Connected)
			{
				queueOpenLoop(session, now.QuadPart);
			}
		}
	}

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
	timeout.tv_usec = (mRatePerSession > 0) ? 1000 : 100 * 1000;	// open-loop는 1ms마다 깨어나 쌓는다. closed-loop는 mRunning 확인용
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
				if (mRoomSize > 0)
				{
					queueJoin(session);	// 첫 묶음은 JoinOk를 받은 뒤
				}
				else if (mRatePerSession > 0)
				{
					LARGE_INTEGER now;
					QueryPerformanceCounter(&now);
					session.mStartQpc = now.QuadPart;	// 이 시각부터 속도대로 쌓는다
				}
				else
				{
					queueBatch(session);	// 첫 묶음 - 다음 바퀴의 writeSet으로 나간다
				}
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
	else if (session.mSendOff >= MAX_CLIENT_BACKLOG)
	{
		// open-loop는 다 보내기 전에 계속 뒤에 붙이므로, 보낸 앞부분을 지우지 않으면 버퍼가 한없이 커진다
		session.mSendBuf.erase(session.mSendBuf.begin(), session.mSendBuf.begin() + session.mSendOff);
		session.mSendOff = 0;
	}
	return true;
}

// 받은 바이트를 패킷 단위로 처리한다
// - Data: 순번이 보낸 순서대로인지 확인 (유실/중복/순서 바뀜 검출). 묶음이 다 돌아오면 다음 묶음
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
	int64_t chatCount = 0;
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

		if (mRoomSize > 0)
		{
			onChatPacket(session, payload, header.len, echoCount, chatCount);
		}
		else if (header.len == mPayloadSize)
		{
			uint64_t seq = 0;
			memcpy(&seq, payload, sizeof(seq));
			bool openLoop = mRatePerSession > 0;	// open-loop는 묶음이 없다 - 순번만 확인
			if (seq != session.mRecvSeq || (!openLoop && session.mWaiting == 0))
			{
				std::cout << "[LoadClient] 순번 어긋남 expected=" << session.mRecvSeq << " actual=" << seq
					<< " waiting=" << session.mWaiting << "\n";
				__debugbreak();
			}
			++session.mRecvSeq;
			++echoCount;
			if (!openLoop)
			{
				recordLatency(session);	// open-loop는 세션당 묶음이 여러 개 날아가 시작 시각 하나로 잴 수 없다
			}
			if (!openLoop && --session.mWaiting == 0)
			{
				++mBatchCount;
				queueBatch(session);	// 묶음이 전부 돌아옴 - 다음 묶음
			}
		}
		else
		{
			std::cout << "[LoadClient] 예상 밖 길이 len=" << header.len << "\n";
			__debugbreak();
		}
		pos += packetSize;
	}
	session.mRecvBuf.erase(session.mRecvBuf.begin(), session.mRecvBuf.begin() + pos);	// 잘린 패킷만 남긴다
	mEchoMsgCount += echoCount;
	if (chatCount > 0)
	{
		mChatRecvCount += chatCount;
	}
	return true;
}

// 채팅 패킷 - 어긋나면 서버의 순서 보장이 깨진 것이므로 멈춰서 본다(에코의 순번 검사와 같은 방침)
// 방 순번: 서버가 방 락 안에서 매기고 같은 락 안에서 방 전원의 링에 넣으므로, 입장 후 첫 메시지부터는 +1씩 와야 한다
//          받는 사람마다 순서가 다르거나 중간에 빠지면 여기서 걸린다
void LoadClient::onChatPacket(LoadSession& session, const char* payload, uint16_t len, int64_t& ownCount, int64_t& chatCount)
{
	eChatType type = static_cast<eChatType>(payload[0]);
	if (type == eChatType::JoinOk && len == sizeof(ChatJoinOk) && !session.mJoined)
	{
		ChatJoinOk ok;
		memcpy(&ok, payload, sizeof(ok));
		if (ok.room != static_cast<uint32_t>(session.mSlot / mRoomSize))
		{
			std::cout << "[LoadClient] JoinOk 방 번호 어긋남 expected=" << session.mSlot / mRoomSize << " actual=" << ok.room << "\n";
			__debugbreak();
		}
		session.mJoined = true;
		++mJoinedCount;
		queueBatch(session);	// 첫 묶음
		return;
	}
	if (type != eChatType::Chat || len != mPayloadSize || !session.mJoined)
	{
		std::cout << "[LoadClient] 예상 밖 채팅 패킷 type=" << static_cast<int>(type) << " len=" << len
			<< " joined=" << session.mJoined << "\n";
		__debugbreak();
	}

	ChatMsg chat;
	memcpy(&chat, payload, sizeof(chat));
	if (session.mRoomSeq != 0 && chat.roomSeq != session.mRoomSeq + 1)
	{
		std::cout << "[LoadClient] 방 순번 어긋남 expected=" << session.mRoomSeq + 1 << " actual=" << chat.roomSeq
			<< " room=" << session.mSlot / mRoomSize << "\n";
		__debugbreak();
	}
	session.mRoomSeq = chat.roomSeq;
	++chatCount;
	if (chat.senderKey != session.mSenderKey)
	{
		return;	// 남의 메시지 - 수신량으로만 센다
	}

	// 자기 메시지 - 보낸 순서대로, 이번 묶음 안에서 돌아와야 한다
	if (chat.senderSeq != session.mRecvSeq || session.mWaiting == 0)
	{
		std::cout << "[LoadClient] 자기 메시지 순번 어긋남 expected=" << session.mRecvSeq << " actual=" << chat.senderSeq
			<< " waiting=" << session.mWaiting << "\n";
		__debugbreak();
	}
	++session.mRecvSeq;
	++ownCount;
	recordLatency(session);	// 서버의 수신 → 방 락 → 모아 보내기 → 방송을 거쳐 자기 몫이 돌아오기까지
	if (--session.mWaiting == 0)
	{
		++mBatchCount;
		queueBatch(session);	// 자기 묶음이 전부 돌아옴 - 다음 묶음
	}
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
	int64_t lastQueued = 0;
	int64_t lastSkipped = 0;
	int64_t lastChatRecv = 0;
	std::vector<uint64_t> lastLatency(LatencyHistogram::BUCKET_COUNT, 0);
	while (mRunning)
	{
		double intervalSec = ticker.waitNext();
		CpuUsage usage = cpu.sample();
		int64_t nowMsg = mEchoMsgCount.load();
		int64_t nowBatch = mBatchCount.load();
		int64_t nowQueued = mQueuedMsgCount.load();
		int64_t nowSkipped = mSkippedMsgCount.load();
		int64_t nowChatRecv = mChatRecvCount.load();
		double cores = (usage.processUser + usage.processKernel) * coreCnt / 100.0;
		std::cout << std::fixed << std::setprecision(0)
			<< "[LoadClient] 메시지/s=" << (nowMsg - lastMsg) / intervalSec
			<< " MB/s=" << (nowMsg - lastMsg) * mMsgSize / intervalSec / 1e6
			<< " 묶음/s=" << (nowBatch - lastBatch) / intervalSec
			<< " 연결=" << mConnectedCount.load()
			<< " unexpectedClose=" << mUnexpectedClose.load()
			<< " connectFail=" << mFailCount.load();
		if (mRatePerSession > 0)
		{
			// 목표 = 세션 수 × 속도, 보냄 = 실제로 쌓은 수, 건너뜀 = 서버가 못 받아 버린 수
			std::cout << " 목표/s=" << static_cast<double>(mConnectedCount.load()) * mRatePerSession
				<< " 보냄/s=" << (nowQueued - lastQueued) / intervalSec
				<< " 건너뜀/s=" << (nowSkipped - lastSkipped) / intervalSec;
		}
		if (mRoomSize > 0)
		{
			// 메시지/s = 보낸(돌아온 자기) 메시지, 수신메시지/s = 받은 Chat 전부. 수신/보냄은 방 크기에 가까워야 한다
			int64_t sent = nowMsg - lastMsg;
			int64_t chatRecv = nowChatRecv - lastChatRecv;
			std::cout << " 수신메시지/s=" << chatRecv / intervalSec
				<< " 수신MB/s=" << chatRecv * mMsgSize / intervalSec / 1e6
				<< std::setprecision(2) << " 수신/보냄=" << ((sent > 0) ? static_cast<double>(chatRecv) / sent : 0.0)
				<< std::setprecision(0) << " 입장=" << mJoinedCount.load();
		}
		if (mRatePerSession == 0)
		{
			// 워커별 누적 히스토그램을 합쳐 1초 차분 = 직전 1초 동안 돌아온 자기 메시지의 왕복 지연 분포
			std::vector<uint64_t> nowLatency(LatencyHistogram::BUCKET_COUNT, 0);
			for (const std::unique_ptr<LatencyHistogram>& hist : mLatencyHists)
			{
				hist->addTo(nowLatency);
			}
			std::vector<uint64_t> diff(nowLatency.size());
			for (size_t i = 0; i < nowLatency.size(); ++i)
			{
				diff[i] = nowLatency[i] - lastLatency[i];
			}
			LatencySummary lat = summarizeLatency(diff);
			std::cout << " 지연(us) p50=" << lat.p50 << " p99=" << lat.p99 << " max=" << lat.max;
			lastLatency = std::move(nowLatency);
		}
		std::cout << std::setprecision(2) << " cpu코어=" << cores << "/" << mThreadCnt << "\n";
		lastMsg = nowMsg;
		lastBatch = nowBatch;
		lastQueued = nowQueued;
		lastSkipped = nowSkipped;
		lastChatRecv = nowChatRecv;
	}
}
