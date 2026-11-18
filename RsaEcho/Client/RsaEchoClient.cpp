#include <WinSock2.h>
#include <WS2tcpip.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include "RsaEchoClient.h"

constexpr ULONGLONG CONNECT_RETRY_MS = 100;   // 접속 실패 뒤 다시 걸기까지
constexpr ULONGLONG SHUTDOWN_WAIT_MS = 10000; // 종료 요청 뒤 진행 중인 응답을 기다리는 상한
static const char* const TEST_PASSWORD = "test-password";

// 에코 데이터 - 값으로 정해지므로 보낸 것을 저장하지 않고 응답을 확인할 수 있다
static void fillEchoData(const uint64_t value, uint8_t (&outData)[RSA_ECHO_DATA_LEN])
{
	for (int32_t i = 0; i < RSA_ECHO_DATA_LEN; ++i)
	{
		outData[i] = static_cast<uint8_t>(value * 31 + static_cast<uint64_t>(i));
	}
}

// 세션 번호로 아이디를 정한다 - 클라를 다시 띄워도 같은 아이디라 가입은 Exists, 로그인은 성공
static void makeAccountID(const int32_t index, char (&outID)[ACCOUNT_ID_LEN])
{
	sprintf_s(outID, "rsa_%05d", index);
}

static bool isAllDone(const std::vector<ClientSession>& sessions, const int32_t begin, const int32_t end)
{
	for (int32_t i = begin; i < end; ++i)
	{
		if (sessions[i].State != eSessionState::Done)
		{
			return false;
		}
	}
	return true;
}

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
	case eSessionState::Paused:
		FD_SET(session.Sock, outReadSet);
		break;
	default:
		break; // Idle, Done - 소켓 없음
	}
}

static void closeSocketRst(const SOCKET sock)
{
	LINGER linger{};
	linger.l_onoff = 1;
	linger.l_linger = 0; // 0초 linger = closesocket이 RST를 보낸다
	setsockopt(sock, SOL_SOCKET, SO_LINGER, reinterpret_cast<const char*>(&linger), sizeof(linger));
	closesocket(sock);
}

RsaEchoClient::~RsaEchoClient()
{
	Stop(); // 스레드를 남긴 채 소멸하면 std::thread 소멸자가 프로그램을 끝낸다
}

bool RsaEchoClient::Start(const ClientConfig& config, const uint8_t (&serverSignKey)[RSA_MODULUS_LEN])
{
	if (!mSelectThreads.empty())
	{
		return false; // 이미 실행 중
	}
	mConfig = config;
	memcpy(mServerSignKey, serverSignKey, sizeof(mServerSignKey));
	mServerAddr = SOCKADDR_IN{};
	mServerAddr.sin_family = AF_INET;
	mServerAddr.sin_port = htons(mConfig.Port);
	if (inet_pton(AF_INET, mConfig.IP.c_str(), &mServerAddr.sin_addr) != 1)
	{
		return false;
	}

	std::vector<ClientSession> sessions(mConfig.SessionCnt); // LoginChannel은 옮길 수 없어 만든 뒤 swap
	for (int32_t i = 0; i < mConfig.SessionCnt; ++i)
	{
		sessions[i].Index = i;
		sessions[i].Rng.seed(static_cast<uint32_t>(i) * 7919u + 1u); // 같은 설정이면 같은 수명 순서
	}
	mSessions.swap(sessions);

	mbStopRequested = false;
	const int32_t perThread = (mConfig.SessionCnt + mConfig.ThreadCnt - 1) / mConfig.ThreadCnt;
	for (int32_t begin = 0; begin < mConfig.SessionCnt; begin += perThread)
	{
		const int32_t end = (std::min)(begin + perThread, mConfig.SessionCnt);
		mSelectThreads.emplace_back(&RsaEchoClient::runSelectLoop, this, begin, end);
	}
	return true;
}

void RsaEchoClient::TogglePause()
{
	mbPaused = !mbPaused; // main만 바꾸므로 읽고 쓰기가 나뉘어도 된다
}

void RsaEchoClient::Stop()
{
	mbStopRequested = true;
	for (std::thread& thread : mSelectThreads)
	{
		thread.join();
	}
	mSelectThreads.clear();
}

void RsaEchoClient::runSelectLoop(const int32_t begin, const int32_t end)
{
	ULONGLONG shutdownDeadline = 0; // 종료 요청을 처음 본 시각 + 상한

	while (!isAllDone(mSessions, begin, end))
	{
		bool bAnyEvent = false;

		// 1. 64개씩 나눠 select - 기다리지 않는다(한 묶음에서 기다리면 뒤 묶음의 일이 밀린다)
		for (int32_t groupBegin = begin; groupBegin < end; groupBegin += SELECT_GROUP_SIZE)
		{
			const int32_t groupEnd = (std::min)(groupBegin + SELECT_GROUP_SIZE, end);
			fd_set readSet;
			fd_set writeSet;
			fd_set exceptSet;
			FD_ZERO(&readSet);
			FD_ZERO(&writeSet);
			FD_ZERO(&exceptSet);
			for (int32_t i = groupBegin; i < groupEnd; ++i)
			{
				addToSet(mSessions[i], &readSet, &writeSet, &exceptSet);
			}
			if (readSet.fd_count + writeSet.fd_count + exceptSet.fd_count == 0)
			{
				continue; // 빈 집합으로 select하면 WSAEINVAL
			}
			timeval noWait{ 0, 0 };
			const int selectResult = select(0, &readSet, &writeSet, &exceptSet, &noWait);
			if (selectResult == SOCKET_ERROR)
			{
				fprintf(stderr, "[RsaEchoClient] select 실패 err=%d (세션 %d-%d)\n", WSAGetLastError(), groupBegin, groupEnd - 1);
				__debugbreak();
			}
			if (selectResult > 0)
			{
				bAnyEvent = true;
				for (int32_t i = groupBegin; i < groupEnd; ++i)
				{
					handleEvent(mSessions[i], readSet, writeSet, exceptSet);
				}
			}
		}

		// 2. select 알림 없이 진행하는 세션 - 새로 접속, 일시정지가 풀린 세션
		for (int32_t i = begin; i < end; ++i)
		{
			updateIdle(mSessions[i]);
			updatePaused(mSessions[i]);
		}

		// 3. 종료 요청 뒤 상한이 지나도 응답이 끝나지 않은 세션은 포기하고 닫는다
		if (mbStopRequested)
		{
			const ULONGLONG now = GetTickCount64();
			if (shutdownDeadline == 0)
			{
				shutdownDeadline = now + SHUTDOWN_WAIT_MS;
			}
			else if (now >= shutdownDeadline)
			{
				for (int32_t i = begin; i < end; ++i)
				{
					ClientSession& session = mSessions[i];
					if (session.State == eSessionState::Sending || session.State == eSessionState::Receiving)
					{
						++mStat.ShutdownTimeoutCnt;
						closeConnected(session, true);
					}
					else if (session.State == eSessionState::Connecting)
					{
						closesocket(session.Sock);
						session.Sock = INVALID_SOCKET;
						session.State = eSessionState::Done;
					}
				}
			}
		}

		if (!bAnyEvent)
		{
			Sleep(1);
		}
	}
}

void RsaEchoClient::handleEvent(ClientSession& session, const fd_set& readSet, const fd_set& writeSet, const fd_set& exceptSet)
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
	case eSessionState::Paused:
		if (FD_ISSET(session.Sock, &readSet))
		{
			onPausedReadable(session);
		}
		break;
	default:
		break;
	}
}

void RsaEchoClient::updateIdle(ClientSession& session)
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
	if (mbPaused || GetTickCount64() < session.RetryTick)
	{
		return;
	}
	startConnect(session);
}

void RsaEchoClient::updatePaused(ClientSession& session)
{
	if (session.State != eSessionState::Paused)
	{
		return;
	}
	if (mbPaused && !mbStopRequested)
	{
		return;
	}
	startNextBatch(session);
}

void RsaEchoClient::startConnect(ClientSession& session)
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
	const BOOL noDelay = TRUE; // 514바이트 패킷을 묶음으로 보낸다 - Nagle이 마지막 패킷을 붙잡지 않게
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

void RsaEchoClient::onConnected(ClientSession& session)
{
	++mStat.ConnectCnt;
	++mStat.ConnectedCnt;
	if (mbStopRequested)
	{
		closeConnected(session, false); // 보낸 것이 없으므로 바로 닫는다
		return;
	}
	session.CloseTick = 0;
	if (mConfig.LifeMaxSec > 0)
	{
		std::uniform_int_distribution<int32_t> lifeSec(mConfig.LifeMinSec, mConfig.LifeMaxSec);
		session.CloseTick = GetTickCount64() + static_cast<ULONGLONG>(lifeSec(session.Rng)) * 1000;
	}
	session.RecvSize = 0;
	session.NextValue = 0;
	session.ExpectValue = 0;

	// 접속마다 새 클라 임시 키 쌍(약 50ms - 이 select 스레드의 다른 세션은 그동안 기다린다)
	session.Channel.Clear();
	ClientHello hello{};
	if (!session.Channel.StartClient(&hello))
	{
		failProtocol(session, "클라 임시 키 쌍을 만들지 못함(CNG)");
		return;
	}
	session.SendSize = 0;
	session.SentSize = 0;
	queuePacket(session, &hello, static_cast<int32_t>(sizeof(hello)));
	session.Step = eSessionStep::Hello;
	session.PendingResCnt = 1;
	session.State = eSessionState::Sending;
}

void RsaEchoClient::onSendable(ClientSession& session)
{
	while (session.SentSize < session.SendSize)
	{
		const int sent = send(session.Sock, session.SendBuf + session.SentSize, session.SendSize - session.SentSize, 0);
		if (sent == SOCKET_ERROR)
		{
			if (WSAGetLastError() == WSAEWOULDBLOCK)
			{
				return; // 커널 송신 버퍼가 참 - 다음 쓰기 알림에서 이어서
			}
			++mStat.UnexpectedCloseCnt;
			closeConnected(session, true);
			return;
		}
		session.SentSize += sent;
	}
	session.State = eSessionState::Receiving;
}

void RsaEchoClient::onReceivable(ClientSession& session)
{
	const int received = recv(session.Sock, session.RecvBuf + session.RecvSize, RECV_BUF_SIZE - session.RecvSize, 0);
	if (received == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK)
	{
		return;
	}
	if (received <= 0)
	{
		++mStat.UnexpectedCloseCnt; // 서버가 먼저 끊음(0) 또는 연결 오류
		closeConnected(session, true);
		return;
	}
	session.RecvSize += received;

	// 완성된 패킷을 모두 꺼낸다
	int32_t offset = 0;
	while (session.RecvSize - offset >= PACKET_HEADER_SIZE)
	{
		PacketHeader header;
		memcpy(&header, session.RecvBuf + offset, PACKET_HEADER_SIZE);
		if (header.Len > RECV_BUF_SIZE - PACKET_HEADER_SIZE)
		{
			failProtocol(session, "받은 패킷 길이가 수신 버퍼보다 큼");
			return;
		}
		if (session.RecvSize - offset < PACKET_HEADER_SIZE + header.Len)
		{
			break; // 나머지는 다음 recv에서
		}
		if (!onPacket(session, session.RecvBuf + offset + PACKET_HEADER_SIZE, header.Len))
		{
			return; // 세션을 닫았다
		}
		offset += PACKET_HEADER_SIZE + header.Len;
	}
	session.RecvSize -= offset;
	memmove(session.RecvBuf, session.RecvBuf + offset, session.RecvSize);
}

bool RsaEchoClient::onPacket(ClientSession& session, const char* payload, const int32_t len)
{
	if (session.PendingResCnt <= 0 || session.State != eSessionState::Receiving)
	{
		failProtocol(session, "기다리지 않던 패킷"); // 보내던 중에 응답이 오거나 요청보다 응답이 많음
		return false;
	}
	if (session.Step == eSessionStep::Hello)
	{
		return onServerHello(session, payload, len);
	}

	std::string plain;
	if (!session.Channel.Open(payload, static_cast<size_t>(len), &plain))
	{
		failProtocol(session, "잠근 응답을 풀지 못함(서명·번호·패딩)");
		return false;
	}
	bool bOk = false;
	switch (session.Step)
	{
	case eSessionStep::Register:
		bOk = onRegisterRes(session, plain);
		break;
	case eSessionStep::Login:
		bOk = onLoginRes(session, plain);
		break;
	case eSessionStep::Echo:
		bOk = onEchoRes(session, plain);
		break;
	default:
		failProtocol(session, "알 수 없는 단계");
		break;
	}
	return bOk;
}

bool RsaEchoClient::onServerHello(ClientSession& session, const char* payload, const int32_t len)
{
	if (len != static_cast<int32_t>(sizeof(ServerHello)))
	{
		failProtocol(session, "ServerHello 길이가 다름");
		return false;
	}
	ServerHello hello;
	memcpy(&hello, payload, sizeof(hello));
	if (!session.Channel.FinishClient(hello, mServerSignKey))
	{
		failProtocol(session, "ServerHello 서명이 서버 고정 공개 키와 맞지 않음");
		return false;
	}
	++mStat.HelloCnt;
	session.PendingResCnt = 0;
	return session.Registered ? sendLogin(session) : sendRegister(session);
}

bool RsaEchoClient::onRegisterRes(ClientSession& session, const std::string& plain)
{
	RsaRegisterRes res;
	if (plain.size() != sizeof(res))
	{
		failProtocol(session, "가입 응답 길이가 다름");
		return false;
	}
	memcpy(&res, plain.data(), sizeof(res));
	if (res.Type != eRsaMsgType::RegisterRes || (res.Result != eRegisterResult::Ok && res.Result != eRegisterResult::Exists))
	{
		failProtocol(session, "가입 실패");
		return false;
	}
	if (res.Result == eRegisterResult::Ok)
	{
		++mStat.RegisterOkCnt;
	}
	else
	{
		++mStat.RegisterExistsCnt;
	}
	session.Registered = true;
	session.PendingResCnt = 0;
	return sendLogin(session);
}

bool RsaEchoClient::onLoginRes(ClientSession& session, const std::string& plain)
{
	RsaLoginRes res;
	if (plain.size() != sizeof(res))
	{
		failProtocol(session, "로그인 응답 길이가 다름");
		return false;
	}
	memcpy(&res, plain.data(), sizeof(res));
	if (res.Type != eRsaMsgType::LoginRes || res.Result != eAuthResult::Ok)
	{
		failProtocol(session, "로그인 실패");
		return false;
	}
	++mStat.LoginOkCnt;
	session.PendingResCnt = 0;
	session.Step = eSessionStep::Echo;
	startNextBatch(session);
	return session.State != eSessionState::Idle && session.State != eSessionState::Done; // 수명·종료로 닫혔으면 false
}

bool RsaEchoClient::onEchoRes(ClientSession& session, const std::string& plain)
{
	RsaEchoMsg res;
	if (plain.size() != sizeof(res))
	{
		failProtocol(session, "에코 응답 길이가 다름");
		return false;
	}
	memcpy(&res, plain.data(), sizeof(res));
	uint8_t expectData[RSA_ECHO_DATA_LEN];
	fillEchoData(session.ExpectValue, expectData);
	if (res.Type != eRsaMsgType::EchoRes || res.Value != session.ExpectValue || memcmp(res.Data, expectData, sizeof(expectData)) != 0)
	{
		fprintf(stderr, "[RsaEchoClient] 에코 불일치: 세션 %d, 기대 값 %llu, 받은 값 %llu\n", session.Index,
			static_cast<unsigned long long>(session.ExpectValue), static_cast<unsigned long long>(res.Value));
		failProtocol(session, "에코 응답이 보낸 것과 다름");
		return false;
	}
	++mStat.EchoCnt;
	++session.ExpectValue;
	--session.PendingResCnt;
	if (session.PendingResCnt > 0)
	{
		return true;
	}
	++mStat.BatchCnt;
	startNextBatch(session);
	return session.State != eSessionState::Idle && session.State != eSessionState::Done;
}

void RsaEchoClient::onPausedReadable(ClientSession& session)
{
	char buf[64];
	const int received = recv(session.Sock, buf, sizeof(buf), 0);
	if (received == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK)
	{
		return;
	}
	if (received > 0)
	{
		failProtocol(session, "일시정지 중 응답이 더 옴");
		return;
	}
	++mStat.UnexpectedCloseCnt;
	closeConnected(session, true);
}

bool RsaEchoClient::sendRegister(ClientSession& session)
{
	RsaRegisterReq req{};
	req.Type = eRsaMsgType::RegisterReq;
	makeAccountID(session.Index, req.AccountID);
	strcpy_s(req.Password, TEST_PASSWORD);
	session.SendSize = 0;
	session.SentSize = 0;
	if (!sealAndQueue(session, &req, sizeof(req)))
	{
		return false;
	}
	session.Step = eSessionStep::Register;
	session.PendingResCnt = 1;
	session.State = eSessionState::Sending;
	return true;
}

bool RsaEchoClient::sendLogin(ClientSession& session)
{
	RsaLoginReq req{};
	req.Type = eRsaMsgType::LoginReq;
	makeAccountID(session.Index, req.AccountID);
	strcpy_s(req.Password, TEST_PASSWORD);
	session.SendSize = 0;
	session.SentSize = 0;
	if (!sealAndQueue(session, &req, sizeof(req)))
	{
		return false;
	}
	session.Step = eSessionStep::Login;
	session.PendingResCnt = 1;
	session.State = eSessionState::Sending;
	return true;
}

void RsaEchoClient::startNextBatch(ClientSession& session)
{
	if (mbStopRequested)
	{
		closeConnected(session, false);
		return;
	}
	if (mbPaused)
	{
		session.State = eSessionState::Paused;
		return;
	}
	if (session.CloseTick != 0 && GetTickCount64() >= session.CloseTick)
	{
		const bool bRst = (session.ConnectCnt % 2 == 0); // 접속마다 FIN과 RST를 번갈아
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
	session.SendSize = 0;
	session.SentSize = 0;
	for (int32_t i = 0; i < mConfig.EchoBatch; ++i)
	{
		RsaEchoMsg req{};
		req.Type = eRsaMsgType::EchoReq;
		req.Value = session.NextValue;
		fillEchoData(session.NextValue, req.Data);
		++session.NextValue;
		if (!sealAndQueue(session, &req, sizeof(req)))
		{
			return;
		}
	}
	session.PendingResCnt = mConfig.EchoBatch;
	session.State = eSessionState::Sending;
}

void RsaEchoClient::queuePacket(ClientSession& session, const void* payload, const int32_t len)
{
	if (session.SendSize + PACKET_HEADER_SIZE + len > SEND_BUF_SIZE)
	{
		fprintf(stderr, "[RsaEchoClient] SendBuf 넘침 - ECHO_BATCH_MAX 확인\n");
		__debugbreak();
	}
	const PacketHeader header{ static_cast<uint16_t>(len) };
	memcpy(session.SendBuf + session.SendSize, &header, PACKET_HEADER_SIZE);
	memcpy(session.SendBuf + session.SendSize + PACKET_HEADER_SIZE, payload, static_cast<size_t>(len));
	session.SendSize += PACKET_HEADER_SIZE + len;
}

bool RsaEchoClient::sealAndQueue(ClientSession& session, const void* plain, const size_t len)
{
	std::string sealed;
	if (!session.Channel.Seal(plain, len, &sealed))
	{
		failProtocol(session, "요청을 잠그지 못함");
		return false;
	}
	queuePacket(session, sealed.data(), static_cast<int32_t>(sealed.size()));
	return true;
}

void RsaEchoClient::failProtocol(ClientSession& session, const char* reason)
{
	++mStat.ProtocolErrorCnt;
	fprintf(stderr, "[RsaEchoClient] 세션 %d 접속 %u번째: %s\n", session.Index, session.ConnectCnt, reason);
	closeConnected(session, true);
}

void RsaEchoClient::closeConnected(ClientSession& session, const bool bRst)
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
	session.Channel.Clear(); // 클라 임시 키를 지운다
	session.PendingResCnt = 0;
	session.RecvSize = 0;
	--mStat.ConnectedCnt;
	session.State = mbStopRequested ? eSessionState::Done : eSessionState::Idle;
}
