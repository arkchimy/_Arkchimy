#include "NetworkLib.h"
#include "ListenSocket.h"
#include "Profiler.h"
#include "SocketUtil.h"
#include <cstdio>

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
	// Session은 mutex를 가져 이동이 안 되므로 resize 대신 만들어서 swap
	std::vector<Session> sessions(maxSession);
	mSessions.swap(sessions);
	mFreeIdx.reserve(maxSession);
	for (int32_t i = maxSession - 1; i >= 0; --i)
	{
		mSessions[i].mIdx = i;
		mFreeIdx.push_back(i);	// 역순으로 넣어 0번부터 꺼내지게
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
	return true;
}

// 안전망: 종료 절차를 건너뛰어도 join 안 된 std::thread 소멸로 terminate 되지 않게
NetworkLib::~NetworkLib()
{
	stop();
}

// 종료: AcceptEx 중단 → 세션 전부 끊기 → 걸린 I/O가 모두 끝날 때까지 대기 → 워커 종료
// socket() 실패로 mRunning이 이미 내려가 있어도 불리므로 mRunning이 아니라 mIocp로 중복 호출을 막는다
void NetworkLib::stop()
{
	if (mIocp == nullptr)
	{
		return;	// 시작 안 했거나 이미 종료됨
	}

	mRunning = false;	// 이후 AcceptEx를 다시 걸지 않는다
	closesocket(mListenSock);	// 걸려 있는 AcceptEx가 전부 실패로 완료된다
	mListenSock = INVALID_SOCKET;

	// 살아 있는 세션의 I/O를 취소하고, 워커가 남은 완료를 처리해 모두 정리될 때까지 기다린다
	// 취소를 대기 루프 안에서 반복하는 이유: 종료 직전에 수락된 세션이 첫 수신을 걸기 전이면 이번 순회에서 놓친다
	while (true)
	{
		for (Session& session : mSessions)
		{
			if (acquireSession(&session))
			{
				CancelIoEx(reinterpret_cast<HANDLE>(session.mSock), nullptr);	// 실패 완료 → 카운트 0 → 해제
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
		mWorkers.emplace_back(&NetworkLib::workerLoop, this);
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
	postRecv(session);
}

// 수신 1건을 건다. 즉시 실패하면 올려둔 I/O 카운트를 되돌린다
void NetworkLib::postRecv(Session* session)
{
	ZeroMemory(static_cast<OVERLAPPED*>(&session->mRecvOv), sizeof(OVERLAPPED));
	session->mRecvOv.type = eIoType::Recv;

	WSABUF buf;
	buf.buf = session->mRecvBuf;
	buf.len = sizeof(session->mRecvBuf);
	DWORD flags = 0;

	++session->mIoCount;	// WSARecv 전에 올린다 - 완료가 먼저 처리돼 0으로 떨어지는 것을 막는다
	if (WSARecv(session->mSock, &buf, 1, nullptr, &flags, &session->mRecvOv, nullptr) == SOCKET_ERROR)
	{
		int err = WSAGetLastError();
		if (err != WSA_IO_PENDING)
		{
			// 상대가 이미 끊었거나 소켓 오류 - 완료 통지가 오지 않으므로 직접 되돌린다
			decIoCount(session);
		}
	}
}

// 수신 완료: 0바이트나 실패면 상대가 끊은 것 - 다시 걸지 않는다
// 받았으면 송신 링에 넣고(에코) 다음 수신을 건다. 링이 가득 차면 끊는다
void NetworkLib::onRecvComplete(Session* session, BOOL ok, DWORD transferred)
{
	if (!ok || transferred == 0)
	{
		decIoCount(session);	// 수신 몫을 내려놓는다 - 송신이 없으면 여기서 해제
		return;
	}

	// 에코: 받은 만큼 송신 링에 넣는다. 컨텐츠가 붙으면 이 자리가 패킷 조립 + 컨텐츠 전달로 바뀐다
	bool pushed = false;
	{
		std::lock_guard<std::mutex> lock(session->mSendLock);
		pushed = session->mSendBuf.push(session->mRecvBuf, static_cast<int32_t>(transferred));
	}
	if (!pushed)
	{
		++mOverflowCnt;
		// 송신 링이 가득 참 - 상대가 받는 속도보다 빨리 쌓였다. 수신을 다시 걸지 않고 끊는다
		// 걸려 있는 송신은 상대가 안 받으면 끝나지 않으므로 취소해서 완료를 앞당긴다
		CancelIoEx(reinterpret_cast<HANDLE>(session->mSock), nullptr);
		decIoCount(session);
		return;
	}
	postSend(session);

	postRecv(session);
	decIoCount(session);	// 끝난 수신 몫
}

// 쌓인 데이터를 송신 1건으로 건다. 이미 송신 중이거나 보낼 게 없으면 아무것도 하지 않는다
// mSendFlag는 mSendLock 안에서만 읽고 쓴다 - 링이 비었는지 확인과 플래그 변경이 한 번에 일어나야 데이터가 남지 않는다
void NetworkLib::postSend(Session* session)
{
	WSABUF bufs[2];
	int32_t bufCnt = 0;
	{
		std::lock_guard<std::mutex> lock(session->mSendLock);
		if (session->mSendFlag)
		{
			return;	// 송신 중 - 완료 쪽이 이어서 보낸다
		}
		bufCnt = session->mSendBuf.getSendBufs(bufs);
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
		}
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

// I/O 카운트 -1. 0이 되면 이 세션을 건드리는 I/O가 하나도 없으므로 해제한다
void NetworkLib::decIoCount(Session* session)
{
	if (--session->mIoCount == 0)
	{
		releaseSession(session);
	}
}

// 소켓을 RST로 닫고, 보내지 못한 데이터를 버리고, 슬롯을 돌려준다
void NetworkLib::releaseSession(Session* session)
{
	closeReset(session->mSock);
	session->mSock = INVALID_SOCKET;
	session->mIp = 0;
	session->mPort = 0;
	session->mSendFlag = false;
	session->mSendBuf.clear();
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
	return stat;
}

// 빈 세션 슬롯 하나를 꺼낸다. 가득 차면 nullptr
Session* NetworkLib::allocSession()
{
	std::lock_guard<std::mutex> lock(mFreeLock);
	if (mFreeIdx.empty())
	{
		return nullptr;
	}
	int32_t idx = mFreeIdx.back();
	mFreeIdx.pop_back();
	return &mSessions[idx];
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

void NetworkLib::workerLoop()
{
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
		}
	}
}
