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
// 키 쌍 만들기는 수십 ms라 접속마다 하면 ClientHello만 보내는 접속 폭주로 인증 스레드가 막힌다(측정: 접속 200개면 약 10초)
// 백그라운드 스레드 하나가 capacity개까지 채우고, 워커는 꺼내기만 한다. 꺼낸 키는 그 연결에서만 쓰고 지운다 - 접속마다 다른 키라 전방 비밀성은 그대로
class RsaKeyPool final
{
public:
	explicit RsaKeyPool(const int32_t capacity);
	~RsaKeyPool();
	RsaKeyPool(const RsaKeyPool&) = delete;
	RsaKeyPool& operator=(const RsaKeyPool&) = delete;

	void Start();
	void Stop();
	bool WaitFull(const uint32_t timeoutMs); // 시작할 때 - 처음 접속부터 꺼낼 키가 있게

	UniquePtr<RsaKey> Take(); // 비었으면 nullptr - 그 접속은 거절한다
	int32_t GetReadyCnt() const;
	int64_t GetFailCnt() const
	{
		return mFailCnt;
	}

private:
	void runFillLoop();

	const int32_t mCapacity;
	std::vector<UniquePtr<RsaKey>> mReady;
	mutable std::mutex mLock;
	std::condition_variable mCondition; // 꺼냈거나 멈출 때 채우는 스레드를 깨운다, 채웠을 때 WaitFull을 깨운다
	std::thread mThread;
	bool mbRunning = false;             // mLock 안에서
	std::atomic<int64_t> mFailCnt = 0;  // 키 쌍 만들기 실패(CNG) - 통계
};
