#pragma once
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include "Assert.h"

// 일정 간격으로 함수 하나를 부르는 스레드 - 접속 후 첫 패킷 기한 감시처럼 칸을 1초마다 도는 일(운영자 서버의 인증 기한 감시와 같은 모양)
// 워커 밖 스레드이므로 라이브러리 Stop 전에 Stop한다. 함수 안에서는 내부 락을 쥐지 않는다(Stop을 기다리게 하지 않게)
class IntervalThread final
{
public:
	~IntervalThread()
	{
		Stop();
	}

	void Start(const int32_t intervalMs, std::function<void()> tick)
	{
		Assert(!mThread.joinable(), "IntervalThread::Start: 이미 시작함");
		mIntervalMs = intervalMs;
		mTick = std::move(tick);
		mbStopping = false;
		mThread = std::thread(&IntervalThread::run, this);
	}

	// 두 번 불러도 된다
	void Stop()
	{
		{
			std::lock_guard<std::mutex> lock(mLock);
			mbStopping = true;
		}
		mWake.notify_one();
		if (mThread.joinable())
		{
			mThread.join();
		}
	}

private:
	void run()
	{
		std::unique_lock<std::mutex> lock(mLock);
		while (!mbStopping)
		{
			// 간격이 다 되기 전에 Stop이 깨우면 바로 끝낸다. 가짜 깨어남이면 다시 기다린다(간격은 마감 시각 기준)
			const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(mIntervalMs);
			while (!mbStopping && mWake.wait_until(lock, deadline) == std::cv_status::no_timeout)
			{
			}
			if (!mbStopping)
			{
				lock.unlock();
				mTick();
				lock.lock();
			}
		}
	}

	int32_t mIntervalMs = 1000;
	std::function<void()> mTick;
	std::mutex mLock;
	std::condition_variable mWake;
	bool mbStopping = false;
	std::thread mThread;
};
