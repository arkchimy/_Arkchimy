#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>
#include "LoginCrypto.h"
#include "Memory.h"

// 서버 임시 RSA 키 쌍을 돌려 쓰는 풀 (2026-11-17 사용자 결정 - 로그인 서버의 RsaKeyPool은 접속마다 새 키를 쓰고 버린다)
// 모든 일을 워커가 한다: 빌릴 때 풀이 비었으면 그 자리에서 새로 만들고(약 50ms), 돌려받을 때 maxUseCnt번 쓴 키는 버린다
// 처음에는 비어 있고, 동시에 열린 세션 수만큼까지 저절로 늘어난다(빌려준 키는 그 세션만 쓴다)
// 키 쌍 만들기가 접속 수의 1/maxUseCnt로 줄어드는 대신, 그 키가 새면 그 키를 쓴 세션들(최대 maxUseCnt개)의 녹화본이 함께 풀린다
// 시간 제한은 없다 - 덜 쓴 키는 다음 접속까지 풀에 남는다
class ReusableKeyPool final
{
public:
	explicit ReusableKeyPool(const int32_t maxUseCnt);
	ReusableKeyPool(const ReusableKeyPool&) = delete;
	ReusableKeyPool& operator=(const ReusableKeyPool&) = delete;

	// 풀 맨 위의 키를 빌린다. 비었으면 새로 만든다(이 스레드에서 약 50ms). CNG 실패면 false - 그 접속은 거절한다
	// outUseCnt = 이번 세션까지 이 키를 쓴 횟수(1부터)
	bool Take(UniquePtr<RsaKey>* outKey, int32_t* outUseCnt);
	// 세션이 끝날 때. useCnt가 maxUseCnt보다 적으면 풀 맨 위에 다시 넣고, 아니면 버린다
	void Return(UniquePtr<RsaKey> key, const int32_t useCnt);
	void Clear(); // 풀에 남은 키를 지운다

	int32_t GetReadyCnt() const;
	int32_t GetLentCnt() const;
	int64_t GetCreatedCnt() const
	{
		return mCreatedCnt;
	}
	int64_t GetReplacedCnt() const
	{
		return mReplacedCnt;
	}

private:
	struct PooledKey
	{
		UniquePtr<RsaKey> Key;
		int32_t UseCnt = 0; // 지금까지 빌려 간 세션 수
	};

	const int32_t mMaxUseCnt;
	// 빌려줄 수 있는 키 - 스택(뒤에서 꺼내고 뒤에 넣는다, 2026-11-17 사용자 결정). 최근에 돌아온 키를 먼저 다시 빌려줘
	// 소수의 키만 쓰여 금방 maxUseCnt를 채우고 버려진다 - 지난 세션을 풀 수 있는 키(1번 이상 쓴 키)가 동시 접속 수 정도만 남는다
	// 큐(먼저 돌아온 키부터)는 모든 키를 고르게 써서, 접속이 뜸하면 덜 쓴 키가 많이 오래 남는다
	std::vector<PooledKey> mReady;
	int32_t mLentCnt = 0;                  // 세션이 쓰고 있는 키 - mLock 안에서(통계)
	mutable std::mutex mLock;              // mReady·mLentCnt - 넣고 꺼내는 순간만 잡는다. 키 만들기는 락 밖
	std::atomic<int64_t> mCreatedCnt = 0;  // 새로 만든 키 수(누적)
	std::atomic<int64_t> mReplacedCnt = 0; // maxUseCnt번 써서 버린 키 수(누적)
};
