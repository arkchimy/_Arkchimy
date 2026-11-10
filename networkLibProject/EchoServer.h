#pragma once
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "NetworkLib.h"

// 라이브러리 사용 예제 겸 검증용 컨텐츠 - 받은 패킷을 그대로 돌려보낸다 (EchoClient가 바이트 단위로 검증)
// SetJobTest로 켜면 작업(AcquireSession/PostJobComplete) 시험 모드 - 받은 패킷을 가짜 DB 스레드에 넘겼다가 돌아오면 에코한다
class EchoServer final : public NetworkLib
{
public:
	// 기반 소멸자에서는 OnRelease(파생 함수)를 부를 수 없으므로 세션 정리를 여기서 끝낸다
	// 걸린 작업이 있는 세션은 작업이 끝날 때까지 해제되지 않으므로, 가짜 DB 스레드는 Stop이 끝난 뒤에 멈춘다
	~EchoServer() override
	{
		Stop();
		stopJobThreads();
	}

	// Start 전에만 부른다. delayMs > 0이면 작업 시험 모드 - 가짜 DB가 작업마다 delayMs만큼 잠든다(Sleep이라 실제로는 그보다 길 수 있다)
	void SetJobTest(const int delayMs, const int threadCnt, const int maxSession)
	{
		mJobDelayMs = delayMs;
		if (mJobDelayMs <= 0)
		{
			return;
		}
		for (int i = 0; i < maxSession; ++i)
		{
			mJobSlots.push_back(MakeUnique<JobSlot>());
		}
		for (int i = 0; i < threadCnt; ++i)
		{
			mJobThreads.emplace_back(&EchoServer::runJobThread, this);
		}
	}

protected:
	void OnAccept(const uint64_t id) override
	{
		if (mJobDelayMs <= 0)
		{
			return; // 에코는 연결별 상태가 없다
		}
		JobSlot& slot = *mJobSlots[getIdx(id)];
		std::lock_guard<std::mutex> slotLock(slot.Lock);
		slot.Pending.clear(); // 슬롯은 재사용된다
		slot.bRunning = false;
	}

	// SendPacket이 헤더를 다시 붙이므로 클라이언트에는 받은 패킷과 같은 바이트가 돌아간다
	// 송신 링이 넘치면 SendPacket이 세션을 끊는다 - 여기서 따로 할 일은 없다
	void OnRecv(const uint64_t id, const char* payload, const int32_t len) override
	{
		if (mJobDelayMs <= 0)
		{
			SendPacket(id, payload, len);
			return;
		}
		// 세션마다 작업은 하나씩 - 작업 둘이 동시에 끝나면 워커 둘이 에코 순서를 섞을 수 있다
		JobSlot& slot = *mJobSlots[getIdx(id)];
		std::lock_guard<std::mutex> slotLock(slot.Lock); // 같은 세션의 OnJobComplete와 동시에 불릴 수 있다
		slot.Pending.emplace_back(payload, static_cast<size_t>(len));
		if (!slot.bRunning)
		{
			startJob(id, slot);
		}
	}

	void OnRelease(const uint64_t id) override
	{
		if (mJobDelayMs <= 0)
		{
			return;
		}
		JobSlot& slot = *mJobSlots[getIdx(id)];
		std::lock_guard<std::mutex> slotLock(slot.Lock);
		Assert(!slot.bRunning); // OnRelease는 그 세션의 OnJobComplete가 모두 끝난 뒤에 온다
		slot.Pending.clear();
	}

	// 받은 순서대로 에코한 뒤, 그사이 쌓인 패킷이 있으면 다음 작업을 건다
	void OnJobComplete(const uint64_t id, JobOverlapped* ov) override
	{
		EchoJob* const job = static_cast<EchoJob*>(ov); // 작업 객체가 JobOverlapped를 상속한다
		for (const std::string& packet : job->Packets)
		{
			SendPacket(id, packet.data(), static_cast<int32_t>(packet.size()));
		}
		Delete(job);

		JobSlot& slot = *mJobSlots[getIdx(id)];
		std::lock_guard<std::mutex> slotLock(slot.Lock);
		slot.bRunning = false;
		if (!slot.Pending.empty())
		{
			startJob(id, slot);
		}
	}

private:
	struct JobSlot
	{
		std::mutex Lock;                  // Pending, bRunning 보호 - OnRecv와 OnJobComplete가 함께 쓴다
		std::vector<std::string> Pending; // 작업을 기다리는 패킷 (받은 순서)
		bool bRunning = false;            // 이 세션의 작업이 가짜 DB에 가 있는가
	};

	// 작업 객체 - OVERLAPPED를 상속해 그대로 PostJobComplete에 넘긴다. OnJobComplete에서 지운다(그 전에 지우면 IOCP 큐가 해제된 주소를 든다)
	struct EchoJob : JobOverlapped
	{
		uint64_t SessionID = 0;
		std::vector<std::string> Packets;
	};

	// 세션 ID의 아래 32비트 = 라이브러리의 슬롯 번호 - 슬롯별 배열의 인덱스
	static uint32_t getIdx(const uint64_t id)
	{
		return static_cast<uint32_t>(id);
	}

	// slot.Lock을 잡은 채로 부른다 - 쌓인 패킷을 작업 하나로 묶어 가짜 DB에 넘긴다
	void startJob(const uint64_t id, JobSlot& slot)
	{
		if (!AcquireSession(id))
		{
			slot.Pending.clear(); // 이 세션은 해제됐거나 바뀌었다 - 콜백 안이라 보통은 실패하지 않는다(세션마다 작업 하나라 개수 한도에도 안 닿는다)
			return;
		}
		EchoJob* const job = New<EchoJob>();
		job->SessionID = id;
		job->Packets.swap(slot.Pending);
		slot.bRunning = true;
		{
			std::lock_guard<std::mutex> queueLock(mJobQueueLock); // 순서: 슬롯 락 → 큐 락 (가짜 DB는 큐 락만 잡는다)
			mJobQueue.push_back(job);
		}
		mJobCv.notify_one();
	}

	// 가짜 DB - 작업 하나마다 잠들었다가 결과를 워커로 돌려보낸다. 멈춤 요청이 와도 큐에 남은 작업은 마저 끝낸다
	void runJobThread()
	{
		while (true)
		{
			EchoJob* job = nullptr;
			{
				std::unique_lock<std::mutex> queueLock(mJobQueueLock);
				mJobCv.wait(queueLock, [this]()
					{
						return mbJobStop || !mJobQueue.empty();
					});
				if (mJobQueue.empty())
				{
					return; // 멈춤 요청 + 남은 작업 없음
				}
				job = mJobQueue.front();
				mJobQueue.pop_front();
			}
			Sleep(static_cast<DWORD>(mJobDelayMs));
			PostJobComplete(job->SessionID, job);
		}
	}

	void stopJobThreads()
	{
		{
			std::lock_guard<std::mutex> queueLock(mJobQueueLock);
			mbJobStop = true;
		}
		mJobCv.notify_all();
		for (std::thread& thread : mJobThreads)
		{
			thread.join();
		}
		mJobThreads.clear();
	}

	int mJobDelayMs = 0; // 0이면 작업 시험 모드 끔
	std::vector<UniquePtr<JobSlot>> mJobSlots;
	std::vector<std::thread> mJobThreads;
	std::mutex mJobQueueLock; // mJobQueue, mbJobStop 보호
	std::condition_variable mJobCv;
	std::deque<EchoJob*> mJobQueue;
	bool mbJobStop = false;
};
