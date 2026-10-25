#include "NetworkLib.h"
#include "ListenSocket.h"
#include "PacketHeader.h"
#include "Profiler.h"
#include "SocketUtil.h"
#include <cstdio>
#include <cstring>

// 수신 완료마다 완성된 패킷을 전부 꺼내므로 수신 링에는 잘린 패킷 1개(최대 패킷 크기 미만)만 남는다
// 링이 최대 패킷보다 크면 수신 링이 가득 차서 WSARecv에 빈 공간을 못 주는 일은 없다
static_assert(RingBuffer::CAPACITY > MAX_PACKET_SIZE, "수신 링은 최대 패킷보다 커야 한다");

static constexpr int PING_TICK_MS = 10;	// 핑 스레드가 깨는 간격 - 핑 간격을 이 단위로 나눠 틱마다 일부 세션에만 보낸다
static thread_local int tWorkerIdx = -1;	// 워커 스레드면 자기 RTT 히스토그램 번호

static int64_t qpcNow()
{
	LARGE_INTEGER t;
	QueryPerformanceCounter(&t);
	return t.QuadPart;
}

WsadataRAII::WsadataRAII()
{
	WSADATA wsa;
	// WSAStartup은 WSAGetLastError가 아니라 반환값으로 에러를 돌려준다
	int err = WSAStartup(MAKEWORD(2, 2), &wsa);
	if (err != 0)
	{
		printf("WSAStartup 실패 err=%d\n", err);
		return;
	}
	mOk = true;
}

WsadataRAII::~WsadataRAII()
{
	if (mOk)
    {
        WSACleanup();
    }
}

// IOCP + 워커 → 세션 풀 → 리슨 소켓(IOCP 등록) → AcceptEx slotCnt개 순서로 시작한다
// 리슨 소켓을 IOCP에 등록하려면 IOCP가 먼저 있어야 한다
bool NetworkLib::start(USHORT port, int backlog, int workerCnt, int slotCnt, int maxSession)
{
	// 실행 중에 다시 부르면 워커가 추가로 뜨고 빈 슬롯 번호가 중복으로 쌓인다 - stop() 후에만 다시 시작할 수 있다
	// 확인과 IOCP 생성 사이에 다른 스레드가 끼지 못하도록 함수 전체를 락으로 감싼다
	std::lock_guard<std::mutex> lock(mStartStopLock);
	if (mIocp != nullptr)
	{
		return false;
	}

	// Session은 mutex를 가져 이동이 안 되므로 resize 대신 만들어서 swap
	std::vector<Session> sessions(maxSession);
	mSessions.swap(sessions);
	mFreeIdx.clear();	// stop() 후 재시작이면 이전 실행의 번호가 남아 있다
	mFreeIdx.reserve(maxSession);
	for (int32_t i = maxSession - 1; i >= 0; --i)
	{
		mSessions[i].mIdx = i;
		mFreeIdx.push_back(i);	// 역순으로 넣어 0번부터 꺼내지게
	}

	// 워커가 뜨기 전에 만든다 - 워커는 자기 번호의 히스토그램에 쓴다
	LARGE_INTEGER freq;
	QueryPerformanceFrequency(&freq);
	mQpcFreq = freq.QuadPart;
	mRttHists.clear();
	for (int i = 0; i < workerCnt; ++i)
	{
		mRttHists.push_back(std::make_unique<LatencyHistogram>());
	}

	if (!initIocp(workerCnt))
	{
		return false;
	}

	mListenSock = createListenSocket(port, backlog);
	if (mListenSock == INVALID_SOCKET)
	{
		return false;
	}

	// 완료 키 0 = 리슨 소켓 - AcceptEx 완료가 이 키로 들어온다
	if (CreateIoCompletionPort(reinterpret_cast<HANDLE>(mListenSock), mIocp, 0, 0) == nullptr)
	{
		printf("리슨 소켓 IOCP 등록 실패 err=%lu\n", GetLastError());
		return false;
	}

	mRunning = true;
	mAcceptOvs.resize(slotCnt);
	for (AcceptOv& ov : mAcceptOvs)
	{
		if (!postAccept(&ov))
		{
			return false;
		}
	}

	if (mPingIntervalMs > 0)
	{
		mPingThread = std::thread(&NetworkLib::pingLoop, this);
	}
	return true;
}

void NetworkLib::setPing(int intervalMs, int sampleMod)
{
	mPingIntervalMs = intervalMs;
	mPingSampleMod = (sampleMod < 1) ? 1 : sampleMod;
}

// 워커별 히스토그램을 합친다 - 각 칸은 조금 늦은 값일 수 있지만 1초 통계용으로는 충분
std::vector<uint64_t> NetworkLib::getRttCounts() const
{
	std::vector<uint64_t> counts(LatencyHistogram::BUCKET_COUNT, 0);
	for (const std::unique_ptr<LatencyHistogram>& hist : mRttHists)
	{
		hist->addTo(counts);
	}
	return counts;
}

// 안전망: 종료 절차를 건너뛰어도 join 안 된 std::thread 소멸로 terminate 되지 않게
// 파생 클래스가 소멸자에서 stop()을 이미 불렀어야 한다 - 여기서 세션이 남아 있으면 OnRelease가 순수 가상 호출이 된다
NetworkLib::~NetworkLib()
{
	stop();
}

// 종료: AcceptEx 중단 → 세션 전부 끊기 → 걸린 I/O가 모두 끝날 때까지 대기 → 워커 종료
// socket() 실패로 mRunning이 이미 내려가 있어도 불리므로 mRunning이 아니라 mIocp로 중복 호출을 막는다
void NetworkLib::stop()
{
	// start와 같은 락 - 시작 도중에 끼어들거나 stop 두 개가 동시에 정리하지 않게
	std::lock_guard<std::mutex> lock(mStartStopLock);
	if (mIocp == nullptr)
	{
		return;	// 시작 안 했거나 이미 종료됨
	}

	mRunning = false;	// 이후 AcceptEx를 다시 걸지 않는다. 핑 스레드도 다음 틱(최대 10ms)에 빠져나온다
	closesocket(mListenSock);	// 걸려 있는 AcceptEx가 전부 실패로 완료된다
	mListenSock = INVALID_SOCKET;
	if (mPingThread.joinable())
	{
		mPingThread.join();	// 세션을 정리하는 동안 새 핑이 끼어들지 않게 먼저 멈춘다
	}

	// 살아 있는 세션의 I/O를 취소하고, 워커가 남은 완료를 처리해 모두 정리될 때까지 기다린다
	// 취소를 대기 루프 안에서 반복하는 이유: 종료 직전에 수락된 세션이 첫 수신을 걸기 전이면 이번 순회에서 놓친다
	while (true)
	{
		for (Session& session : mSessions)
		{
			if (acquireSession(&session))
			{
				requestDisconnect(&session);	// 새 I/O를 막고 걸린 I/O를 취소 → 실패 완료 → 카운트 0 → 해제
				decIoCount(&session);
			}
		}

		bool allFree = false;
		{
			std::lock_guard<std::mutex> lock(mFreeLock);
			allFree = (mFreeIdx.size() == mSessions.size());
		}
		if (allFree && mPendingAccept == 0)
		{
			break;
		}
		Sleep(10);
	}

	stopWorkers();
}

bool NetworkLib::initIocp(int workerCnt)
{
	// 마지막 인자 0 = 동시 실행 스레드 수를 코어 수로
	mIocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);
	if (mIocp == nullptr)
	{
		printf("CreateIoCompletionPort 실패 err=%lu\n", GetLastError());
		return false;
	}
	
	for (int i = 0; i < workerCnt; ++i)
	{
		mWorkers.emplace_back(&NetworkLib::workerLoop, this, i);
	}
	return true;
}

void NetworkLib::stopWorkers()
{
	if (mIocp == nullptr)
	{
		return;
	}

	// 종료 신호 = 키 0 + overlapped nullptr. 워커 하나가 하나씩 가져간다
	for (size_t i = 0; i < mWorkers.size(); ++i)
	{
		PostQueuedCompletionStatus(mIocp, 0, 0, nullptr);
	}
	for (std::thread& t : mWorkers)
	{
		t.join();
	}
	mWorkers.clear();

	CloseHandle(mIocp);
	mIocp = nullptr;
}

// AcceptEx 등록이 실패하면 슬롯을 버리지 않고 성공할 때까지 재시도 - 걸려 있는 AcceptEx 수를 고정으로 유지
// false: 종료 중이거나 socket() 생성 실패(치명적 - 종료 요청을 보냄)
bool NetworkLib::postAccept(AcceptOv* ov)
{
	while (mRunning)
	{
		SOCKET acceptSocket = INVALID_SOCKET;
		{
			ProfileScope scope("AcceptEx_Socket");
			acceptSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		}

		if (acceptSocket == INVALID_SOCKET)
		{
			// LOG : Socket 생성에 실패한 상황
			mRunning = false;	// 종료 요청 - main이 보고 stop()을 부른다
			return false;
		}

		ZeroMemory(static_cast<OVERLAPPED*>(ov), sizeof(OVERLAPPED));	// 재사용 전 OVERLAPPED 필드 초기화
		ov->type = eIoType::Accept;
		ov->mAcceptSocket = acceptSocket;

		// 에러 코드는 스코프 안에서 읽음 (소멸자의 기록 코드가 last error를 덮어쓸 가능성 배제)
		DWORD bytesReceived = 0;
		BOOL ok = FALSE;
		int err = 0;
		++mPendingAccept;	// AcceptEx 전에 올린다 - 완료가 먼저 처리돼 -1이 앞서는 것을 막는다
		{
			ProfileScope scope("AcceptEx_Post");
			ok = AcceptEx(mListenSock, acceptSocket, ov->mBuffer, 0,
				sizeof(SOCKADDR_IN) + 16, sizeof(SOCKADDR_IN) + 16, &bytesReceived, ov);
			if (!ok)
			{
				err = WSAGetLastError();
			}
		}

		if (ok || err == WSA_IO_PENDING)
		{
			return true;
		}
		--mPendingAccept;	// 등록 실패 - 완료 통지가 오지 않는다
		closesocket(acceptSocket);
	}
	return false;
}

// AcceptEx 완료: 같은 슬롯에 AcceptEx를 먼저 다시 걸고, 받은 소켓으로 세션을 만들어 첫 수신을 건다
void NetworkLib::onAcceptComplete(AcceptOv* ov, BOOL ok)
{
	// 재등록하면 다음 연결이 같은 버퍼에 주소를 덮어쓰므로, 소켓과 주소를 먼저 꺼내 둔다
	SOCKET sock = ov->mAcceptSocket;
	uint32_t ip = 0;
	uint16_t port = 0;
	if (ok)
	{
		SOCKADDR* localAddr = nullptr;
		SOCKADDR* remoteAddr = nullptr;
		int localLen = 0;
		int remoteLen = 0;
		// 0, sizeof(SOCKADDR_IN) + 16 두 개는 AcceptEx를 걸 때 넘긴 값과 같아야 한다
		GetAcceptExSockaddrs(ov->mBuffer, 0, sizeof(SOCKADDR_IN) + 16, sizeof(SOCKADDR_IN) + 16,
			&localAddr, &localLen, &remoteAddr, &remoteLen);
		SOCKADDR_IN* remote = reinterpret_cast<SOCKADDR_IN*>(remoteAddr);
		ip = remote->sin_addr.s_addr;
		port = ntohs(remote->sin_port);
	}
	postAccept(ov);	// 슬롯이 비어 있는 시간을 줄인다 - 비면 연결이 백로그로 간다(측정 결과)
	--mPendingAccept;	// 끝난 AcceptEx 몫

	if (!ok)
	{
		closesocket(sock);	// 수락 도중 상대가 끊었거나 리슨 소켓이 닫힘(종료 중)
		return;
	}

	// AcceptEx로 받은 소켓은 리슨 소켓의 속성을 물려받지 않은 상태 - 이걸 해야 getpeername/shutdown이 동작한다
	if (setsockopt(sock, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT,
		reinterpret_cast<char*>(&mListenSock), sizeof(mListenSock)) == SOCKET_ERROR)
	{
		// LOG : SO_UPDATE_ACCEPT_CONTEXT 실패 (종료 중 리슨 소켓이 닫힌 경우 포함)
		closeReset(sock);
		return;
	}

	Session* session = allocSession();
	if (session == nullptr)
	{
		++mSessionFullCnt;
		closeReset(sock);	// 세션 슬롯이 가득 참 - 받지 않고 끊는다
		return;
	}

	// 완료 키 = Session* - 이 소켓의 완료 통지가 오면 어느 세션인지 바로 안다
	if (CreateIoCompletionPort(reinterpret_cast<HANDLE>(sock), mIocp, reinterpret_cast<ULONG_PTR>(session), 0) == nullptr)
	{
		// LOG : 세션 소켓 IOCP 등록 실패
		closeReset(sock);
		freeSession(session);
		return;
	}

	session->mSock = sock;
	session->mIp = ip;
	session->mPort = port;
	++mAcceptCnt;

	// 수락 처리 몫으로 카운트를 잡는다 - 0인 채로 OnAccept를 부르면 그 안의 SendPacket이 findSession에서 실패하고,
	// 첫 수신이 바로 실패해 해제되면 OnAccept보다 OnRelease가 먼저 불릴 수 있다
	++session->mIoCount;
	OnAccept(session->mId);
	postRecv(session);	// OnAccept 뒤에 건다 - 이 세션의 OnRecv가 OnAccept보다 먼저 불리지 않게
	decIoCount(session);	// 수락 처리 몫
}

// 수신 링의 빈 공간에 수신 1건을 건다 (끝에서 감기면 WSABUF 2개). 즉시 실패하면 올려둔 I/O 카운트를 되돌린다
// 빈 공간이 0인 경우는 없다 - 파일 맨 위 static_assert 참고
void NetworkLib::postRecv(Session* session)
{
	if (session->mDisconnect)
	{
		return;	// 끊는 중 - 다시 걸지 않으면 호출한 쪽의 카운트 반납으로 해제된다
	}

	ZeroMemory(static_cast<OVERLAPPED*>(&session->mRecvOv), sizeof(OVERLAPPED));
	session->mRecvOv.type = eIoType::Recv;

	WSABUF bufs[2];
	int32_t bufCnt = session->mRecvBuf.getWriteBufs(bufs);
	DWORD flags = 0;

	++session->mIoCount;	// WSARecv 전에 올린다 - 완료가 먼저 처리돼 0으로 떨어지는 것을 막는다
	if (WSARecv(session->mSock, bufs, bufCnt, nullptr, &flags, &session->mRecvOv, nullptr) == SOCKET_ERROR)
	{
		int err = WSAGetLastError();
		if (err != WSA_IO_PENDING)
		{
			// 상대가 이미 끊었거나 소켓 오류 - 완료 통지가 오지 않으므로 직접 되돌린다
			decIoCount(session);
			return;
		}
	}

	// 위의 확인과 WSARecv 사이에 끊기 요청이 오면, 그 CancelIoEx는 아직 안 걸린 이 수신을 못 잡는다 - 걸고 나서 다시 확인
	if (session->mDisconnect)
	{
		CancelIoEx(reinterpret_cast<HANDLE>(session->mSock), &session->mRecvOv);
	}
}

// 수신 완료: 0바이트나 실패면 상대가 끊은 것 - 다시 걸지 않는다
// 받았으면 완성된 패킷을 하나씩 OnRecv로 넘기고 다음 수신을 건다. 마지막에 잘린 패킷은 링에 남아 다음 수신과 이어진다
void NetworkLib::onRecvComplete(Session* session, BOOL ok, DWORD transferred)
{
	if (!ok || transferred == 0)
	{
		decIoCount(session);	// 수신 몫을 내려놓는다 - 송신이 없으면 여기서 해제
		return;
	}

	// 퐁의 도착 시각 - 완료를 꺼낸 직후 한 번만 찍는다. 같은 완료 안의 앞 패킷들을 처리한 시간이 RTT에 섞이지 않게
	int64_t recvQpc = qpcNow();

	// WSARecv가 링의 빈 공간에 직접 받았으므로 쓰기 위치만 옮긴다
	session->mRecvBuf.moveWrite(static_cast<int32_t>(transferred));

	char packet[MAX_PACKET_SIZE];
	while (!session->mDisconnect)	// OnRecv 안에서 Disconnect했거나 SendPacket이 넘쳐 끊었으면 남은 패킷은 버린다
	{
		PacketHeader header;
		if (!session->mRecvBuf.peek(reinterpret_cast<char*>(&header), PACKET_HEADER_SIZE))
		{
			break;	// 헤더도 다 오지 않음
		}
		if (header.len > MAX_PAYLOAD_SIZE || static_cast<uint8_t>(header.kind) > static_cast<uint8_t>(ePacketKind::Pong))
		{
			// 정상 클라이언트는 보낼 수 없는 헤더 - 패킷 경계를 잃었으므로 이후 바이트도 믿을 수 없다
			++mBadPacketCnt;
			requestDisconnect(session);
			break;
		}

		int32_t packetSize = PACKET_HEADER_SIZE + header.len;
		if (!session->mRecvBuf.peek(packet, packetSize))
		{
			break;	// 페이로드가 덜 옴
		}
		session->mRecvBuf.pop(packetSize);

		const char* payload = packet + PACKET_HEADER_SIZE;
		switch (header.kind)
		{
		case ePacketKind::Data:
			OnRecv(session->mId, payload, header.len);
			break;
		case ePacketKind::Ping:
			sendRaw(session, ePacketKind::Pong, payload, header.len);	// 상대가 자기 시계로 잰다 - 그대로 돌려준다
			break;
		case ePacketKind::Pong:
			onPong(session, payload, header.len, recvQpc);
			break;
		}
	}

	postRecv(session);	// 끊는 중이면 걸지 않는다
	decIoCount(session);	// 끝난 수신 몫
}

// 컨텐츠 패킷 송신. 아무 스레드에서나 부를 수 있다 (OnRecv 안 포함)
bool NetworkLib::SendPacket(uint64_t id, const char* payload, int32_t len)
{
	Session* session = findSession(id);
	if (session == nullptr)
	{
		return false;	// 이미 해제됨 (또는 슬롯이 다른 세션에 재사용됨)
	}
	bool sent = sendRaw(session, ePacketKind::Data, payload, len);
	decIoCount(session);	// findSession 몫
	return sent;
}

// 헤더를 붙여 송신 링에 넣고 송신을 건다 - 컨텐츠 패킷과 라이브러리 제어 패킷(핑/퐁)이 같은 경로를 탄다
bool NetworkLib::sendRaw(Session* session, ePacketKind kind, const char* payload, int32_t len)
{
	if (len < 0 || len > MAX_PAYLOAD_SIZE)
	{
		return false;	// 받는 쪽이 비정상 패킷으로 보고 끊을 길이
	}
	if (session->mDisconnect)
	{
		return false;
	}

	PacketHeader header;
	header.len = static_cast<uint16_t>(len);
	header.kind = kind;
	bool pushed = false;
	{
		std::lock_guard<std::mutex> lock(session->mSendLock);
		// 헤더와 페이로드 중 하나만 들어가면 스트림이 깨지므로 둘 다 들어갈 때만 넣는다
		if (session->mSendBuf.getFreeSize() >= PACKET_HEADER_SIZE + len)
		{
			session->mSendBuf.push(reinterpret_cast<const char*>(&header), PACKET_HEADER_SIZE);
			session->mSendBuf.push(payload, len);
			pushed = true;
		}
	}

	if (!pushed)
	{
		if (kind != ePacketKind::Data)
		{
			return false;	// 측정용 제어 패킷(핑/퐁) 때문에 연결을 끊지는 않는다 - 이번 것만 버린다
		}
		// 송신 링이 가득 참 - 상대가 받는 속도보다 빨리 쌓였다
		++mOverflowCnt;
		requestDisconnect(session);
	}
	else
	{
		postSend(session);
	}
	return pushed;
}

// 끊기 요청 - 실제 해제와 OnRelease는 걸린 I/O가 모두 끝난 뒤 워커에서 일어난다
void NetworkLib::Disconnect(uint64_t id)
{
	Session* session = findSession(id);
	if (session == nullptr)
	{
		return;
	}
	requestDisconnect(session);
	decIoCount(session);	// findSession 몫
}

// 끊기 플래그를 세우고 걸린 I/O를 취소한다 - 실패 완료가 오면 다시 걸지 않으므로 카운트가 0으로 내려간다
// 걸려 있는 송신은 상대가 안 받으면 끝나지 않으므로 취소해서 완료를 앞당긴다
// 호출하는 쪽이 카운트를 잡고 있어야 mSock이 유효하다
void NetworkLib::requestDisconnect(Session* session)
{
	session->mDisconnect = true;
	CancelIoEx(reinterpret_cast<HANDLE>(session->mSock), nullptr);
}

// 쌓인 데이터를 송신 1건으로 건다. 이미 송신 중이거나 보낼 게 없으면 아무것도 하지 않는다
// mSendFlag는 mSendLock 안에서만 읽고 쓴다 - 링이 비었는지 확인과 플래그 변경이 한 번에 일어나야 데이터가 남지 않는다
void NetworkLib::postSend(Session* session)
{
	if (session->mDisconnect)
	{
		return;
	}

	WSABUF bufs[2];
	int32_t bufCnt = 0;
	{
		std::lock_guard<std::mutex> lock(session->mSendLock);
		if (session->mSendFlag)
		{
			return;	// 송신 중 - 완료 쪽이 이어서 보낸다
		}
		bufCnt = session->mSendBuf.getReadBufs(bufs);
		if (bufCnt == 0)
		{
			return;
		}
		session->mSendFlag = true;
	}

	ZeroMemory(static_cast<OVERLAPPED*>(&session->mSendOv), sizeof(OVERLAPPED));
	session->mSendOv.type = eIoType::Send;

	++session->mIoCount;	// WSASend 전에 올린다 - postRecv와 같은 이유
	if (WSASend(session->mSock, bufs, bufCnt, nullptr, 0, &session->mSendOv, nullptr) == SOCKET_ERROR)
	{
		int err = WSAGetLastError();
		if (err != WSA_IO_PENDING)
		{
			// 끊긴 소켓 - 완료 통지가 오지 않으므로 직접 되돌린다
			// mSendFlag는 true로 남겨 더 보내지 않는다 (해제 시 false로 돌아감)
			decIoCount(session);
			return;
		}
	}

	// postRecv와 같은 이유 - 상대가 안 받는 중에 끊기 요청을 놓치면 이 송신이 끝나지 않아 해제되지 않는다
	if (session->mDisconnect)
	{
		CancelIoEx(reinterpret_cast<HANDLE>(session->mSock), &session->mSendOv);
	}
}

// 송신 완료: 보낸 만큼 링에서 빼고 플래그를 내린 뒤, 송신 중에 쌓인 데이터가 있으면 이어서 보낸다
// 실패(끊김, 취소)면 플래그를 내리지 않아 더 보내지 않는다 - 해제 시 false로 돌아감
void NetworkLib::onSendComplete(Session* session, BOOL ok, DWORD transferred)
{
	if (!ok)
	{
		decIoCount(session);
		return;
	}

	{
		std::lock_guard<std::mutex> lock(session->mSendLock);
		session->mSendBuf.pop(static_cast<int32_t>(transferred));
		session->mSendFlag = false;
	}
	postSend(session);	// 송신 중에 쌓인 데이터
	decIoCount(session);	// 끝난 송신 몫
}

// I/O 카운트 -1. 0이 되면 이 세션을 건드리는 I/O도 스레드도 없으므로 해제한다
// 해제는 IOCP에 작업으로 넣어 항상 워커가 한다 - SendPacket/Disconnect를 부른 컨텐츠 스레드에서 0이 되면
// 그 자리에서 OnRelease가 재진입해, 컨텐츠가 잡고 있던 락을 다시 잡으려다 데드락이 날 수 있다
void NetworkLib::decIoCount(Session* session)
{
	if (--session->mIoCount != 0)
	{
		return;
	}

	ZeroMemory(static_cast<OVERLAPPED*>(&session->mReleaseOv), sizeof(OVERLAPPED));
	session->mReleaseOv.type = eIoType::Release;
	if (!PostQueuedCompletionStatus(mIocp, 0, reinterpret_cast<ULONG_PTR>(session), &session->mReleaseOv))
	{
		// LOG : PQCS 실패 - 슬롯이 영영 안 돌아오는 것보다는 이 자리에서 해제하는 편이 낫다
		releaseSession(session);
	}
}

// 소켓을 RST로 닫고, 컨텐츠에 알리고, 보내지 못한 데이터와 덜 받은 패킷을 버리고, 슬롯을 돌려준다
// OnRelease는 슬롯을 돌려주기 전에 부른다 - 콜백 도중 같은 슬롯이 다른 연결에 재사용되지 않게
void NetworkLib::releaseSession(Session* session)
{
	closeReset(session->mSock);
	session->mSock = INVALID_SOCKET;
	OnRelease(session->mId);

	session->mIp = 0;
	session->mPort = 0;
	session->mDisconnect = false;
	session->mSendFlag = false;
	session->mSendBuf.clear();
	session->mRecvBuf.clear();
	session->mPingSeq = 0;	// 카운트 0이라 핑 스레드가 이 슬롯을 잡을 수 없다 - 안전하게 초기화
	session->mPongSeq = 0;
	++mReleaseCnt;
	freeSession(session);
}

// 각 카운터를 따로 읽으므로 네 값이 같은 순간의 값은 아니다 - 1초 통계용으로는 충분
// release를 accept보다 먼저 읽어 현재 세션 수(accept - release)가 음수로 나오지 않게 한다
NetStat NetworkLib::getStat() const
{
	NetStat stat;
	stat.releaseCnt = mReleaseCnt.load();
	stat.acceptCnt = mAcceptCnt.load();
	stat.overflowCnt = mOverflowCnt.load();
	stat.sessionFullCnt = mSessionFullCnt.load();
	stat.badPacketCnt = mBadPacketCnt.load();
	stat.pingSentCnt = mPingSentCnt.load();
	stat.pingSkipCnt = mPingSkipCnt.load();
	return stat;
}

// 핑 전용 타이머 스레드 - 워커와 독립이라 워커가 멈춰도 핑이 예정대로 나가고, 퐁이 IOCP에서 기다린 시간이 RTT에 잡힌다
// 틱(10ms)마다 세션의 1/K만 보내 순간 부하를 나눈다 (K = 핑 간격 / 틱). 각 세션은 핑 간격마다 1번씩 받는다
void NetworkLib::pingLoop()
{
	// 고해상도 타이머가 없으면(Windows 10 1803 이전) 일반 타이머 - 약 15.6ms 단위로 깨지만 절대 시각 기준이라 평균 간격은 지켜진다
	HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
	if (timer == nullptr)
	{
		timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
	}
	if (timer == nullptr)
	{
		printf("핑 타이머 생성 실패 err=%lu - RTT 측정 안 함\n", GetLastError());
		return;
	}

	const int sliceCnt = (mPingIntervalMs / PING_TICK_MS > 1) ? mPingIntervalMs / PING_TICK_MS : 1;
	const int64_t tickQpc = mQpcFreq * PING_TICK_MS / 1000;
	const int64_t startQpc = qpcNow();
	uint64_t tick = 0;	// 처리한 틱 수 - n번째 틱은 startQpc + n * tickQpc 에 처리돼야 한다

	while (mRunning)
	{
		// 지금까지 처리됐어야 할 틱 수 - 늦게 깼으면 놓친 틱까지 처리해 각 세션의 핑 간격을 지킨다
		uint64_t due = static_cast<uint64_t>((qpcNow() - startQpc) / tickQpc) + 1;
		if (due - tick > static_cast<uint64_t>(sliceCnt))
		{
			tick = due - sliceCnt;	// 한 바퀴 넘게 밀렸으면(디버거 정지 등) 한 바퀴만 따라잡는다
		}
		while (tick < due)
		{
			sendPingSlice(static_cast<int>(tick % sliceCnt), sliceCnt);
			++tick;
		}

		// 다음 틱 시각까지 잔다 - 절대 시각 기준이라 한 번 늦게 깨도 이후 일정이 밀리지 않는다
		int64_t waitQpc = startQpc + static_cast<int64_t>(tick) * tickQpc - qpcNow();
		if (waitQpc > 0)
		{
			LARGE_INTEGER dueTime;
			dueTime.QuadPart = -(waitQpc * 10'000'000 / mQpcFreq);	// 음수 = 지금부터의 상대 시간, 100ns 단위
			SetWaitableTimer(timer, &dueTime, 0, nullptr, nullptr, FALSE);
			WaitForSingleObject(timer, INFINITE);
		}
	}
	CloseHandle(timer);
}

// 표본 슬롯(0, S, 2S, ...)을 다시 sliceCnt개 조각으로 나눠 slice번 조각에만 핑을 보낸다 (S = 표본 비율)
// "슬롯 % S == 0"으로 뽑은 뒤 그대로 "% sliceCnt"로 나누면 S와 sliceCnt가 공약수를 가질 때 표본이 한 조각에 몰린다
void NetworkLib::sendPingSlice(int slice, int sliceCnt)
{
	const size_t step = static_cast<size_t>(sliceCnt) * mPingSampleMod;
	for (size_t idx = static_cast<size_t>(slice) * mPingSampleMod; idx < mSessions.size(); idx += step)
	{
		Session* session = &mSessions[idx];
		if (!acquireSession(session))
		{
			continue;	// 빈 슬롯은 카운트 0이라 여기서 바로 걸러진다
		}

		PingPayload ping;
		ping.seq = session->mPingSeq.load() + 1;
		session->mPingSeq = ping.seq;	// 보내기 전에 올린다 - 퐁이 아주 빨리 와도 인정 범위 안에 들도록
		ping.t0 = qpcNow();	// SendPacket 시점 - 송신 링에서 앞 데이터 뒤에 기다리는 시간도 RTT에 들어간다(체감 지연)
		if (sendRaw(session, ePacketKind::Ping, reinterpret_cast<const char*>(&ping), sizeof(ping)))
		{
			++mPingSentCnt;
		}
		else
		{
			++mPingSkipCnt;	// 송신 링이 가득 찼거나 끊는 중 - 많으면 가장 막힌 세션의 표본이 빠지고 있다는 뜻
		}
		decIoCount(session);
	}
}

// 퐁 처리: 인정 범위 = (마지막으로 인정한 순번, 마지막으로 보낸 순번]
// 최신 핑의 퐁만 받으면, RTT가 핑 간격보다 길 때 퐁이 올 때마다 이미 다음 핑이 나가 있어 느릴 때의 표본이 전부 버려진다
void NetworkLib::onPong(Session* session, const char* payload, int32_t len, int64_t recvQpc)
{
	if (len != sizeof(PingPayload))
	{
		++mBadPacketCnt;
		requestDisconnect(session);	// 받은 핑을 그대로 돌려주지 않는 상대
		return;
	}

	PingPayload ping;
	memcpy(&ping, payload, sizeof(ping));
	if (ping.seq <= session->mPongSeq || ping.seq > session->mPingSeq.load())
	{
		return;	// 이미 센 퐁이거나 보낸 적 없는 순번 - 버린다
	}
	session->mPongSeq = ping.seq;

	int64_t us = (recvQpc - ping.t0) * 1'000'000 / mQpcFreq;
	mRttHists[tWorkerIdx]->record(us > 0 ? static_cast<uint64_t>(us) : 0);
}

// 빈 세션 슬롯 하나를 꺼내고 세대를 올려 새 ID를 붙인다. 가득 차면 nullptr
// 꺼낸 시점의 I/O 카운트는 0이라 acquireSession이 실패하므로, 여기서 쓴 mId를 동시에 읽는 스레드는 없다
Session* NetworkLib::allocSession()
{
	std::lock_guard<std::mutex> lock(mFreeLock);
	if (mFreeIdx.empty())
	{
		return nullptr;
	}
	int32_t idx = mFreeIdx.back();
	mFreeIdx.pop_back();

	Session* session = &mSessions[idx];
	uint32_t gen = static_cast<uint32_t>(session->mId >> 32) + 1;
	session->mId = (static_cast<uint64_t>(gen) << 32) | static_cast<uint32_t>(idx);
	return session;
}

// 세션 슬롯을 빈 슬롯 스택에 돌려준다
void NetworkLib::freeSession(Session* session)
{
	std::lock_guard<std::mutex> lock(mFreeLock);
	mFreeIdx.push_back(session->mIdx);
}

// 세션이 살아 있을 때(카운트 > 0)만 I/O 카운트를 +1 한다
// 성공하면 내가 -1 하기 전까지 해제되지 않으므로 mSock을 안전하게 쓸 수 있다
bool NetworkLib::acquireSession(Session* session)
{
	int32_t count = session->mIoCount.load();
	while (count > 0)
	{
		if (session->mIoCount.compare_exchange_weak(count, count + 1))
		{
			return true;
		}
	}
	return false;	// 0 = 비어 있거나 해제 중
}

// 컨텐츠는 Session*가 아니라 ID를 들고 있다가 이 함수로 세션을 얻는다 (SendPacket/Disconnect용)
// 슬롯 번호만으로 찾으면, 해제 후 같은 슬롯에 들어온 다른 클라이언트를 잡을 수 있다 - 그래서 acquire 후 ID까지 비교
// acquire에 성공하면 해제·재할당이 막히므로 그 뒤에 읽는 mId는 바뀌지 않는다
Session* NetworkLib::findSession(uint64_t id)
{
	uint32_t idx = static_cast<uint32_t>(id);
	if (idx >= mSessions.size())
	{
		return nullptr;
	}

	Session* session = &mSessions[idx];
	if (!acquireSession(session))
	{
		return nullptr;
	}
	if (session->mId != id)
	{
		decIoCount(session);	// 같은 슬롯의 다른 세션 - 잡은 카운트만 되돌린다
		return nullptr;
	}
	return session;
}

void NetworkLib::workerLoop(int workerIdx)
{
	tWorkerIdx = workerIdx;
	while (true)
	{
		DWORD transferred = 0;
		ULONG_PTR key = 0;
		OVERLAPPED* overlapped = nullptr;
		BOOL ok = FALSE;
		DWORD err = 0;

		// 완료 대기 시간 포함 - 길면 워커가 일감을 기다리며 놀고 있다는 뜻
		// 에러 코드는 스코프 안에서 읽음 (소멸자의 기록 코드가 last error를 덮어쓸 가능성 배제)
		{
			ProfileScope scope("Worker_GQCS");
			ok = GetQueuedCompletionStatus(mIocp, &transferred, &key, &overlapped, INFINITE);
			if (!ok)
			{
				err = GetLastError();
			}
		}

		if (overlapped == nullptr)
		{
			// ok == TRUE면 stopWorkers의 종료 신호, FALSE면 IOCP 자체 오류
			if (!ok)
			{
				printf("GQCS 실패 err=%lu\n", err);
			}
			break;
		}

		// 완료 1건 처리 전체, 루프 한 바퀴 끝에서 기록됨
		ProfileScope completeScope("Worker_Complete");

		// ok == FALSE이고 overlapped != nullptr 이면 그 I/O가 실패한 것 (연결 끊김, 취소 등) - 각 완료 처리가 판단한다
		MyOverlapped* ov = static_cast<MyOverlapped*>(overlapped);
		Session* session = reinterpret_cast<Session*>(key);	// 리슨 소켓(Accept)은 키 0이라 nullptr
		switch (ov->type)
		{
		case eIoType::Accept:
			onAcceptComplete(static_cast<AcceptOv*>(ov), ok);
			break;
		case eIoType::Recv:
			onRecvComplete(session, ok, transferred);
			break;
		case eIoType::Send:
			onSendComplete(session, ok, transferred);
			break;
		case eIoType::Release:
			releaseSession(session);
			break;
		}
	}
}
