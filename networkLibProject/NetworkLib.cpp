#include "NetworkLib.h"
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

// 리슨 소켓 생성 → bind → listen
bool NetworkLib::initListenSocket(USHORT port, int backlog)
{
	mListenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (mListenSock == INVALID_SOCKET)
	{
		printf("socket 실패 err=%d\n", WSAGetLastError());
		return false;
	}

	SOCKADDR_IN addr{};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons(port);

	if (bind(mListenSock, reinterpret_cast<SOCKADDR*>(&addr), sizeof(addr)) == SOCKET_ERROR)
	{
		printf("bind 실패 err=%d\n", WSAGetLastError());
		closesocket(mListenSock);
		mListenSock = INVALID_SOCKET;
		return false;
	}

	if (listen(mListenSock, SOMAXCONN_HINT(backlog)) == SOCKET_ERROR)
	{
		printf("listen 실패 err=%d\n", WSAGetLastError());
		closesocket(mListenSock);
		mListenSock = INVALID_SOCKET;
		return false;
	}
	return true;
}

// 안전망: 종료 절차를 건너뛰어도 join 안 된 std::thread 소멸로 terminate 되지 않게
NetworkLib::~NetworkLib()
{
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

void NetworkLib::workerLoop()
{
	while (true)
	{
		DWORD transferred = 0;
		ULONG_PTR key = 0;
		OVERLAPPED* ov = nullptr;
		BOOL ok = GetQueuedCompletionStatus(mIocp, &transferred, &key, &ov, INFINITE);

		if (ov == nullptr)
		{
			// ok == TRUE면 stopWorkers의 종료 신호, FALSE면 IOCP 자체 오류
			if (!ok)
			{
				printf("GQCS 실패 err=%lu\n", GetLastError());
			}
			break;
		}

		// TODO(유저): ov 종류별 분기 (eAccept / eRecv / eSend)
		// ok == FALSE이고 ov != nullptr 이면 그 I/O가 실패한 것 (연결 끊김 등)
	}
}
