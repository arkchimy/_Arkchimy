#include <chrono>
#include "RsaKeyPool.h"

RsaKeyPool::RsaKeyPool(const int32_t capacity)
	: mCapacity(capacity)
{
	Assert(capacity > 0, "RsaKeyPool: capacity가 0 이하");
	mReady.reserve(static_cast<size_t>(capacity));
}

RsaKeyPool::~RsaKeyPool()
{
	Stop();
}

void RsaKeyPool::Start()
{
	std::lock_guard<std::mutex> lock(mLock);
	Assert(!mbRunning, "RsaKeyPool::Start: 이미 돌고 있음");
	mbRunning = true;
	mThread = std::thread(&RsaKeyPool::runFillLoop, this);
}

void RsaKeyPool::Stop()
{
	{
		std::lock_guard<std::mutex> lock(mLock);
		mbRunning = false;
	}
	mCondition.notify_all();
	if (mThread.joinable())
	{
		mThread.join(); // 만들던 키 쌍 하나(최대 약 0.1초)를 끝낸 뒤 멈춘다
	}
	std::lock_guard<std::mutex> lock(mLock);
	mReady.clear(); // 쓰지 않은 키 쌍도 지운다
}

bool RsaKeyPool::WaitFull(const uint32_t timeoutMs)
{
	std::unique_lock<std::mutex> lock(mLock);
	return mCondition.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this]()
		{
			return static_cast<int32_t>(mReady.size()) >= mCapacity;
		});
}

UniquePtr<RsaKey> RsaKeyPool::Take()
{
	UniquePtr<RsaKey> key;
	{
		std::lock_guard<std::mutex> lock(mLock);
		if (!mReady.empty())
		{
			key = std::move(mReady.back());
			mReady.pop_back();
		}
	}
	mCondition.notify_all(); // 빈자리가 생겼다
	return key;
}

int32_t RsaKeyPool::GetReadyCnt() const
{
	std::lock_guard<std::mutex> lock(mLock);
	return static_cast<int32_t>(mReady.size());
}

void RsaKeyPool::runFillLoop()
{
	std::unique_lock<std::mutex> lock(mLock);
	while (mbRunning)
	{
		if (static_cast<int32_t>(mReady.size()) >= mCapacity)
		{
			mCondition.wait(lock); // 가득 참 - 꺼내거나 멈출 때까지
			continue;
		}
		lock.unlock(); // 만드는 동안(수십 ms) 워커가 꺼낼 수 있게 락 밖에서
		UniquePtr<RsaKey> key = MakeUnique<RsaKey>();
		const bool bOk = key->Generate();
		lock.lock();
		if (bOk)
		{
			mReady.push_back(std::move(key));
			mCondition.notify_all(); // WaitFull
		}
		else
		{
			++mFailCnt;
			mCondition.wait_for(lock, std::chrono::milliseconds(100)); // CNG가 계속 실패해도 CPU를 태우지 않게
		}
	}
}
