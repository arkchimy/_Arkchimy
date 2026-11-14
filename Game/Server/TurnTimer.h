#pragma once
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

// 턴 마감 시각 순 대기열 - 스레드 하나가 가장 이른 마감까지 자다가 깨어나 콜백을 부른다(설계 3-9-2)
// 대기열 락은 넣고 뺄 때만 잠깐 잡고, 콜백은 대기열 락을 놓은 뒤 부른다(콜백이 파티 락을 잡으므로 - 락 순서 파티 → 대기열)
// 턴이 먼저 넘어가도 옛 항목은 지우지 않는다 - 깨어났을 때 턴 번호가 달라 콜백이 아무 일도 하지 않는다
class TurnTimer final
{
public:
	using Clock = std::chrono::steady_clock;
	using ExpireFunc = std::function<void(const uint64_t partyID, const uint32_t turnNo)>;

	~TurnTimer();

	void Start(ExpireFunc onExpire);
	void Stop(); // 스레드를 깨워 끝낸다 - GameServer::Shutdown에서 라이브러리 Stop()보다 먼저(워커 밖 스레드는 Stop 전에 멈춘다). 두 번 불러도 된다
	void Add(const Clock::time_point deadline, const uint64_t partyID, const uint32_t turnNo); // 파티 락 안에서 불러도 된다

private:
	struct Entry
	{
		Clock::time_point Deadline;
		uint64_t PartyID;
		uint32_t TurnNo;
	};

	class LaterFirst final // priority_queue가 가장 이른 마감을 맨 위에 두도록
	{
	public:
		bool operator()(const Entry& left, const Entry& right) const
		{
			return left.Deadline > right.Deadline;
		}
	};

	void run();

	std::mutex mLock;              // 대기열 락
	std::condition_variable mWake; // 더 이른 마감이 들어오거나 멈출 때 깨운다
	std::priority_queue<Entry, std::vector<Entry>, LaterFirst> mQueue;
	bool mbStopping = false;
	ExpireFunc mOnExpire;
	std::thread mThread;
};
