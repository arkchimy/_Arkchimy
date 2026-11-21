#include "RsaKeyPool.h"

RsaKeyPool::RsaKeyPool(const int32_t capacity, const int32_t maxUseCnt)
	: mCapacity(capacity)
	, mMaxUseCnt(maxUseCnt)
{
	Assert(capacity > 0 && maxUseCnt > 0, "RsaKeyPool: capacity 또는 maxUseCnt가 0 이하");
}

bool RsaKeyPool::Fill()
{
	bool bAllOk = true;
	for (int32_t i = 0; i < mCapacity && bAllOk; ++i)
	{
		bAllOk = pushNewKey();
	}
	return bAllOk;
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
			PooledKey& front = mReady.front(); // 가장 오래 기다린 키 - 방금 돌려받은 키는 맨 뒤에 있다
			key = std::move(front.Key);
			*outUseCnt = front.UseCnt + 1;
			mReady.pop_front();
		}
	}
	return key;
}

void RsaKeyPool::Return(UniquePtr<RsaKey> key, const int32_t useCnt)
{
	if (key == nullptr)
	{
		return;
	}
	if (useCnt >= mMaxUseCnt)
	{
		key.reset(); // 다 쓴 키 - 지운다
		if (!mbRefillStopped)
		{
			pushNewKey(); // 그 자리를 돌려준 스레드가 채운다(약 55ms 동안 이 워커는 다른 연결을 처리하지 못한다)
		}
		return;
	}
	std::lock_guard<std::mutex> lock(mLock);
	if (static_cast<int32_t>(mReady.size()) < mCapacity) // 풀 + 빌려 간 키가 늘 capacity개라 가득 찰 일은 없다 - 혹시 차 있으면 지운다
	{
		PooledKey pooled;
		pooled.Key = std::move(key);
		pooled.UseCnt = useCnt;
		mReady.push_back(std::move(pooled)); // 맨 뒤 - 앞의 키가 다 나간 뒤에 다시 빌려준다
	}
}

void RsaKeyPool::StopRefill()
{
	mbRefillStopped = true;
}

int32_t RsaKeyPool::GetReadyCnt() const
{
	std::lock_guard<std::mutex> lock(mLock);
	return static_cast<int32_t>(mReady.size());
}

bool RsaKeyPool::pushNewKey()
{
	UniquePtr<RsaKey> key = MakeUnique<RsaKey>();
	const bool bOk = key->Generate(); // 수십 ms - 락 밖에서(그동안 다른 워커가 꺼내고 돌려줄 수 있게)
	if (bOk)
	{
		++mCreatedCnt;
		std::lock_guard<std::mutex> lock(mLock);
		if (static_cast<int32_t>(mReady.size()) < mCapacity)
		{
			PooledKey pooled;
			pooled.Key = std::move(key);
			mReady.push_back(std::move(pooled));
		}
	}
	else
	{
		++mFailCnt; // CNG 실패 - 그 자리는 비어 풀이 하나 작아진다(통계로 보인다)
	}
	return bOk;
}
