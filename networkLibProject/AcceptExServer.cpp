#include "AcceptExServer.h"
#include "ListenSocket.h"
#include <cstdio>

bool AcceptExServer::init(USHORT port, int backlog, int workerCnt)
{
	mListenSock = createListenSocket(port, backlog);
	if (mListenSock == INVALID_SOCKET)
	{
		return false;
	}

	mIocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);
	if (mIocp == nullptr)
	{
		return false;
	}

	// 리슨 소켓을 IOCP에 등록해야 AcceptEx 완료가 GQCS로 들어온다
	if (CreateIoCompletionPort(reinterpret_cast<HANDLE>(mListenSock), mIocp, 0, 0) == nullptr)
	{
		return false;
	}

	mRunning = true;
	for (int i = 0; i < workerCnt; ++i)
	{
		mWorkers.emplace_back(&AcceptExServer::workerLoop, this);
	}
	mStatThread = std::thread(&AcceptExServer::statLoop, this);

	mAcceptOvs.resize(workerCnt);
	for (int i = 0; i < workerCnt; ++i)
	{
		postAccept(&mAcceptOvs[i]);	// 초기에 워커 수만큼 미리 걸어둔다
	}
	return true;
}

bool AcceptExServer::postAccept(AcceptOv* ov)
{
	SOCKET acceptSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (acceptSocket == INVALID_SOCKET)
	{
		return false;
	}

	ZeroMemory(static_cast<OVERLAPPED*>(ov), sizeof(OVERLAPPED));	// 재사용 전 OVERLAPPED 필드 초기화
	ov->mAcceptSocket = acceptSocket;

	DWORD bytesReceived = 0;
	BOOL ok = AcceptEx(mListenSock, acceptSocket, ov->mBuffer, 0,
		sizeof(SOCKADDR_IN) + 16, sizeof(SOCKADDR_IN) + 16, &bytesReceived, ov);

	if (!ok && WSAGetLastError() != WSA_IO_PENDING)
	{
		closesocket(acceptSocket);
		return false;
	}
	return true;
}

void AcceptExServer::workerLoop()
{
	while (true)
	{
		DWORD transferred = 0;
		ULONG_PTR key = 0;
		OVERLAPPED* overlapped = nullptr;
		BOOL ok = GetQueuedCompletionStatus(mIocp, &transferred, &key, &overlapped, INFINITE);

		if (overlapped == nullptr)
		{
			break;	// stop()의 종료 신호
		}

		AcceptOv* ov = static_cast<AcceptOv*>(overlapped);
		closesocket(ov->mAcceptSocket);
		++mAcceptCount;

		postAccept(ov);	// 같은 슬롯으로 바로 재등록
	}
}

void AcceptExServer::statLoop()
{
	long long last = 0;
	while (mRunning)
	{
		Sleep(1000);
		long long now = mAcceptCount.load();
		printf("[AcceptExServer] TPS=%lld\n", now - last);
		last = now;
	}
}

void AcceptExServer::stop()
{
	if (!mRunning)
	{
		return;
	}

	mRunning = false;
	for (size_t i = 0; i < mWorkers.size(); ++i)
	{
		PostQueuedCompletionStatus(mIocp, 0, 0, nullptr);
	}
	for (std::thread& t : mWorkers)
	{
		t.join();
	}
	mWorkers.clear();
	mStatThread.join();	// statLoop는 Sleep(1000) 끝나는 시점에 mRunning 보고 빠져나감

	closesocket(mListenSock);
	CloseHandle(mIocp);
}

AcceptExServer::~AcceptExServer()
{
	stop();
}
