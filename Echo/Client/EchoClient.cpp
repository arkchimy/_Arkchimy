#include <WinSock2.h>
#include <WS2tcpip.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>
#include "EchoClient.h"

constexpr ULONGLONG CONNECT_RETRY_MS = 100;   // 접속 실패 뒤 다시 걸기까지
constexpr ULONGLONG SHUTDOWN_WAIT_MS = 5000;  // 종료 요청 뒤 진행 중인 묶음의 응답을 기다리는 상한

// 모든 세션이 끝났나 - 종료 요청 뒤 진행 중인 묶음을 마치고 닫은 세션은 Done
static bool isAllDone(const std::vector<ClientSession>& sessions)
{
	for (const ClientSession& session : sessions)
	{
		if (session.State != eSessionState::Done)
		{
			return false;
		}
	}
	return true;
}

// 상태에 따라 select에 맡길 집합을 고른다
static void addToSet(const ClientSession& session, fd_set* outReadSet, fd_set* outWriteSet, fd_set* outExceptSet)
{
	switch (session.State)
	{
	case eSessionState::Connecting:
		FD_SET(session.Sock, outWriteSet);  // 접속 성공
		FD_SET(session.Sock, outExceptSet); // 접속 실패
		break;
	case eSessionState::Sending:
		FD_SET(session.Sock, outWriteSet);
		break;
	case eSessionState::Receiving:
	case eSessionState::WaitClose:
		FD_SET(session.Sock, outReadSet);
		break;
	default:
		break; // Idle, Done - 소켓 없음
	}
}

// 요청 100개와 기대 응답 100개를 같은 순서로 만든다
static void buildBatch(ClientSession& session)
{
	for (int32_t i = 0; i < BATCH_MSG_CNT; ++i)
	{
		PacketHeader header;
		header.Len = static_cast<uint16_t>(sizeof(EchoReq));

		EchoReq req;
		req.Type = eEchoType::EchoReq;
		req.Value = session.NextValue;

		EchoRes res;
		res.Type = eEchoType::EchoRes;
		res.Value = session.NextValue;
		++session.NextValue;

		char* const send = session.SendBuf + i * MSG_SIZE;
		char* const expect = session.ExpectBuf + i * MSG_SIZE;
		memcpy(send, &header, PACKET_HEADER_SIZE);
		memcpy(send + PACKET_HEADER_SIZE, &req, sizeof(req));
		memcpy(expect, &header, PACKET_HEADER_SIZE);
		memcpy(expect + PACKET_HEADER_SIZE, &res, sizeof(res));
	}
	session.BatchSize = BATCH_BUF_SIZE;
	session.SentSize = 0;
	session.RecvSize = 0;
}

// 길이가 범위 밖인 헤더 하나 - 접속마다 4097(최대 + 1)과 65535(-1을 부호 없이 읽은 값)를 번갈아
static void buildBadHeader(ClientSession& session)
{
	PacketHeader header;
	header.Len = (session.ConnectCnt % 2 == 1) ? static_cast<uint16_t>(MAX_PAYLOAD_SIZE + 1) : static_cast<uint16_t>(-1);
	memcpy(session.SendBuf, &header, PACKET_HEADER_SIZE);
	session.BatchSize = PACKET_HEADER_SIZE;
	session.SentSize = 0;
	session.RecvSize = 0;
}

static void closeSocketRst(const SOCKET sock)
{
	LINGER linger{};
	linger.l_onoff = 1;
	linger.l_linger = 0; // 0초 linger = closesocket이 RST를 보낸다
	setsockopt(sock, SOL_SOCKET, SO_LINGER, reinterpret_cast<const char*>(&linger), sizeof(linger));
	closesocket(sock);
}

EchoClient::~EchoClient()
{
	Stop(); // 스레드를 남긴 채 소멸하면 std::thread 소멸자가 프로그램을 끝낸다
}

bool EchoClient::Start(const ClientConfig& config)
{
	if (mSelectThread.joinable())
	{
		return false; // 이미 실행 중
	}
	mConfig = config;
	mServerAddr = SOCKADDR_IN{};
	mServerAddr.sin_family = AF_INET;
	mServerAddr.sin_port = htons(mConfig.Port);
	if (inet_pton(AF_INET, mConfig.IP.c_str(), &mServerAddr.sin_addr) != 1)
	{
		return false;
	}
	mbStopRequested = false;
	mSelectThread = std::thread(&EchoClient::runSelectLoop, this);
	return true;
}

void EchoClient::Stop()
{
	mbStopRequested = true;
	if (mSelectThread.joinable())
	{
		mSelectThread.join();
	}
}

void EchoClient::runSelectLoop()
{
	// 1. 세션 만들기 - 앞쪽은 정상, 뒤쪽은 BadHeader
	const int32_t sessionCnt = mConfig.NormalCnt + mConfig.BadHeaderCnt;
	std::vector<ClientSession> sessions(sessionCnt);
	for (int32_t i = 0; i < sessionCnt; ++i)
	{
		sessions[i].Role = (i < mConfig.NormalCnt) ? eSessionRole::Normal : eSessionRole::BadHeader;
		sessions[i].NextValue = static_cast<uint64_t>(i) << 32; // 세션마다 겹치지 않는 값 - 다른 세션의 응답이 섞이면 바로 드러난다
	}

	ULONGLONG shutdownDeadline = 0; // 종료 요청을 처음 본 시각 + 상한

	// 2. 모든 세션이 Done이 될 때까지
	while (!isAllDone(sessions))
	{
		bool bAnyEvent = false;

		// 3. 64개씩 나눠 select - Windows select는 한 번에 소켓 FD_SETSIZE(64)개까지
		for (int32_t begin = 0; begin < sessionCnt; begin += SELECT_GROUP_SIZE)
		{
			const int32_t end = (begin + SELECT_GROUP_SIZE < sessionCnt) ? begin + SELECT_GROUP_SIZE : sessionCnt;

			fd_set readSet;
			fd_set writeSet;
			fd_set exceptSet;
			FD_ZERO(&readSet);
			FD_ZERO(&writeSet);
			FD_ZERO(&exceptSet);
			for (int32_t i = begin; i < end; ++i)
			{
				addToSet(sessions[i], &readSet, &writeSet, &exceptSet);
			}
			if (readSet.fd_count + writeSet.fd_count + exceptSet.fd_count == 0)
			{
				continue; // 이 묶음에 소켓이 없음 - 빈 집합으로 select하면 WSAEINVAL
			}

			// 기다리지 않는다 - 한 묶음에서 기다리면 뒤 묶음의 일이 밀린다
			timeval noWait{ 0, 0 };
			const int selectResult = select(0, &readSet, &writeSet, &exceptSet, &noWait);
			if (selectResult == SOCKET_ERROR)
			{
				// 그대로 넘어가면 이 묶음의 세션들이 매 바퀴 같은 오류로 조용히 멈춘다
				fprintf(stderr, "[EchoClient] select 실패 err=%d (세션 %d-%d)\n", WSAGetLastError(), begin, end - 1);
				__debugbreak();
			}
			if (selectResult > 0)
			{
				bAnyEvent = true;
				for (int32_t i = begin; i < end; ++i)
				{
					handleEvent(sessions[i], readSet, writeSet, exceptSet);
				}
			}
		}

		// 4. 소켓이 없는 세션 - 새로 접속하거나, 종료 요청이면 Done
		for (ClientSession& session : sessions)
		{
			updateIdle(session);
		}

		// 5. 종료 요청 뒤 상한이 지나도 응답이 끝나지 않은 세션은 포기하고 닫는다
		if (mbStopRequested)
		{
			const ULONGLONG now = GetTickCount64();
			if (shutdownDeadline == 0)
			{
				shutdownDeadline = now + SHUTDOWN_WAIT_MS;
			}
			else if (now >= shutdownDeadline)
			{
				for (ClientSession& session : sessions)
				{
					if (session.State == eSessionState::Sending || session.State == eSessionState::Receiving || session.State == eSessionState::WaitClose)
					{
						++mStat.ShutdownTimeoutCnt;
						closeConnected(session, true);
					}
					else if (session.State == eSessionState::Connecting)
					{
						closesocket(session.Sock); // 아직 연결 전 - 기다리던 응답이 없으므로 세지 않는다
						session.Sock = INVALID_SOCKET;
						session.State = eSessionState::Done;
					}
				}
			}
		}

		if (!bAnyEvent)
		{
			Sleep(1); // 모든 묶음에 일이 없으면 잠깐 쉰다 - 바쁜 대기를 피한다
		}
	}
}

void EchoClient::handleEvent(ClientSession& session, const fd_set& readSet, const fd_set& writeSet, const fd_set& exceptSet)
{
	switch (session.State)
	{
	case eSessionState::Connecting:
		if (FD_ISSET(session.Sock, &exceptSet))
		{
			++mStat.ConnectFailCnt;
			closesocket(session.Sock);
			session.Sock = INVALID_SOCKET;
			session.RetryTick = GetTickCount64() + CONNECT_RETRY_MS;
			session.State = eSessionState::Idle;
		}
		else if (FD_ISSET(session.Sock, &writeSet))
		{
			onConnected(session);
		}
		break;
	case eSessionState::Sending:
		if (FD_ISSET(session.Sock, &writeSet))
		{
			onSendable(session);
		}
		break;
	case eSessionState::Receiving:
		if (FD_ISSET(session.Sock, &readSet))
		{
			onReceivable(session);
		}
		break;
	case eSessionState::WaitClose:
		if (FD_ISSET(session.Sock, &readSet))
		{
			onWaitCloseReadable(session);
		}
		break;
	default:
		break; // Idle, Done - select에 넣지 않으므로 알림이 없다
	}
}

void EchoClient::updateIdle(ClientSession& session)
{
	if (session.State != eSessionState::Idle)
	{
		return;
	}
	if (mbStopRequested)
	{
		session.State = eSessionState::Done;
		return;
	}
	if (GetTickCount64() < session.RetryTick)
	{
		return; // 접속 실패 뒤 잠깐 쉬는 중
	}
	startConnect(session);
}

void EchoClient::startConnect(ClientSession& session)
{
	const SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (sock == INVALID_SOCKET)
	{
		++mStat.ConnectFailCnt;
		session.RetryTick = GetTickCount64() + CONNECT_RETRY_MS;
		return;
	}

	u_long nonBlocking = 1;
	ioctlsocket(sock, FIONBIO, &nonBlocking);
	const BOOL noDelay = TRUE; // 작은 send를 운영체제가 모아 보내지 않게 - 잘린 위치가 그대로 서버에 도착하도록
	setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));

	++session.ConnectCnt;
	if (connect(sock, reinterpret_cast<const SOCKADDR*>(&mServerAddr), sizeof(mServerAddr)) == SOCKET_ERROR &&
		WSAGetLastError() != WSAEWOULDBLOCK)
	{
		++mStat.ConnectFailCnt;
		closesocket(sock);
		session.RetryTick = GetTickCount64() + CONNECT_RETRY_MS;
		return;
	}
	session.Sock = sock;
	session.State = eSessionState::Connecting;
}

void EchoClient::onConnected(ClientSession& session)
{
	++mStat.ConnectCnt;
	++mStat.ConnectedCnt;

	if (mbStopRequested)
	{
		closeConnected(session, false); // 종료 요청 뒤에 연결됨 - 보낸 것이 없으므로 바로 닫는다
		return;
	}

	if (session.Role == eSessionRole::Normal)
	{
		session.CloseTick = 0;
		if (mConfig.LifeMaxSec > 0)
		{
			std::uniform_int_distribution<int32_t> lifeSec(mConfig.LifeMinSec, mConfig.LifeMaxSec);
			session.CloseTick = GetTickCount64() + static_cast<ULONGLONG>(lifeSec(mRng)) * 1000;
		}
		buildBatch(session);
	}
	else
	{
		buildBadHeader(session);
	}
	session.State = eSessionState::Sending;
}

void EchoClient::onSendable(ClientSession& session)
{
	// 정상 세션은 SEND_CHUNK_SIZE씩 잘라 보낸다 - 서버가 패킷 경계와 다른 곳에서 잘린 데이터를 받게
	const int32_t chunkSize = (session.Role == eSessionRole::Normal) ? SEND_CHUNK_SIZE : session.BatchSize;
	while (session.SentSize < session.BatchSize)
	{
		const int32_t len = (std::min)(chunkSize, session.BatchSize - session.SentSize);
		const int sent = send(session.Sock, session.SendBuf + session.SentSize, len, 0);
		if (sent == SOCKET_ERROR)
		{
			if (WSAGetLastError() == WSAEWOULDBLOCK)
			{
				return; // 커널 송신 버퍼가 참 - 다음 쓰기 알림에서 이어서
			}
			if (session.Role == eSessionRole::Normal)
			{
				++mStat.UnexpectedCloseCnt; // 정상 세션인데 보내는 중에 연결이 끊김
			}
			else
			{
				++mStat.BadHeaderSendFailCnt; // 헤더를 보내기도 전에 끊김 - 서버의 BadPacketCnt에 안 잡히는 시도
			}
			closeConnected(session, true);
			return;
		}
		session.SentSize += sent;
	}

	if (session.Role == eSessionRole::Normal)
	{
		session.State = eSessionState::Receiving;
	}
	else
	{
		++mStat.BadHeaderSentCnt;
		session.State = eSessionState::WaitClose;
	}
}

void EchoClient::onReceivable(ClientSession& session)
{
	char buf[BATCH_BUF_SIZE];
	const int32_t want = session.BatchSize - session.RecvSize; // 보낸 만큼만 받는다 - 그보다 많이 오면 다음 recv에서 드러난다
	const int received = recv(session.Sock, buf, want, 0);
	if (received == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK)
	{
		return;
	}
	if (received <= 0)
	{
		++mStat.UnexpectedCloseCnt; // 정상 세션인데 서버가 먼저 끊음(0) 또는 연결 오류
		closeConnected(session, true);
		return;
	}

	if (memcmp(buf, session.ExpectBuf + session.RecvSize, received) != 0)
	{
		// 받은 응답이 기대와 다르다 - 어느 위치인지 남기고 그 자리에서 멈춘다
		int32_t offset = 0;
		while (buf[offset] == session.ExpectBuf[session.RecvSize + offset])
		{
			++offset;
		}
		fprintf(stderr, "[EchoClient] 응답 불일치: 접속 %u번째, 묶음 안 위치 %d, 기대 0x%02X, 받음 0x%02X\n", session.ConnectCnt,
			session.RecvSize + offset, static_cast<unsigned char>(session.ExpectBuf[session.RecvSize + offset]),
			static_cast<unsigned char>(buf[offset]));
		__debugbreak();
	}
	session.RecvSize += received;

	if (session.RecvSize == session.BatchSize)
	{
		onBatchDone(session);
	}
}

void EchoClient::onBatchDone(ClientSession& session)
{
	++mStat.BatchCnt;

	if (mbStopRequested)
	{
		closeConnected(session, false); // 응답을 다 받았다 - 종료 요청이면 닫고 끝
		return;
	}
	if (session.CloseTick != 0 && GetTickCount64() >= session.CloseTick)
	{
		// 수명이 다 됐다 - 접속마다 FIN과 RST를 번갈아 쓴다
		const bool bRst = (session.ConnectCnt % 2 == 0);
		if (bRst)
		{
			++mStat.RstCloseCnt;
		}
		else
		{
			++mStat.FinCloseCnt;
		}
		closeConnected(session, bRst);
		return;
	}
	buildBatch(session);
	session.State = eSessionState::Sending;
}

void EchoClient::onWaitCloseReadable(ClientSession& session)
{
	char buf[64];
	const int received = recv(session.Sock, buf, sizeof(buf), 0);
	if (received == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK)
	{
		return;
	}
	if (received > 0)
	{
		// 서버는 비정상 헤더를 받으면 아무것도 보내지 않고 끊어야 한다
		fprintf(stderr, "[EchoClient] 비정상 세션에 데이터가 옴: %d바이트\n", received);
		__debugbreak();
	}
	++mStat.BadHeaderClosedCnt; // 0(서버가 닫음) 또는 연결 오류(서버가 RST)
	closeConnected(session, false);
}

void EchoClient::closeConnected(ClientSession& session, const bool bRst)
{
	if (bRst)
	{
		closeSocketRst(session.Sock);
	}
	else
	{
		closesocket(session.Sock); // FIN
	}
	session.Sock = INVALID_SOCKET;
	--mStat.ConnectedCnt;
	session.State = mbStopRequested ? eSessionState::Done : eSessionState::Idle;
}
