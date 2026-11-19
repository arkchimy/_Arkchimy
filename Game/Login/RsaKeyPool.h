#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>
#include "LoginCrypto.h"
#include "Memory.h"

// 서버 임시 RSA 키 쌍을 미리 만들어 두는 곳(설계 login_rsa_change.docx 2-7)
// 키 쌍 만들기는 수십 ms라 접속마다 하면 ClientHello만 보내는 접속 폭주로 처리하는 스레드가 막힌다(측정: 접속 200개면 약 10초)
// 백그라운드 스레드 하나가 capacity개까지 채우고, 워커는 꺼내기만 한다
// 키 돌려 쓰기(설계 암호화_구조_결정.docx 2-3, 2026-11-19 사용자 결정): 연결이 끝나면 키를 돌려받아 maxUseCnt번(연결 수)까지 다시 빌려준다
// 키 쌍 만들기가 접속의 1/maxUseCnt로 준다. 대신 그 키가 새면 그 키를 쓴 연결들(최대 maxUseCnt개)의 가입·로그인 요청이 함께 풀린다. maxUseCnt = 1이면 접속마다 새 키
// 돌려받은 키는 맨 위에 넣어 먼저 다시 빌려준다(스택) - 1번 이상 쓴 키가 동시 접속 수 정도만 남는다(RsaEcho ReusableKeyPool과 같은 방식)
class RsaKeyPool final
{
public:
	RsaKeyPool(const int32_t capacity, const int32_t maxUseCnt);
	~RsaKeyPool();
	RsaKeyPool(const RsaKeyPool&) = delete;
	RsaKeyPool& operator=(const RsaKeyPool&) = delete;

	void Start();
	void Stop();
	bool WaitFull(const uint32_t timeoutMs); // 시작할 때 - 처음 접속부터 꺼낼 키가 있게

	// 맨 위의 키를 빌린다. 비었으면 nullptr - 그 접속은 거절한다. outUseCnt = 이번 연결까지 이 키를 쓴 횟수(1부터)
	UniquePtr<RsaKey> Take(int32_t* outUseCnt);
	// 연결이 끝날 때. useCnt가 maxUseCnt보다 적고 풀에 자리가 있으면 맨 위에 다시 넣고, 아니면 지운다. key가 nullptr이면 아무것도 안 한다
	void Return(UniquePtr<RsaKey> key, const int32_t useCnt);
	int32_t GetReadyCnt() const;
	int64_t GetFailCnt() const
	{
		return mFailCnt;
	}
	int64_t GetCreatedCnt() const
	{
		return mCreatedCnt;
	}

private:
	struct PooledKey
	{
		UniquePtr<RsaKey> Key;
		int32_t UseCnt = 0; // 지금까지 빌려 간 연결 수
	};

	void runFillLoop();

	const int32_t mCapacity;
	const int32_t mMaxUseCnt;
	std::vector<PooledKey> mReady;
	mutable std::mutex mLock;
	std::condition_variable mCondition; // 꺼냈거나 멈출 때 채우는 스레드를 깨운다, 채웠을 때 WaitFull을 깨운다
	std::thread mThread;
	bool mbRunning = false;             // mLock 안에서
	std::atomic<int64_t> mFailCnt = 0;  // 키 쌍 만들기 실패(CNG) - 통계
	std::atomic<int64_t> mCreatedCnt = 0; // 만든 키 쌍 수(누적, 시작 때 채운 것 포함) - 통계
};
