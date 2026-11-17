#include "ReusableKeyPool.h"

ReusableKeyPool::ReusableKeyPool(const int32_t maxUseCnt)
	: mMaxUseCnt(maxUseCnt)
{
	Assert(maxUseCnt > 0, "ReusableKeyPool: maxUseCnt가 0 이하");
}

bool ReusableKeyPool::Take(UniquePtr<RsaKey>* outKey, int32_t* outUseCnt)
{
	Assert(outKey != nullptr && outUseCnt != nullptr, "ReusableKeyPool::Take: out 매개변수가 null");
	{
		std::lock_guard<std::mutex> lock(mLock);
		if (!mReady.empty())
		{
			PooledKey& pooled = mReady.back();
			*outKey = std::move(pooled.Key);
			*outUseCnt = pooled.UseCnt + 1;
			mReady.pop_back();
			++mLentCnt;
			return true;
		}
	}
	// 풀이 비었다 - 새로 만든다. 락 밖에서(약 50ms) - 그동안 다른 워커가 꺼내고 돌려줄 수 있다
	UniquePtr<RsaKey> key = MakeUnique<RsaKey>();
	if (!key->Generate())
	{
		return false;
	}
	++mCreatedCnt;
	*outKey = std::move(key);
	*outUseCnt = 1;
	std::lock_guard<std::mutex> lock(mLock);
	++mLentCnt;
	return true;
}

void ReusableKeyPool::Return(UniquePtr<RsaKey> key, const int32_t useCnt)
{
	std::lock_guard<std::mutex> lock(mLock);
	--mLentCnt;
	if (useCnt >= mMaxUseCnt)
	{
		++mReplacedCnt;
		return; // key가 여기서 없어지며 CNG 키도 지워진다 - 빈 자리는 다음 Take가 비었을 때 새로 만든다
	}
	PooledKey pooled;
	pooled.Key = std::move(key);
	pooled.UseCnt = useCnt;
	mReady.push_back(std::move(pooled)); // 스택 맨 위 - 다음 접속이 먼저 쓴다
}

void ReusableKeyPool::Clear()
{
	std::lock_guard<std::mutex> lock(mLock);
	mReady.clear();
}

int32_t ReusableKeyPool::GetReadyCnt() const
{
	std::lock_guard<std::mutex> lock(mLock);
	return static_cast<int32_t>(mReady.size());
}

int32_t ReusableKeyPool::GetLentCnt() const
{
	std::lock_guard<std::mutex> lock(mLock);
	return mLentCnt;
}
