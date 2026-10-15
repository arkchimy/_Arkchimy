#include "AcceptExServer.h"
#include "ListenSocket.h"
#include "SocketUtil.h"
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
    mAcceptCount.resize(workerCnt);
    mWorkerCnt = workerCnt;
    for (int i = 0; i < workerCnt; ++i)
    {
        mWorkers.emplace_back(&AcceptExServer::workerLoop, this);
    }
    mStatThread = std::thread(&AcceptExServer::statLoop, this);

    mAcceptOvs.resize(workerCnt);
    for (int i = 0; i < workerCnt; ++i)
    {
        postAccept(&mAcceptOvs[i]); // 초기에 워커 수만큼 미리 걸어둔다
    }
    return true;
}

// 실패해도 슬롯을 버리지 않고 성공할 때까지 재시도 - 걸려 있는 AcceptEx 수를 고정으로 유지
// false는 종료 중(mRunning == false)일 때만 반환
bool AcceptExServer::postAccept(AcceptOv *ov)
{
    while (mRunning)
    {
        SOCKET acceptSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (acceptSocket == INVALID_SOCKET)
        {
            continue;
        }

        ZeroMemory(static_cast<OVERLAPPED *>(ov), sizeof(OVERLAPPED)); // 재사용 전 OVERLAPPED 필드 초기화
        ov->mAcceptSocket = acceptSocket;

        DWORD bytesReceived = 0;
        BOOL ok = AcceptEx(mListenSock, acceptSocket, ov->mBuffer, 0,
                           sizeof(SOCKADDR_IN) + 16, sizeof(SOCKADDR_IN) + 16, &bytesReceived, ov);

        if (ok || WSAGetLastError() == WSA_IO_PENDING)
        {
            return true;
        }
        closesocket(acceptSocket);
    }
    return false;
}

void AcceptExServer::workerLoop()
{
    long long idx = mCntIdx++;
    while (true)
    {
        DWORD transferred = 0;
        ULONG_PTR key = 0;
        OVERLAPPED *overlapped = nullptr;
        BOOL ok = GetQueuedCompletionStatus(mIocp, &transferred, &key, &overlapped, INFINITE);

        if (overlapped == nullptr)
        {
            break; // stop()의 종료 신호
        }

        AcceptOv *ov = static_cast<AcceptOv *>(overlapped);
        ++mAcceptCount[idx]; // 성공/실패 상관없이 완료통지 처리 수를 센다

        closeReset(ov->mAcceptSocket);
        postAccept(ov); // 같은 슬롯으로 바로 재등록
    }
}

void AcceptExServer::statLoop()
{
    long long last = 0;
    long long elapsedSec = 0;
    bool started = false;
    while (mRunning)
    {
        Sleep(1000);
        long long now = 0;
        for (int idx = 0; idx < mWorkerCnt; ++idx)
        {
            now += mAcceptCount[idx];
        }

        started = started || (now > 0); // 첫 이벤트가 발생한 뒤부터만 경과 시간을 셈
        if (started)
        {
            ++elapsedSec;
        }
        long long avg = started ? now / elapsedSec : 0;
        printf("[AcceptExServer] 완료통지 tps=%lld avg=%lld total=%lld\n", now - last, avg, now);
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
    for (std::thread &t : mWorkers)
    {
        t.join();
    }
    mWorkers.clear();
    mStatThread.join(); // statLoop는 Sleep(1000) 끝나는 시점에 mRunning 보고 빠져나감

    closesocket(mListenSock);
    CloseHandle(mIocp);
}

AcceptExServer::~AcceptExServer()
{
    stop();
}
