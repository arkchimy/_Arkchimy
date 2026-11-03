#include <cstdio>
#include "NetworkLib.h"
#include "Assert.h"
#include "ListenSocket.h"
#include "Memory.h"
#include "PacketHeader.h"
#include "Profiler.h"
#include "SocketUtil.h"

// 수신 완료마다 완성된 패킷을 전부 꺼내므로 수신 링에는 잘린 패킷 1개(최대 패킷 크기 미만)만 남는다
// 링이 최대 패킷보다 크면 수신 링이 가득 차서 WSARecv에 빈 공간을 못 주는 일은 없다
static_assert(RECV_RING_SIZE > MAX_PACKET_SIZE, "수신 링은 최대 패킷보다 커야 한다");
static_assert(SEND_RING_SIZE >= MAX_PACKET_SIZE, "송신 링에는 최대 패킷이 하나는 들어가야 한다 - 아니면 큰 패킷을 보내는 즉시 끊긴다");

static thread_local int tWorkerIdx = -1;	// 워커 스레드면 자기 번호(워커별 카운터) - 워커가 아니면 -1
static thread_local bool tbDeferSend = false;	// 이 스레드가 지금 수신 처리 중 - 이 동안의 송신은 받는 사람이 누구든 링에 쌓기만 하고 수신 처리 끝에서 한 번에 보낸다
static thread_local std::vector<Session*> tSendTargets;	// 이번 수신 처리에서 보낼 게 생긴 세션 - 각자 카운트를 하나씩 잡고 있다. 워커 시작 때 maxSession만큼 reserve(재할당 없음)

static int64_t getNowQPC()
{
	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);
	return now.QuadPart;
}

WsadataRAII::WsadataRAII()
{
	WSADATA wsa;
	// WSAStartup은 WSAGetLastError가 아니라 반환값으로 에러를 돌려준다
	const int err = WSAStartup(MAKEWORD(2, 2), &wsa);
	if (err != 0)
	{
		printf("WSAStartup 실패 err=%d\n", err);
		return;
	}
	mbOk = true;
}

WsadataRAII::~WsadataRAII()
{
	if (mbOk)
	{
		WSACleanup();
	}
}

// IOCP + 워커 → 세션 풀 → 리슨 소켓(IOCP 등록) → AcceptEx slotCnt개 순서로 시작한다
// 리슨 소켓을 IOCP에 등록하려면 IOCP가 먼저 있어야 한다
bool NetworkLib::Start(const USHORT port, const int backlog, const int workerCnt, const int slotCnt, const int maxSession)
{
	// 실행 중에 다시 부르면 워커가 추가로 뜨고 빈 슬롯 번호가 중복으로 쌓인다 - Stop() 후에만 다시 시작할 수 있다
	// 확인과 IOCP 생성 사이에 다른 스레드가 끼지 못하도록 함수 전체를 락으로 감싼다
	std::lock_guard<std::mutex> startStopLock(mStartStopLock);
	if (mIOCP != nullptr)
	{
		return false;
	}

	// 즉시 완료 처리 경로는 모든 세션에 같은 설정이 걸렸다고 가정한다 - 걸 수 없는 환경이면 시작 전에 끈다
	if (mbSkipOnSuccess && !IsIfsTcpSocket())
	{
		printf("IFS가 아닌 LSP가 있어 skipOnSuccess를 끈다\n");
		mbSkipOnSuccess = false;
	}

	// Session은 mutex를 가져 이동이 안 되므로 resize 대신 만들어서 swap
	std::vector<Session> sessions(maxSession);
	mSessions.swap(sessions);
	mFreeIdx.clear();	// Stop() 후 재시작이면 이전 실행의 번호가 남아 있다
	mFreeIdx.reserve(maxSession);
	for (int32_t i = maxSession - 1; i >= 0; --i)
	{
		Session& session = mSessions[i];
		session.mIdx = i;
		mFreeIdx.push_back(i);	// 역순으로 넣어 0번부터 꺼내지게
	}

	// 워커가 뜨기 전에 만든다 - 워커는 자기 번호의 카운터에 쓴다
	mWorkerCounters.clear();
	for (int i = 0; i < workerCnt; ++i)
	{
		mWorkerCounters.push_back(MakeUnique<WorkerCounter>());
	}

	if (!initIOCP(workerCnt))
	{
		return false;
	}

	mListenSock = CreateListenSocket(port, backlog);
	if (mListenSock == INVALID_SOCKET)
	{
		return false;
	}

	// 완료 키 0 = 리슨 소켓 - AcceptEx 완료가 이 키로 들어온다
	if (CreateIoCompletionPort(reinterpret_cast<HANDLE>(mListenSock), mIOCP, 0, 0) == nullptr)
	{
		printf("리슨 소켓 IOCP 등록 실패 err=%lu\n", GetLastError());
		return false;
	}

	mbRunning = true;
	mAcceptOvs.resize(slotCnt);
	for (AcceptOv& ov : mAcceptOvs)
	{
		if (!postAccept(ov))
		{
			return false;
		}
	}

	// 송신 정체 감시 - 세션 배열이 준비된 뒤 시작한다. Start가 앞에서 실패하면 띄우지 않으므로 Stop의 join 대상도 없다
	if (mSendStallSec > 0)
	{
		LARGE_INTEGER freq;
		QueryPerformanceFrequency(&freq);
		mQpcFreq = freq.QuadPart;
		mWatchStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);	// 수동 리셋, 처음엔 꺼짐 - Stop이 켜면 감시가 바로 깨어나 끝난다
		if (mWatchStopEvent == nullptr)
		{
			printf("감시 스레드 이벤트 생성 실패 err=%lu\n", GetLastError());
			return false;
		}
		mWatchThread = std::thread(&NetworkLib::runSendStallWatch, this);
	}

	return true;
}

// 안전망: 종료 절차를 건너뛰어도 join 안 된 std::thread 소멸로 terminate 되지 않게
// 파생 클래스가 소멸자에서 Stop()을 이미 불렀어야 한다 - 여기서 세션이 남아 있으면 OnRelease가 순수 가상 호출이 된다
NetworkLib::~NetworkLib()
{
	Stop();
}

// 종료: AcceptEx 중단 → 세션 전부 끊기 → 걸린 I/O가 모두 끝날 때까지 대기 → 워커 종료
// socket() 실패로 mbRunning이 이미 내려가 있어도 불리므로 mbRunning이 아니라 mIOCP로 중복 호출을 막는다
void NetworkLib::Stop()
{
	// Start와 같은 락 - 시작 도중에 끼어들거나 Stop 두 개가 동시에 정리하지 않게
	std::lock_guard<std::mutex> startStopLock(mStartStopLock);
	if (mIOCP == nullptr)
	{
		return;	// 시작 안 했거나 이미 종료됨
	}

	mbRunning = false;	// 이후 AcceptEx를 다시 걸지 않는다
	stopSendStallWatch();	// 아래에서 세션을 다 끊는 동안 감시가 같은 세션을 잡지 않게 먼저 멈춘다
	closesocket(mListenSock);	// 걸려 있는 AcceptEx가 전부 실패로 완료된다
	mListenSock = INVALID_SOCKET;

	// 살아 있는 세션의 I/O를 취소하고, 워커가 남은 완료를 처리해 모두 정리될 때까지 기다린다
	// 취소를 대기 루프 안에서 반복하는 이유: 종료 직전에 수락된 세션이 첫 수신을 걸기 전이면 이번 순회에서 놓친다
	while (true)
	{
		for (Session& session : mSessions)
		{
			if (tryIncIoCount(session))
			{
				requestDisconnect(session);	// 새 I/O를 막고 걸린 I/O를 취소 → 실패 완료 → 카운트 0 → 해제
				decIoCount(session);
			}
		}

		bool bAllFree = false;
		{
			std::lock_guard<std::mutex> freeLock(mFreeLock);
			bAllFree = (mFreeIdx.size() == mSessions.size());
		}
		if (bAllFree && mPendingAcceptCnt == 0)
		{
			break;
		}
		Sleep(10);
	}

	stopWorkers();
}

bool NetworkLib::initIOCP(const int workerCnt)
{
	// 마지막 인자 0 = 동시 실행 스레드 수를 코어 수로
	mIOCP = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);
	if (mIOCP == nullptr)
	{
		printf("CreateIoCompletionPort 실패 err=%lu\n", GetLastError());
		return false;
	}

	for (int i = 0; i < workerCnt; ++i)
	{
		mWorkers.emplace_back(mbGqcsEx ? &NetworkLib::runWorkerLoopGqcsEx : &NetworkLib::runWorkerLoopGQCS, this, i);
	}
	return true;
}

void NetworkLib::stopWorkers()
{
	if (mIOCP == nullptr)
	{
		return;
	}

	// 종료 신호 = 키 0 + overlapped nullptr. 워커 하나가 하나씩 가져간다
	for (size_t i = 0; i < mWorkers.size(); ++i)
	{
		PostQueuedCompletionStatus(mIOCP, 0, 0, nullptr);
	}
	for (std::thread& worker : mWorkers)
	{
		worker.join();
	}
	mWorkers.clear();

	CloseHandle(mIOCP);
	mIOCP = nullptr;
}

// AcceptEx 등록이 실패하면 슬롯을 버리지 않고 성공할 때까지 재시도 - 걸려 있는 AcceptEx 수를 고정으로 유지
// false: 종료 중이거나 socket() 생성 실패(치명적 - 종료 요청을 보냄)
bool NetworkLib::postAccept(AcceptOv& ov)
{
	while (mbRunning)
	{
		SOCKET acceptSocket = INVALID_SOCKET;
		{
			ProfileScope socketScope("AcceptEx_Socket");
			acceptSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		}

		if (acceptSocket == INVALID_SOCKET)
		{
			// LOG : Socket 생성에 실패한 상황
			mbRunning = false;	// 종료 요청 - main이 보고 Stop()을 부른다
			return false;
		}

		MemSet(static_cast<OVERLAPPED*>(&ov), 0, sizeof(OVERLAPPED));	// 재사용 전 OVERLAPPED 필드 초기화
		ov.Type = eIoType::Accept;
		ov.AcceptSocket = acceptSocket;

		// 에러 코드는 스코프 안에서 읽음 (소멸자의 기록 코드가 last error를 덮어쓸 가능성 배제)
		DWORD bytesReceived = 0;
		BOOL bOk = FALSE;
		int err = 0;
		++mPendingAcceptCnt;	// AcceptEx 전에 올린다 - 완료가 먼저 처리돼 -1이 앞서는 것을 막는다
		{
			ProfileScope postScope("AcceptEx_Post");
			bOk = AcceptEx(mListenSock, acceptSocket, ov.Buffer, 0,
				sizeof(SOCKADDR_IN) + 16, sizeof(SOCKADDR_IN) + 16, &bytesReceived, &ov);
			if (!bOk)
			{
				err = WSAGetLastError();
			}
		}

		if (bOk || err == WSA_IO_PENDING)
		{
			return true;
		}
		--mPendingAcceptCnt;	// 등록 실패 - 완료 통지가 오지 않는다
		closesocket(acceptSocket);
	}
	return false;
}

// AcceptEx 완료: 같은 슬롯에 AcceptEx를 먼저 다시 걸고, 받은 소켓으로 세션을 만들어 첫 수신을 건다
void NetworkLib::onAcceptComplete(AcceptOv& ov, const BOOL bOk)
{
	// 재등록하면 다음 연결이 같은 버퍼에 주소를 덮어쓰므로, 소켓과 주소를 먼저 꺼내 둔다
	const SOCKET sock = ov.AcceptSocket;
	uint32_t ip = 0;
	uint16_t port = 0;
	if (bOk)
	{
		SOCKADDR* localAddr = nullptr;
		SOCKADDR* remoteAddr = nullptr;
		int localLen = 0;
		int remoteLen = 0;
		// 0, sizeof(SOCKADDR_IN) + 16 두 개는 AcceptEx를 걸 때 넘긴 값과 같아야 한다
		GetAcceptExSockaddrs(ov.Buffer, 0, sizeof(SOCKADDR_IN) + 16, sizeof(SOCKADDR_IN) + 16,
			&localAddr, &localLen, &remoteAddr, &remoteLen);
		const SOCKADDR_IN* remote = reinterpret_cast<const SOCKADDR_IN*>(remoteAddr);
		ip = remote->sin_addr.s_addr;
		port = ntohs(remote->sin_port);
	}
	postAccept(ov);	// 슬롯이 비어 있는 시간을 줄인다 - 비면 연결이 백로그로 간다(측정 결과)
	--mPendingAcceptCnt;	// 끝난 AcceptEx 몫

	if (!bOk)
	{
		closesocket(sock);	// 수락 도중 상대가 끊었거나 리슨 소켓이 닫힘(종료 중)
		return;
	}

	// AcceptEx로 받은 소켓은 리슨 소켓의 속성을 물려받지 않은 상태 - 이걸 해야 getpeername/shutdown이 동작한다
	if (setsockopt(sock, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT,
		reinterpret_cast<char*>(&mListenSock), sizeof(mListenSock)) == SOCKET_ERROR)
	{
		// LOG : SO_UPDATE_ACCEPT_CONTEXT 실패 (종료 중 리슨 소켓이 닫힌 경우 포함)
		CloseReset(sock);
		return;
	}
	if (mbSendBufZero)
	{
		// 송신 버퍼 0 - 커널이 송신 링을 직접 잡고 보내므로 WSASend가 즉시 끝나지 않고 대기(비동기)로 넘어간다
		// 송신 중인 데이터는 완료 전까지 송신 링에 그대로 있어서 버퍼를 직접 잡혀도 안전하다
		const int zero = 0;
		setsockopt(sock, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&zero), sizeof(zero));
	}

	Session* const session = allocSession();
	if (session == nullptr)
	{
		++mSessionFullCnt;
		CloseReset(sock);	// 세션 슬롯이 가득 참 - 받지 않고 끊는다
		return;
	}

	// 완료 키 = Session* - 이 소켓의 완료 통지가 오면 어느 세션인지 바로 안다
	if (CreateIoCompletionPort(reinterpret_cast<HANDLE>(sock), mIOCP, reinterpret_cast<ULONG_PTR>(session), 0) == nullptr)
	{
		// LOG : 세션 소켓 IOCP 등록 실패
		CloseReset(sock);
		freeSession(*session);
		return;
	}

	// 완료 통지 skip 방식: 즉시 끝난 I/O는 완료 통지가 큐에 들어오지 않는다 - postRecv/postSend가 그 자리에서 처리한다
	// 이 소켓만 설정이 빠지면 즉시 완료가 큐와 즉시 처리 양쪽에서 처리되므로, 실패하면 받지 않는다
	if (mbSkipOnSuccess &&
		!SetFileCompletionNotificationModes(reinterpret_cast<HANDLE>(sock), FILE_SKIP_COMPLETION_PORT_ON_SUCCESS | FILE_SKIP_SET_EVENT_ON_HANDLE))
	{
		// LOG : SetFileCompletionNotificationModes 실패
		CloseReset(sock);
		freeSession(*session);
		return;
	}

	session->mSock = sock;
	session->mIP = ip;
	session->mPort = port;
	++mAcceptCnt;

	// 수락 처리 몫으로 카운트를 잡는다 - 0인 채로 OnAccept를 부르면 그 안의 Disconnect가 findSession에서 실패하고,
	// 첫 수신이 바로 실패해 해제되면 OnAccept보다 OnRelease가 먼저 불릴 수 있다
	++session->mIoCount;
	OnAccept(session->mID);
	postRecv(*session);	// OnAccept 뒤에 건다 - 이 세션의 OnRecv가 OnAccept보다 먼저 불리지 않게
	decIoCount(*session);	// 수락 처리 몫
}

// 수신 링의 빈 공간에 수신 1건을 건다 (끝에서 감기면 WSABUF 2개). 즉시 실패하면 올려둔 I/O 카운트를 되돌린다
// 빈 공간이 0인 경우는 없다 - 파일 맨 위 static_assert 참고
// 완료 통지 skip 방식이면 즉시 끝난 수신은 완료 통지가 오지 않으므로 여기서 처리하고 다시 건다 - 재귀 대신 반복
void NetworkLib::postRecv(Session& session)
{
	while (!session.mbDisconnect)	// 끊는 중이면 걸지 않는다 - 호출한 쪽의 카운트 반납으로 해제된다
	{
		MemSet(static_cast<OVERLAPPED*>(&session.mRecvOv), 0, sizeof(OVERLAPPED));
		session.mRecvOv.Type = eIoType::Recv;

		WSABUF bufs[2];
		const int32_t bufCnt = session.mRecvBuf.GetWriteBufs(bufs);
		DWORD flags = 0;
		DWORD received = 0;	// 즉시 완료(0 반환)일 때만 유효 - 대기로 넘어가면 완료 통지의 값을 쓴다

		++session.mIoCount;	// WSARecv 전에 올린다 - 완료가 먼저 처리돼 0으로 떨어지는 것을 막는다
		int ret = 0;
		int err = 0;
		{
			ProfileScope recvScope("WSARecv");	// 시스템 콜 하나만 감싼다 - 커널 안에서 쓴 시간이 이 태그로 잡힌다
			const int64_t callStart = getNowQPC();
			ret = WSARecv(session.mSock, bufs, bufCnt, &received, &flags, &session.mRecvOv, nullptr);
			if (ret == SOCKET_ERROR)
			{
				err = WSAGetLastError();
			}
			if (tWorkerIdx >= 0)	// 반환 결과(즉시 성공 / 대기)별 반환 시간 - 워커 스레드만 센다
			{
				WorkerCounter& counter = *mWorkerCounters[tWorkerIdx];
				CountIoResult(ret, err, getNowQPC() - callStart, counter.RecvSyncCnt, counter.RecvSyncTicks, counter.RecvPendingCnt, counter.RecvPendingTicks);
			}
		}
		if (ret == SOCKET_ERROR)
		{
			if (err != WSA_IO_PENDING)
			{
				// 상대가 이미 끊었거나 소켓 오류 - 완료 통지가 오지 않으므로 직접 되돌린다
				decIoCount(session);
				return;
			}
		}
		else if (mbSkipOnSuccess)
		{
			// 즉시 완료 - 큐에 완료가 들어가지 않는다. 호출한 쪽이 카운트를 잡고 있으므로 수신 몫을 먼저 반납해도 해제되지 않는다
			const bool bAlive = completeRecv(session, received);
			decIoCount(session);	// 끝난 수신 몫
			if (!bAlive)
			{
				return;	// 상대가 끊음 - 다시 걸지 않는다
			}
			continue;
		}

		// 위의 확인과 WSARecv 사이에 끊기 요청이 오면, 그 CancelIoEx는 아직 안 걸린 이 수신을 못 잡는다 - 걸고 나서 다시 확인
		if (session.mbDisconnect)
		{
			CancelIoEx(reinterpret_cast<HANDLE>(session.mSock), &session.mRecvOv);
		}
		return;
	}
}

// 수신 완료: 실패나 0바이트면 상대가 끊은 것 - 다시 걸지 않는다. 받았으면 패킷을 처리하고 다음 수신을 건다
void NetworkLib::onRecvComplete(Session& session, const BOOL bOk, const DWORD transferred)
{
	if (!bOk || !completeRecv(session, transferred))
	{
		decIoCount(session);	// 수신 몫을 내려놓는다 - 송신이 없으면 여기서 해제
		return;
	}
	postRecv(session);	// 끊는 중이면 걸지 않는다
	decIoCount(session);	// 끝난 수신 몫
}

// 받은 바이트에서 완성된 패킷을 하나씩 OnRecv로 넘긴다. 마지막에 잘린 패킷은 링에 남아 다음 수신과 이어진다
bool NetworkLib::completeRecv(Session& session, const DWORD transferred)
{
	Assert(tWorkerIdx >= 0);	// 수신 처리는 워커에서만 일어난다(완료 처리, 또는 워커가 건 수신의 즉시 완료)
	if (transferred == 0)
	{
		return false;
	}

	// WSARecv가 링의 빈 공간에 직접 받았으므로 쓰기 위치만 옮긴다
	session.mRecvBuf.MoveWrite(static_cast<int32_t>(transferred));

	char packet[MAX_PACKET_SIZE];
	int64_t packetCnt = 0;	// 완료 1건에 패킷이 여러 개일 수 있다 - 카운터는 끝에 한 번만 올린다
	const bool bOwnBatch = !tbDeferSend;	// 워커가 꺼낸 완료 전부에 걸쳐 이미 켜 두었으면(mergeSends) 보내기는 묶음 끝에 맡긴다
	tbDeferSend = true;	// 이 수신으로 생긴 송신을 받는 사람마다 WSASend 한 번으로 묶는다 - 묶임을 완료 지연이라는 우연에 맡기지 않는다
	while (!session.mbDisconnect)	// OnRecv 안에서 Disconnect했거나 SendPacket이 넘쳐 끊었으면 남은 패킷은 버린다
	{
		PacketHeader header;
		if (!session.mRecvBuf.Peek(reinterpret_cast<char*>(&header), PACKET_HEADER_SIZE))
		{
			break;	// 헤더도 다 오지 않음
		}
		if (header.Len > MAX_PAYLOAD_SIZE)
		{
			// 정상 클라이언트는 보낼 수 없는 헤더 - 패킷 경계를 잃었으므로 이후 바이트도 믿을 수 없다
			++mBadPacketCnt;
			requestDisconnect(session);
			break;
		}

		const int32_t packetSize = PACKET_HEADER_SIZE + header.Len;
		if (!session.mRecvBuf.Peek(packet, packetSize))
		{
			break;	// 페이로드가 덜 옴
		}
		session.mRecvBuf.Pop(packetSize);

		const char* payload = packet + PACKET_HEADER_SIZE;
		++packetCnt;
		OnRecv(session.mID, payload, header.Len);
	}
	if (bOwnBatch)
	{
		tbDeferSend = false;
		flushSends();	// 쌓인 송신을 받는 사람마다 한 번에 - 컨텐츠가 잡았던 락(채팅의 방 락 등) 밖에서 WSASend가 일어난다
	}

	WorkerCounter& counter = *mWorkerCounters[tWorkerIdx];
	AddSingleWriter(counter.RecvPacketCnt, packetCnt);
	AddSingleWriter(counter.RecvCompleteCnt, 1);
	return true;
}

// 컨텐츠 패킷 송신. OnRecv 안에서만 부를 수 있다 - 그 수신 처리가 끝날 때 받는 사람마다 한 번에 보낸다
bool NetworkLib::SendPacket(const uint64_t id, const char* payload, const int32_t len)
{
	// 모으는 구간 밖이면 모은 것을 보낼 곳(flushSends)이 없다 - 링에 넣으면 메시지가 안 나가고 목록 몫 카운트도 반납되지 않는다
	if (!tbDeferSend)
	{
		return false;
	}
	Session* const session = findSession(id);
	if (session == nullptr)
	{
		return false;	// 이미 해제됨 (또는 슬롯이 다른 세션에 재사용됨)
	}
	const bool bSent = marshalToSendBuf(*session, payload, len);
	decIoCount(*session);	// findSession 몫
	return bSent;
}

// 헤더(길이)를 붙여 송신 링에 넣고 보낼 목록에 올린다 - WSASend는 수신 처리 끝(flushSends)에서
bool NetworkLib::marshalToSendBuf(Session& session, const char* payload, const int32_t len)
{
	if (len < 0 || len > MAX_PAYLOAD_SIZE)
	{
		return false;	// 받는 쪽이 비정상 패킷으로 보고 끊을 길이
	}
	if (session.mbDisconnect)
	{
		return false;
	}

	PacketHeader header;
	header.Len = static_cast<uint16_t>(len);
	const int32_t packetSize = PACKET_HEADER_SIZE + len;
	bool bPushed = false;
	{
		std::lock_guard<std::mutex> sendLock(session.mSendLock);
		// 헤더와 페이로드 중 하나만 들어가면 스트림이 깨지므로 둘 다 들어갈 때만 넣는다 (SEND_RING_SIZE 설명 참고)
		if (session.mSendBuf.GetFreeSize() >= packetSize)
		{
			session.mSendBuf.Push(reinterpret_cast<const char*>(&header), PACKET_HEADER_SIZE);
			session.mSendBuf.Push(payload, len);
			bPushed = true;
		}
	}
	if (!bPushed)
	{
		// 송신 링이 가득 참 - 상대가 받는 속도보다 빨리 쌓였다
		++mOverflowCnt;
		requestDisconnect(session);
		return false;
	}
	Assert(tWorkerIdx >= 0);	// 수신 처리 안이라 항상 워커 스레드
	AddSingleWriter(mWorkerCounters[tWorkerIdx]->SendPacketCnt, 1);
	addSendTarget(session);	// 받는 사람이 누구든 수신 처리 끝에서 보낸다
	return true;
}

// 보낼 목록에 올린다. marshalToSendBuf를 부른 쪽이 카운트를 잡고 있으므로 0에서 올리는 일은 없다
void NetworkLib::addSendTarget(Session& session)
{
	if (session.mbSendTarget.exchange(true))
	{
		return;	// 이미 어느 워커의 목록에 있다 - 그 워커가 보낼 때 링에 들어 있는 이번 것도 같이 나간다
	}
	++session.mIoCount;	// 목록에 있는 동안 해제되지 않게 - flushSends가 반납
	tSendTargets.push_back(&session);	// 세션은 목록에 최대 한 번이라 maxSession을 넘지 않는다 - reserve해 둔 용량 안
}

// 목록의 세션마다 송신 1건 - postSend는 링에 쌓인 전부를 한 번에 건다
void NetworkLib::flushSends()
{
	for (Session* session : tSendTargets)
	{
		// 링을 읽기 전에 내린다 - 그 뒤에 넣은 스레드는 플래그가 내려간 것을 보고 자기 목록에 다시 올린다
		// (내리기와 postSend의 링 읽기 사이를 mSendLock이 이어 줘서, 그 전에 넣은 것은 이번에 나간다)
		session->mbSendTarget = false;
		postSend(*session);
		decIoCount(*session);	// 목록 몫 - 0이 되면 해제 작업이 큐로 간다(재진입 없음)
	}
	tSendTargets.clear();	// 용량은 그대로 - 다음 수신 처리에서 할당 없음
}

// 끊기 요청 - 실제 해제와 OnRelease는 걸린 I/O가 모두 끝난 뒤 워커에서 일어난다
void NetworkLib::Disconnect(const uint64_t id)
{
	Session* const session = findSession(id);
	if (session == nullptr)
	{
		return;
	}
	requestDisconnect(*session);
	decIoCount(*session);	// findSession 몫
}

// 끊기 플래그를 세우고 걸린 I/O를 취소한다 - 실패 완료가 오면 다시 걸지 않으므로 카운트가 0으로 내려간다
// 걸려 있는 송신은 상대가 안 받으면 끝나지 않으므로 취소해서 완료를 앞당긴다
// 호출하는 쪽이 카운트를 잡고 있어야 mSock이 유효하다
void NetworkLib::requestDisconnect(Session& session)
{
	session.mbDisconnect = true;
	CancelIoEx(reinterpret_cast<HANDLE>(session.mSock), nullptr);
}

// 쌓인 데이터를 송신 1건으로 건다. 이미 송신 중이거나 보낼 게 없으면 아무것도 하지 않는다
// mbSendFlag는 mSendLock 안에서만 읽고 쓴다 - 링이 비었는지 확인과 플래그 변경이 한 번에 일어나야 데이터가 남지 않는다
// 완료 통지 skip 방식이면 즉시 끝난 송신은 완료 통지가 오지 않으므로 여기서 마무리하고, 그사이 쌓인 데이터를 이어서 보낸다 - 재귀 대신 반복
// 워커에서만 불린다(flushSends, 송신 완료) - 즉시 완료 처리도 그 워커에서 일어난다
void NetworkLib::postSend(Session& session)
{
	while (!session.mbDisconnect)
	{
		WSABUF bufs[2];
		int32_t bufCnt = 0;
		{
			std::lock_guard<std::mutex> sendLock(session.mSendLock);
			if (session.mbSendFlag)
			{
				return;	// 송신 중 - 완료 쪽이 이어서 보낸다
			}
			bufCnt = session.mSendBuf.GetReadBufs(bufs);
			if (bufCnt == 0)
			{
				return;
			}
			session.mbSendFlag = true;
			// 플래그와 같은 락 안에서 적는다 - 락 밖이면 감시 스레드가 "송신 중"을 보면서 이전 송신의 시각을 읽어 정상 클라를 정체로 오판할 수 있다
			// WSASend 전에 적어야 빠른 완료(송신 완료까지 걸린 시간 측정)와 경쟁하지 않는다
			session.mWsaSendQPC = getNowQPC();
		}

		MemSet(static_cast<OVERLAPPED*>(&session.mSendOv), 0, sizeof(OVERLAPPED));
		session.mSendOv.Type = eIoType::Send;
		DWORD sent = 0;	// 즉시 완료(0 반환)일 때만 유효 - 대기로 넘어가면 완료 통지의 값을 쓴다

		++session.mIoCount;	// WSASend 전에 올린다 - postRecv와 같은 이유
		int ret = 0;
		int err = 0;
		{
			ProfileScope sendScope("WSASend");	// 시스템 콜 하나만 감싼다
			const int64_t callStart = getNowQPC();
			ret = WSASend(session.mSock, bufs, bufCnt, &sent, 0, &session.mSendOv, nullptr);
			if (ret == SOCKET_ERROR)
			{
				err = WSAGetLastError();
			}
			if (tWorkerIdx >= 0)	// 반환 결과(즉시 성공 / 대기)별 반환 시간 - 지금은 항상 워커지만 카운터가 워커에만 있어 확인한다
			{
				WorkerCounter& counter = *mWorkerCounters[tWorkerIdx];
				CountIoResult(ret, err, getNowQPC() - callStart, counter.SendSyncCnt, counter.SendSyncTicks, counter.SendPendingCnt, counter.SendPendingTicks);
			}
		}
		if (ret == SOCKET_ERROR)
		{
			if (err != WSA_IO_PENDING)
			{
				// 끊긴 소켓 - 완료 통지가 오지 않으므로 직접 되돌린다
				// mbSendFlag는 true로 남겨 더 보내지 않는다 (해제 시 false로 돌아감)
				// 수신이 살아 있으면 보내지 못하는 세션이 남으므로 끊는다 - 안 끊으면 송신 정체 감시가 상대 탓(정체)으로 세게 된다
				requestDisconnect(session);
				decIoCount(session);
				return;
			}
		}
		else if (mbSkipOnSuccess)
		{
			// 즉시 완료 - 큐에 완료가 들어가지 않는다. 호출한 쪽이 카운트를 잡고 있으므로 송신 몫을 먼저 반납해도 해제되지 않는다
			completeSend(session, sent);
			decIoCount(session);	// 끝난 송신 몫
			continue;	// 송신하는 동안 다른 스레드가 쌓은 데이터
		}

		// postRecv와 같은 이유 - 상대가 안 받는 중에 끊기 요청을 놓치면 이 송신이 끝나지 않아 해제되지 않는다
		if (session.mbDisconnect)
		{
			CancelIoEx(reinterpret_cast<HANDLE>(session.mSock), &session.mSendOv);
		}
		return;
	}
}

// 송신 완료: 마무리하고, 송신 중에 쌓인 데이터가 있으면 이어서 보낸다
// 실패(끊김, 취소)면 플래그를 내리지 않아 더 보내지 않는다 - 해제 시 false로 돌아감
void NetworkLib::onSendComplete(Session& session, const BOOL bOk, const DWORD transferred)
{
	if (!bOk)
	{
		requestDisconnect(session);	// 송신 실패 - 위 postSend의 즉시 실패와 같은 이유로 끊는다(이미 끊는 중이면 취소만 한 번 더)
		decIoCount(session);
		return;
	}
	if (mTestSendCompleteDelayMs > 0)
	{
		Sleep(mTestSendCompleteDelayMs);	// [테스트 전용] 그동안 다른 워커가 같은 세션의 수신을 처리하며 송신 링에 쌓는다
	}
	completeSend(session, transferred);
	postSend(session);	// 송신 중에 쌓인 데이터
	decIoCount(session);	// 끝난 송신 몫
}

// 보낸 만큼 링에서 빼고 플래그를 내린다 - 큐로 온 완료와 즉시 완료(완료 통지 skip 방식)가 같이 쓴다
void NetworkLib::completeSend(Session& session, const DWORD transferred)
{
	int64_t wsaSendQPC = 0;
	{
		std::lock_guard<std::mutex> sendLock(session.mSendLock);
		session.mSendBuf.Pop(static_cast<int32_t>(transferred));
		session.mbSendFlag = false;
		wsaSendQPC = session.mWsaSendQPC;	// 플래그를 내리면 다른 워커가 새 송신을 걸며 덮어쓸 수 있어 락 안에서 읽는다
	}
	if (tWorkerIdx < 0)
	{
		return;	// 카운터는 워커에만 있다 - 지금은 항상 워커지만 안전망
	}
	WorkerCounter& counter = *mWorkerCounters[tWorkerIdx];
	AddSingleWriter(counter.SendCompleteCnt, 1);
	AddSingleWriter(counter.SendBytes, transferred);
	AddSingleWriter(counter.SendWaitTicks, getNowQPC() - wsaSendQPC);	// 송신을 건 순간부터 이 완료를 처리하는 지금까지
}

// I/O 카운트 -1. 0이 되면 이 세션을 건드리는 I/O도 스레드도 없으므로 해제한다
// 해제는 IOCP에 작업으로 넣어 항상 워커가 한다 - 다른 세션의 OnRecv 안에서 SendPacket/Disconnect로 0이 되면
// 그 자리에서 OnRelease가 재진입해, 컨텐츠가 잡고 있던 락(채팅의 방 락 등)을 다시 잡으려다 데드락이 날 수 있다
void NetworkLib::decIoCount(Session& session)
{
	if (--session.mIoCount != 0)
	{
		return;
	}

	MemSet(static_cast<OVERLAPPED*>(&session.mReleaseOv), 0, sizeof(OVERLAPPED));
	session.mReleaseOv.Type = eIoType::Release;
	if (!PostQueuedCompletionStatus(mIOCP, 0, reinterpret_cast<ULONG_PTR>(&session), &session.mReleaseOv))
	{
		// LOG : PQCS 실패 - 슬롯이 영영 안 돌아오는 것보다는 이 자리에서 해제하는 편이 낫다
		releaseSession(session);
	}
}

// 소켓을 RST로 닫고, 컨텐츠에 알리고, 보내지 못한 데이터와 덜 받은 패킷을 버리고, 슬롯을 돌려준다
// OnRelease는 슬롯을 돌려주기 전에 부른다 - 콜백 도중 같은 슬롯이 다른 연결에 재사용되지 않게
void NetworkLib::releaseSession(Session& session)
{
	CloseReset(session.mSock);
	session.mSock = INVALID_SOCKET;
	OnRelease(session.mID);

	session.mIP = 0;
	session.mPort = 0;
	session.mbDisconnect = false;
	session.mbSendFlag = false;
	session.mSendBuf.Clear();
	session.mRecvBuf.Clear();
	++mReleaseCnt;
	freeSession(session);
}

// 송신 정체 감시: 1초마다 세션 전부를 돌며 "송신이 걸려 있고, 건 지 기준 시간이 넘은" 세션에 끊기 요청을 한다
// 받는 쪽 수신 창이 0이면 TCP는 끊지 않고 WSASend도 끝나지 않는다 - 그 세션의 송신 링·소켓이 계속 묶이므로 시간으로 끊는다
// 끊기 요청만 하고(걸린 송신 취소 → 실패 완료 → 카운트 0), 해제와 OnRelease는 지금처럼 워커에서 일어난다
// 1초에 한 번, 세션마다 카운트 CAS 1회와 송신 락 1회 - 핫패스(수신·송신)에는 이미 적던 시각 하나 말고 더해지는 일이 없다
void NetworkLib::runSendStallWatch()
{
	SetThreadDescription(GetCurrentThread(), L"SendStallWatch");
	const int64_t limitTicks = static_cast<int64_t>(mSendStallSec) * mQpcFreq;

	// 1초 동안 멈춤 신호가 없으면(WAIT_TIMEOUT) 한 번 순회, 신호가 오면(WAIT_OBJECT_0) 바로 끝난다
	while (WaitForSingleObject(mWatchStopEvent, 1000) == WAIT_TIMEOUT)
	{
		const int64_t now = getNowQPC();
		for (Session& session : mSessions)
		{
			if (!tryIncIoCount(session))
			{
				continue;	// 빈 슬롯이거나 해제 중
			}
			bool bStall = false;
			{
				std::lock_guard<std::mutex> sendLock(session.mSendLock);
				bStall = session.mbSendFlag && (now - session.mWsaSendQPC > limitTicks);
			}
			// 끊기 플래그를 확인과 동시에 세운다 - 따로 하면 그 사이 워커가 넘침 등으로 끊어 한 연결이 두 통계에 잡힌다
			// 이미 끊는 중이면 다른 이유로 끊긴 것이라 정체로 세지 않는다
			if (bStall && !session.mbDisconnect.exchange(true))
			{
				++mSendStallCnt;
				CancelIoEx(reinterpret_cast<HANDLE>(session.mSock), nullptr);	// requestDisconnect의 나머지 절반 - 플래그는 위에서 세웠다
			}
			decIoCount(session);
		}
	}
}

void NetworkLib::stopSendStallWatch()
{
	if (!mWatchThread.joinable())
	{
		return;	// 감시를 끈 설정이거나 Start가 중간에 실패
	}
	SetEvent(mWatchStopEvent);	// 1초 대기 중이어도 바로 깨운다
	mWatchThread.join();
	CloseHandle(mWatchStopEvent);
	mWatchStopEvent = nullptr;
}

// 각 카운터를 따로 읽으므로 값들이 같은 순간의 값은 아니다 - 1초 통계용으로는 충분
// release를 accept보다 먼저 읽어 현재 세션 수(accept - release)가 음수로 나오지 않게 한다
NetStat NetworkLib::GetStat() const
{
	NetStat stat;
	stat.ReleaseCnt = mReleaseCnt.load();
	stat.AcceptCnt = mAcceptCnt.load();
	stat.OverflowCnt = mOverflowCnt.load();
	stat.SendStallCnt = mSendStallCnt.load();
	stat.SessionFullCnt = mSessionFullCnt.load();
	stat.BadPacketCnt = mBadPacketCnt.load();
	for (const UniquePtr<WorkerCounter>& counter : mWorkerCounters)
	{
		stat.RecvPacketCnt += counter->RecvPacketCnt.load(std::memory_order_relaxed);
		stat.SendPacketCnt += counter->SendPacketCnt.load(std::memory_order_relaxed);
		stat.RecvCompleteCnt += counter->RecvCompleteCnt.load(std::memory_order_relaxed);
		stat.SendCompleteCnt += counter->SendCompleteCnt.load(std::memory_order_relaxed);
		stat.SendBytes += counter->SendBytes.load(std::memory_order_relaxed);
		stat.GqcsCallCnt += counter->GqcsCallCnt.load(std::memory_order_relaxed);
		stat.IocpCompleteCnt += counter->IocpCompleteCnt.load(std::memory_order_relaxed);
		stat.SendSyncCnt += counter->SendSyncCnt.load(std::memory_order_relaxed);
		stat.SendPendingCnt += counter->SendPendingCnt.load(std::memory_order_relaxed);
		stat.SendSyncTicks += counter->SendSyncTicks.load(std::memory_order_relaxed);
		stat.SendPendingTicks += counter->SendPendingTicks.load(std::memory_order_relaxed);
		stat.RecvSyncCnt += counter->RecvSyncCnt.load(std::memory_order_relaxed);
		stat.RecvPendingCnt += counter->RecvPendingCnt.load(std::memory_order_relaxed);
		stat.RecvSyncTicks += counter->RecvSyncTicks.load(std::memory_order_relaxed);
		stat.RecvPendingTicks += counter->RecvPendingTicks.load(std::memory_order_relaxed);
		stat.SendWaitTicks += counter->SendWaitTicks.load(std::memory_order_relaxed);
	}
	return stat;
}

// 빈 세션 슬롯 하나를 꺼내고 세대를 올려 새 ID를 붙인다. 가득 차면 nullptr
// 꺼낸 시점의 I/O 카운트는 0이라 tryIncIoCount가 실패하므로, 여기서 쓴 mID를 동시에 읽는 스레드는 없다
Session* NetworkLib::allocSession()
{
	std::lock_guard<std::mutex> freeLock(mFreeLock);
	if (mFreeIdx.empty())
	{
		return nullptr;
	}
	const int32_t idx = mFreeIdx.back();
	mFreeIdx.pop_back();

	Session& session = mSessions[idx];
	const uint32_t gen = static_cast<uint32_t>(session.mID >> 32) + 1;
	session.mID = (static_cast<uint64_t>(gen) << 32) | static_cast<uint32_t>(idx);
	return &session;
}

// 세션 슬롯을 빈 슬롯 스택에 돌려준다
void NetworkLib::freeSession(const Session& session)
{
	std::lock_guard<std::mutex> freeLock(mFreeLock);
	mFreeIdx.push_back(session.mIdx);
}

// 세션이 살아 있을 때(카운트 > 0)만 I/O 카운트를 +1 한다
// 성공하면 내가 -1 하기 전까지 해제되지 않으므로 mSock을 안전하게 쓸 수 있다
bool NetworkLib::tryIncIoCount(Session& session)
{
	int32_t count = session.mIoCount.load();
	while (count > 0)
	{
		if (session.mIoCount.compare_exchange_weak(count, count + 1))
		{
			return true;
		}
	}
	return false;	// 0 = 비어 있거나 해제 중
}

// 컨텐츠는 Session*가 아니라 ID를 들고 있다가 이 함수로 세션을 얻는다 (SendPacket/Disconnect용)
// 슬롯 번호만으로 찾으면, 해제 후 같은 슬롯에 들어온 다른 클라이언트를 잡을 수 있다 - 그래서 카운트를 올린 뒤 ID까지 비교
// 카운트를 올리는 데 성공하면 해제·재할당이 막히므로 그 뒤에 읽는 mID는 바뀌지 않는다
Session* NetworkLib::findSession(const uint64_t id)
{
	const uint32_t idx = GetIdx(id);
	if (idx >= mSessions.size())
	{
		return nullptr;
	}

	Session& session = mSessions[idx];
	if (!tryIncIoCount(session))
	{
		return nullptr;
	}
	if (session.mID != id)
	{
		decIoCount(session);	// 같은 슬롯의 다른 세션 - 잡은 카운트만 되돌린다
		return nullptr;
	}
	return &session;
}

// 완료 1건 처리 - 두 워커 함수가 같이 쓴다. bOk == FALSE면 그 I/O가 실패한 것(연결 끊김, 취소 등)이고 각 완료 처리가 판단한다
void NetworkLib::handleCompletion(MyOverlapped& ov, Session* session, const BOOL bOk, const DWORD transferred)
{
	ProfileScope completeScope("Complete");	// 완료 1건 처리 전체
	switch (ov.Type)
	{
	case eIoType::Accept:
		onAcceptComplete(static_cast<AcceptOv&>(ov), bOk);
		break;
	case eIoType::Recv:
		Assert(session != nullptr);	// 세션 소켓의 완료 키는 Session*
		onRecvComplete(*session, bOk, transferred);
		break;
	case eIoType::Send:
		Assert(session != nullptr);
		onSendComplete(*session, bOk, transferred);
		break;
	case eIoType::Release:
		Assert(session != nullptr);	// decIoCount가 완료 키로 Session*를 넣는다
		releaseSession(*session);
		break;
	default:
		Assert(false);	// 우리가 건 I/O에는 위 네 종류만 있다
		break;
	}
}

void NetworkLib::runWorkerLoopGQCS(const int workerIdx)
{
	tWorkerIdx = workerIdx;
	tSendTargets.reserve(mSessions.size());	// 세션은 목록에 최대 한 번 - 최대 세션 수만큼 잡아 두면 수신 처리 중 재할당이 없다
	while (true)
	{
		DWORD transferred = 0;
		ULONG_PTR key = 0;
		OVERLAPPED* overlapped = nullptr;
		BOOL bOk = FALSE;
		DWORD err = 0;

		// 완료 대기 시간 포함 - 길면 워커가 일감을 기다리며 놀고 있다는 뜻
		// 에러 코드는 스코프 안에서 읽음 (소멸자의 기록 코드가 last error를 덮어쓸 가능성 배제)
		{
			ProfileScope gqcsScope("GQCS");
			bOk = GetQueuedCompletionStatus(mIOCP, &transferred, &key, &overlapped, INFINITE);
			if (!bOk)
			{
				err = GetLastError();
			}
		}
		AddSingleWriter(mWorkerCounters[tWorkerIdx]->GqcsCallCnt, 1);	// 호출 1번 = 완료 1건 (GQCSEx와 비교용)
		AddSingleWriter(mWorkerCounters[tWorkerIdx]->IocpCompleteCnt, 1);

		if (overlapped == nullptr)
		{
			// bOk == TRUE면 stopWorkers의 종료 신호, FALSE면 IOCP 자체 오류
			if (!bOk)
			{
				printf("GQCS 실패 err=%lu\n", err);
			}
			break;
		}

		if (mbMergeSends)
		{
			tbDeferSend = true;	// GQCS는 한 번에 완료 1건 - 묶음 = 완료 1건이라 수신 1건 끝에서 보내는 것과 거의 같다(수락·해제 콜백의 송신도 묶인다)
		}
		// 리슨 소켓(Accept)은 키 0이라 session이 nullptr
		handleCompletion(*static_cast<MyOverlapped*>(overlapped), reinterpret_cast<Session*>(key), bOk, transferred);
		if (mbMergeSends)
		{
			tbDeferSend = false;
			flushSends();
		}
	}
}

// GQCS 대신 GQCSEx로 완료를 최대 mGqcsBatch개씩 꺼낸다 - 호출 1번에 완료 여러 개라 커널 전환과 IOCP 포트 락을 잡는 횟수가 준다
// GQCS와 달리 주의할 점 두 가지
// 1. 완료마다의 성공/실패를 반환값으로 주지 않는다 - OVERLAPPED.Internal(NTSTATUS)을 직접 본다
// 2. 종료 신호(overlapped == nullptr)를 한 번에 여러 개 꺼낼 수 있다 - 내 몫 1개 외에는 다른 워커 몫이라 다시 넣는다
void NetworkLib::runWorkerLoopGqcsEx(const int workerIdx)
{
	tWorkerIdx = workerIdx;
	tSendTargets.reserve(mSessions.size());	// 세션은 목록에 최대 한 번 - 최대 세션 수만큼 잡아 두면 수신 처리 중 재할당이 없다
	std::vector<OVERLAPPED_ENTRY> entries(mGqcsBatch);
	while (true)
	{
		ULONG count = 0;
		BOOL bOk = FALSE;
		DWORD err = 0;

		// 완료 대기 시간 포함 - 길면 워커가 일감을 기다리며 놀고 있다는 뜻
		{
			ProfileScope gqcsScope("GQCS");
			bOk = GetQueuedCompletionStatusEx(mIOCP, entries.data(), mGqcsBatch, &count, INFINITE, FALSE);
			if (!bOk)
			{
				err = GetLastError();
			}
		}
		if (!bOk)
		{
			printf("GQCSEx 실패 err=%lu\n", err);	// 함수 자체의 실패(IOCP 오류) - 개별 I/O 실패는 아래에서 Internal로 판단
			break;
		}
		AddSingleWriter(mWorkerCounters[tWorkerIdx]->GqcsCallCnt, 1);	// 호출 1번 = 완료 count건
		AddSingleWriter(mWorkerCounters[tWorkerIdx]->IocpCompleteCnt, count);

		if (mbMergeSends)
		{
			tbDeferSend = true;	// 이 묶음에서 생긴 송신은 묶음 끝에서 받는 사람마다 한 번에 - 여러 보낸 사람의 메시지가 합쳐진다
		}
		bool bQuit = false;
		for (ULONG i = 0; i < count; ++i)
		{
			const OVERLAPPED_ENTRY& entry = entries[i];
			if (entry.lpOverlapped == nullptr)
			{
				// stopWorkers의 종료 신호 - 워커 수만큼 넣었으므로 두 번째부터는 다른 워커에게 돌려준다
				if (bQuit)
				{
					PostQueuedCompletionStatus(mIOCP, 0, 0, nullptr);
				}
				bQuit = true;
				continue;	// 같이 꺼낸 다른 완료는 마저 처리한다 - 버리면 I/O 카운트가 안 내려가 세션이 해제되지 않는다
			}

			// NTSTATUS가 음수면 그 I/O가 실패한 것 (연결 끊김, 취소 등) - GQCS가 FALSE를 돌려주는 경우와 같다
			// PQCS로 넣은 해제 작업은 mReleaseOv를 0으로 채워 넣으므로 성공(0)으로 보인다
			const BOOL bIoOk = (static_cast<LONG>(entry.lpOverlapped->Internal) >= 0) ? TRUE : FALSE;
			handleCompletion(*static_cast<MyOverlapped*>(entry.lpOverlapped), reinterpret_cast<Session*>(entry.lpCompletionKey),
				bIoOk, entry.dwNumberOfBytesTransferred);
		}
		if (mbMergeSends)
		{
			tbDeferSend = false;
			flushSends();	// 종료 신호를 꺼냈어도 같은 묶음에서 생긴 송신은 마저 보낸다
		}
		if (bQuit)
		{
			break;
		}
	}
}
