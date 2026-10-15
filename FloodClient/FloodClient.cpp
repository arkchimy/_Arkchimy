#include "FloodClient.h"
#include <cstdio>

namespace
{
	// RST로 즉시 닫아 TIME_WAIT 없이 포트를 바로 회수한다 (루프백 고속 재사용)
	void closeReset(SOCKET s)
	{
		LINGER linger{};
		linger.l_onoff = 1;
		linger.l_linger = 0;
		setsockopt(s, SOL_SOCKET, SO_LINGER, reinterpret_cast<const char*>(&linger), sizeof(linger));
		closesocket(s);
	}
}

FloodClient::~FloodClient()
{
	stop();
}

void FloodClient::stop()
{
	if (!mRunning)
	{
		return;
	}

	mRunning = false;
	for (size_t i = 0; i < mWorkers.size(); ++i)
	{
		PostQueuedCompletionStatus(mIocp, 0, 0, nullptr);	// 워커당 종료 신호 하나씩
	}
	for (std::thread& t : mWorkers)
	{
		t.join();
	}
	mWorkers.clear();
	mStatThread.join();

	// 아직 완료 전인 슬롯의 소켓 정리
	for (ConnectOv& ov : mConnectOvs)
	{
		if (ov.mSocket != INVALID_SOCKET)
		{
			closesocket(ov.mSocket);
		}
	}
	CloseHandle(mIocp);
	mIocp = nullptr;
}

// ConnectEx는 Mswsock의 확장 함수라 런타임에 포인터를 받아와야 한다
bool FloodClient::loadConnectEx()
{
	SOCKET tmp = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (tmp == INVALID_SOCKET)
	{
		return false;
	}

	GUID guid = WSAID_CONNECTEX;
	DWORD bytes = 0;
	int ret = WSAIoctl(tmp, SIO_GET_EXTENSION_FUNCTION_POINTER,
		&guid, sizeof(guid), &mConnectEx, sizeof(mConnectEx), &bytes, nullptr, nullptr);

	closesocket(tmp);
	if (ret == SOCKET_ERROR || mConnectEx == nullptr)
	{
		printf("ConnectEx 포인터 획득 실패 err=%d\n", WSAGetLastError());
		return false;
	}
	return true;
}

bool FloodClient::init(const std::string& ip, USHORT port, int slotCnt, int workerCnt)
{
	mTargetAddr.sin_family = AF_INET;
	mTargetAddr.sin_port = htons(port);
	if (inet_pton(AF_INET, ip.c_str(), &mTargetAddr.sin_addr) != 1)
	{
		printf("inet_pton 실패: %s\n", ip.c_str());
		return false;
	}

	if (!loadConnectEx())
	{
		return false;
	}

	mIocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);
	if (mIocp == nullptr)
	{
		printf("CreateIoCompletionPort 실패 err=%lu\n", GetLastError());
		return false;
	}

	mRunning = true;
	mSlotCnt = slotCnt;
	mWorkerCnt = workerCnt;
	mSuccess.resize(workerCnt);
	mRefused.resize(workerCnt);
	mOther.resize(workerCnt);
	for (int i = 0; i < workerCnt; ++i)
	{
		mWorkers.emplace_back(&FloodClient::workerLoop, this);
	}
	mStatThread = std::thread(&FloodClient::statLoop, this);

	mConnectOvs.resize(slotCnt);
	for (int i = 0; i < slotCnt; ++i)
	{
		postConnect(&mConnectOvs[i]);	// 초기에 slotCnt개를 동시에 걸어둔다
	}
	return true;
}

// 소켓 생성 → bind(포트 0) → IOCP 등록 → ConnectEx.
// 실패하면 슬롯을 버리지 않고 running 동안 성공할 때까지 재시도 → 동시 걸림 수를 SLOT_CNT로 유지
bool FloodClient::postConnect(ConnectOv* ov)
{
	while (mRunning)
	{
		SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (s == INVALID_SOCKET)
		{
			continue;	// 핸들/포트 고갈 - 다른 슬롯이 닫히며 회수되면 성공
		}

		// ConnectEx는 미리 bind된 소켓을 요구한다. 포트 0 → OS가 빈 포트 할당
		sockaddr_in local{};
		local.sin_family = AF_INET;
		local.sin_addr.s_addr = htonl(INADDR_ANY);
		local.sin_port = 0;
		if (bind(s, reinterpret_cast<sockaddr*>(&local), sizeof(local)) == SOCKET_ERROR)
		{
			closesocket(s);
			continue;	// 포트 고갈(10048) 등 - 재시도
		}

		if (CreateIoCompletionPort(reinterpret_cast<HANDLE>(s), mIocp, 0, 0) == nullptr)
		{
			closesocket(s);
			continue;
		}

		ZeroMemory(static_cast<OVERLAPPED*>(ov), sizeof(OVERLAPPED));
		ov->mSocket = s;

		DWORD sent = 0;
		BOOL ok = mConnectEx(s, reinterpret_cast<sockaddr*>(&mTargetAddr), sizeof(mTargetAddr),
			nullptr, 0, &sent, ov);

		if (ok || WSAGetLastError() == WSA_IO_PENDING)
		{
			return true;	// 완료는 workerLoop에서 받는다
		}

		// 즉시 실패 - 완료통지가 안 오므로 여기서 진단만 남기고 소켓 닫고 재시도
		mLastSyncErr.store(WSAGetLastError(), std::memory_order_relaxed);
		mSyncFail.fetch_add(1, std::memory_order_relaxed);
		closesocket(s);
	}
	return false;
}

void FloodClient::workerLoop()
{
	long long idx = mCntIdx++;
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

		ConnectOv* ov = static_cast<ConnectOv*>(overlapped);

		if (ok)
		{
			++mSuccess[idx];
			// ConnectEx로 연결된 소켓은 이 옵션을 설정해야 일반 connect 소켓처럼 동작한다.
			// 안 하면 LINGER/closesocket 등이 온전치 않아 RST 대신 TIME_WAIT로 빠질 수 있다
			setsockopt(ov->mSocket, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, nullptr, 0);
		}
		else
		{
			// 소켓 비동기 I/O의 실패 원인은 GQCS 직후 GetLastError가 아니라
			// WSAGetOverlappedResult로 얻어야 올바른 WSA 코드(10061 등)가 나온다
			DWORD flags = 0, bytes = 0;
			WSAGetOverlappedResult(ov->mSocket, ov, &bytes, FALSE, &flags);
			int werr = WSAGetLastError();
			if (werr == WSAECONNREFUSED)
			{
				++mRefused[idx];	// backlog 넘침 또는 리슨 소켓 없음
			}
			else
			{
				++mOther[idx];	// 그 외 (포트/자원 관련 등)
				mLastOtherErr.store(static_cast<long>(werr), std::memory_order_relaxed);	// 진단용 마지막 코드
			}
		}

		closeReset(ov->mSocket);
		ov->mSocket = INVALID_SOCKET;
		postConnect(ov);	// 같은 슬롯으로 즉시 재등록
	}
}

void FloodClient::statLoop()
{
	long long lastSuccess = 0, lastRefused = 0, lastOther = 0;
	long long elapsedSec = 0;
	bool started = false;
	while (mRunning)
	{
		Sleep(1000);

		long long success = 0, refused = 0, other = 0;
		for (int i = 0; i < mWorkerCnt; ++i)
		{
			success += mSuccess[i];
			refused += mRefused[i];
			other += mOther[i];
		}

		started = started || (success + refused + other > 0);
		if (started)
		{
			++elapsedSec;
		}
		long long avgSuccess = started ? success / elapsedSec : 0;

		long long syncFail = mSyncFail.load(std::memory_order_relaxed);
		printf("[FloodClient] 성공tps=%lld 거절tps=%lld 기타tps=%lld(err=%ld) | 동기실패누적=%lld(err=%ld) | avg성공=%lld | 누적(성공=%lld 거절=%lld 기타=%lld)\n",
			success - lastSuccess, refused - lastRefused, other - lastOther,
			mLastOtherErr.load(std::memory_order_relaxed),
			syncFail, mLastSyncErr.load(std::memory_order_relaxed),
			avgSuccess, success, refused, other);

		lastSuccess = success;
		lastRefused = refused;
		lastOther = other;
	}
}
