#include <chrono>
#include "RsaKeyPool.h"

RsaKeyPool::RsaKeyPool(const int32_t capacity, const int32_t maxUseCnt)
	: mCapacity(capacity)
	, mMaxUseCnt(maxUseCnt)
{
	Assert(capacity > 0 && maxUseCnt > 0, "RsaKeyPool: capacity 또는 maxUseCnt가 0 이하");
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

UniquePtr<RsaKey> RsaKeyPool::Take(int32_t* outUseCnt)
{
	Assert(outUseCnt != nullptr, "RsaKeyPool::Take: outUseCnt가 null");
	UniquePtr<RsaKey> key;
	*outUseCnt = 0;
	{
		std::lock_guard<std::mutex> lock(mLock);
		if (!mReady.empty())
		{
			PooledKey& top = mReady.back();
			key = std::move(top.Key);
			*outUseCnt = top.UseCnt + 1;
			mReady.pop_back();
		}
	}
	mCondition.notify_all(); // 빈자리가 생겼다
	return key;
}

void RsaKeyPool::Return(UniquePtr<RsaKey> key, const int32_t useCnt)
{
	if (key == nullptr || useCnt >= mMaxUseCnt)
	{
		return; // 다 쓴 키 - 여기서 지운다(UniquePtr). 빈자리는 채우는 스레드가 Take 때 이미 알고 채운다
	}
	std::lock_guard<std::mutex> lock(mLock);
	if (mbRunning && static_cast<int32_t>(mReady.size()) < mCapacity)
	{
		PooledKey pooled;
		pooled.Key = std::move(key);
		pooled.UseCnt = useCnt;
		mReady.push_back(std::move(pooled)); // 맨 위 - 먼저 다시 빌려준다
	}
	// 풀이 가득 찼으면(채우는 스레드가 이미 새 키로 채움) 돌려받은 키는 락을 놓은 뒤 지운다
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
			++mCreatedCnt;
			PooledKey pooled;
			pooled.Key = std::move(key);
			mReady.push_back(std::move(pooled));
			mCondition.notify_all(); // WaitFull
		}
		else
		{
			++mFailCnt;
			mCondition.wait_for(lock, std::chrono::milliseconds(100)); // CNG가 계속 실패해도 CPU를 태우지 않게
		}
	}
}
