#include <utility>
#include "TurnTimer.h"
#include "Assert.h"

TurnTimer::~TurnTimer()
{
	Stop();
}

void TurnTimer::Start(ExpireFunc onExpire)
{
	Assert(!mThread.joinable() && onExpire, "TurnTimer::Start: 이미 시작했거나 콜백이 없음");
	mOnExpire = std::move(onExpire);
	mbStopping = false;
	mThread = std::thread(&TurnTimer::run, this);
}

void TurnTimer::Stop()
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

void TurnTimer::Add(const Clock::time_point deadline, const uint64_t partyID, const uint32_t turnNo)
{
	bool bEarliest = false;
	{
		std::lock_guard<std::mutex> lock(mLock);
		bEarliest = mQueue.empty() || deadline < mQueue.top().Deadline;
		mQueue.push(Entry{ deadline, partyID, turnNo });
	}
	if (bEarliest)
	{
		mWake.notify_one(); // 자고 있는 스레드가 더 늦은 마감을 기다리고 있을 수 있다
	}
}

void TurnTimer::run()
{
	std::unique_lock<std::mutex> lock(mLock);
	while (!mbStopping)
	{
		if (mQueue.empty())
		{
			mWake.wait(lock);
			continue;
		}
		const Entry earliest = mQueue.top();
		if (Clock::now() < earliest.Deadline)
		{
			mWake.wait_until(lock, earliest.Deadline); // 더 이른 마감이 들어오면 Add가 깨운다
			continue;
		}
		mQueue.pop();
		lock.unlock(); // 콜백은 대기열 락 밖에서 - 콜백이 파티 락을 잡고, 그 안에서 Add(대기열 락)를 부른다
		mOnExpire(earliest.PartyID, earliest.TurnNo);
		lock.lock();
	}
}
