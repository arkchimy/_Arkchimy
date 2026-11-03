#pragma once
#include <WinSock2.h>	// timeapi.h보다 먼저 와야 한다(windows.h 정의가 필요하고, WinSock2.h를 windows.h보다 먼저 넣어야 하므로)
#include <timeapi.h>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>
#include "ChatProtocol.h"
#include "Memory.h"
#include "NetworkLib.h"

#pragma comment(lib, "winmm.lib")	// timeGetTime

// 채팅 컨텐츠의 누적 카운터 스냅샷 - 1초 차분으로 초당 값을 만든다
struct ChatStat
{
	int64_t JoinCnt = 0;	// 방 입장 수
	int64_t BroadcastCnt = 0;	// 방에 뿌린 Chat 수
	int64_t DeliverCnt = 0;	// 뿌리면서 부른 SendPacket 수 (= 방 인원 합)
	int64_t LockWaitTicks = 0;	// 방 락을 잡기까지 기다린 시간(QPC 틱 합)
	int64_t LockHoldTicks = 0;	// 방 락을 잡고 있던 시간(QPC 틱 합) - 구성원 전원의 송신 링에 넣는 시간(WSASend는 락 밖)
	int64_t BadCnt = 0;	// 잘못된 컨텐츠 패킷으로 끊은 수
	int64_t IdleCnt = 0;	// 입장 후 무응답 기준 시간 동안 아무것도 안 보내 끊은 수
	int64_t JoinTimeoutCnt = 0;	// 접속 후 입장 기준 시간 안에 Join이 없어 끊은 수
};

// 방 단위 브로드캐스트 채팅 - 모아 보내기를 에코가 아닌 트래픽(내 메시지가 다른 세션들에게 가는 구조)에서 재기 위한 컨텐츠
//
// 순서 보장: 메시지 하나를 방 전원의 송신 링에 넣는 일을 방 락 하나 안에서 끝낸다
// 구성원 목록만 복사하고 락 밖에서 보내면, 워커 둘이 같은 방의 메시지를 동시에 보낼 때 받는 사람마다 순서가 달라진다
// 그래서 방 순번(RoomSeq)도 같은 락 안에서 매기고, 받는 쪽은 +1씩 오는지로 순서와 누락을 검사한다
class ChatServer final : public NetworkLib
{
public:
	// 기반 소멸자에서는 OnRelease(파생 함수)를 부를 수 없으므로 세션 정리를 여기서 끝낸다
	// 감시 스레드를 먼저 멈춘다 - 아래 멤버(슬롯 배열)가 사라지기 전에. Stop 뒤에 불려도 Disconnect는 이미 해제된 세션에서 아무것도 안 한다
	~ChatServer() override
	{
		stopTimeoutWatch();
		Stop();
	}

	// Start 전에 부른다. maxSession은 Start에 넘기는 값과 같아야 한다 - 세션별 방 번호를 슬롯 번호로 찾는다
	void SetChat(const int roomCnt, const int maxSession)
	{
		mRoomCnt = roomCnt;
		mRooms = MakeUniqueArray<Room>(roomCnt);	// Room은 mutex를 가져 이동이 안 되므로 vector 대신 배열
		mRoomOf.assign(maxSession, NO_ROOM);
		mSlotCnt = maxSession;
		mSlotWatches = MakeUniqueArray<SlotWatch>(maxSession);	// atomic이라 이동이 안 되므로 배열
		LARGE_INTEGER freq;
		QueryPerformanceFrequency(&freq);
		mQpcFreq = freq.QuadPart;
	}

	// SetChat 뒤, Start 전에 부른다. 무응답·입장 제한 시간(초), 0이면 그 판정을 끈다. 둘 다 0이면 감시 스레드를 띄우지 않는다
	// 무응답: 입장 후 마지막으로 무엇이든 받은 뒤 idleSec이 지나면 끊는다 - 보낼 게 없는 클라는 Heartbeat로 살아 있음을 알린다
	// 입장: 접속 후 joinSec 안에 Join이 없으면 끊는다 - 입장 전에는 Heartbeat를 보내도 늘어나지 않는다(하트비트만 보내며 자리 차지 방지)
	void SetTimeout(const int idleSec, const int joinSec)
	{
		Assert(!IsRunning());
		Assert(!mWatchThread.joinable());	// 한 번만
		mIdleMs = (idleSec > 0) ? static_cast<uint32_t>(idleSec) * 1000 : 0;
		mJoinMs = (joinSec > 0) ? static_cast<uint32_t>(joinSec) * 1000 : 0;
		if (mIdleMs == 0 && mJoinMs == 0)
		{
			return;
		}
		mWatchStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);	// 수동 리셋 - 소멸자가 켜면 감시가 바로 끝난다
		if (mWatchStopEvent == nullptr)
		{
			printf("[chat] 감시 이벤트 생성 실패 err=%lu - 무응답 판정을 끈다\n", GetLastError());
			return;
		}
		mWatchThread = std::thread(&ChatServer::runTimeoutWatch, this);	// Start 전에 돌아도 슬롯이 비어 있어 하는 일이 없다
	}

	ChatStat GetChatStat() const
	{
		ChatStat stat;
		stat.JoinCnt = mJoinCnt.load();
		stat.BadCnt = mBadCnt.load();
		stat.IdleCnt = mIdleCnt.load();
		stat.JoinTimeoutCnt = mJoinTimeoutCnt.load();
		std::lock_guard<std::mutex> counterLock(mCounterLock);
		for (const UniquePtr<ChatCounter>& counter : mCounters)
		{
			stat.BroadcastCnt += counter->BroadcastCnt.load(std::memory_order_relaxed);
			stat.DeliverCnt += counter->DeliverCnt.load(std::memory_order_relaxed);
			stat.LockWaitTicks += counter->LockWaitTicks.load(std::memory_order_relaxed);
			stat.LockHoldTicks += counter->LockHoldTicks.load(std::memory_order_relaxed);
		}
		return stat;
	}

	int64_t GetQpcFreq() const
	{
		return mQpcFreq;
	}

protected:
	// 같은 세션의 OnAccept/OnRecv/OnRelease는 동시에 불리지 않으므로(라이브러리 콜백 규칙) 자기 슬롯의 방 번호는 락 없이 읽고 쓴다
	void OnAccept(const uint64_t id) override
	{
		mRoomOf[GetIdx(id)] = NO_ROOM;
		SlotWatch& watch = mSlotWatches[GetIdx(id)];
		const uint32_t now = timeGetTime();
		watch.AcceptTime = now;
		watch.LastRecvTime = now;
		watch.bJoined = false;
		watch.ID = id;	// 마지막에 - 감시는 ID가 0이 아닌 슬롯만 보므로, 시각을 다 적은 뒤에 연다
	}

	void OnRecv(const uint64_t id, const char* payload, const int32_t len) override
	{
		mSlotWatches[GetIdx(id)].LastRecvTime = timeGetTime();	// 무엇을 받든 살아 있다는 뜻 - 채팅을 계속 보내는 클라는 하트비트가 필요 없다
		if (len < 1)
		{
			disconnectBad(id);
			return;
		}
		switch (static_cast<eChatType>(payload[0]))
		{
		case eChatType::Heartbeat:
			if (len != sizeof(ChatHeartbeat))
			{
				disconnectBad(id);
			}
			break;	// 시각은 위에서 갱신했다 - 답하지 않는다
		case eChatType::Join:
			if (len != sizeof(ChatJoin))
			{
				disconnectBad(id);
				return;
			}
			join(id, reinterpret_cast<const ChatJoin*>(payload)->Room);
			break;
		case eChatType::Chat:
			if (len < static_cast<int32_t>(sizeof(ChatMsg)) || CHAT_MAX_PAYLOAD < len)
			{
				disconnectBad(id);	// 최대 길이는 채팅 정책 - 라이브러리 한도 안이어도 끊는다
				return;
			}
			broadcast(id, payload, len);
			break;
		default:
			disconnectBad(id);	// 서버가 받을 종류가 아님(JoinOk 포함)
			break;
		}
	}

	void OnRelease(const uint64_t id) override
	{
		mSlotWatches[GetIdx(id)].ID = 0;	// 감시 대상에서 뺀다 - 이 ID는 이제 무효
		int32_t& roomIdx = mRoomOf[GetIdx(id)];
		if (roomIdx != NO_ROOM)
		{
			leave(roomIdx, id);
			roomIdx = NO_ROOM;
		}
	}

private:
	static constexpr int32_t NO_ROOM = -1;

	struct Room
	{
		std::mutex Lock;	// Members, Seq 보호 - 메시지를 전원의 링에 넣는 동안 잡는다
		std::vector<uint64_t> Members;
		uint64_t Seq = 0;	// 마지막으로 매긴 방 순번
	};

	// 슬롯별 무응답·입장 감시 상태 - 쓰는 쪽은 그 세션의 콜백(워커, 한 번에 하나), 읽는 쪽은 감시 스레드라 atomic
	// 시각은 timeGetTime(ms, 32비트) - 경과는 항상 부호 없는 뺄셈으로 구해 49.7일 넘어감에도 맞는다
	struct SlotWatch
	{
		std::atomic<uint64_t> ID = 0;	// 0이면 빈 슬롯(감시 안 함). 감시가 끊을 때 이 값을 0으로 바꿔 한 번만 끊고 센다
		std::atomic<uint32_t> AcceptTime = 0;
		std::atomic<uint32_t> LastRecvTime = 0;
		std::atomic<bool> bJoined = false;	// 입장 전이면 입장 제한, 입장 후면 무응답 기준으로 본다
	};

	// 워커별 카운터 - 브로드캐스트마다 올리므로 워커끼리 같은 캐시 라인을 쓰지 않게 64바이트 정렬, 쓰는 스레드는 하나
	struct alignas(64) ChatCounter
	{
		std::atomic<int64_t> BroadcastCnt = 0;
		std::atomic<int64_t> DeliverCnt = 0;
		std::atomic<int64_t> LockWaitTicks = 0;
		std::atomic<int64_t> LockHoldTicks = 0;
	};

	// 이 스레드의 카운터 - 처음 부를 때 만들어 목록에 올린다 (서버 객체는 프로세스에 하나라는 전제)
	ChatCounter& getMyCounter()
	{
		thread_local ChatCounter* tCounter = nullptr;
		if (tCounter == nullptr)
		{
			UniquePtr<ChatCounter> counter = MakeUnique<ChatCounter>();
			tCounter = counter.get();
			std::lock_guard<std::mutex> counterLock(mCounterLock);
			mCounters.push_back(std::move(counter));
		}
		return *tCounter;
	}

	static int64_t getNowQPC()
	{
		LARGE_INTEGER now;
		QueryPerformanceCounter(&now);
		return now.QuadPart;
	}

	void disconnectBad(const uint64_t id)
	{
		++mBadCnt;
		Disconnect(id);
	}

	// 방을 옮긴다. JoinOk는 방 락 안에서 보낸다 - 그래야 이 세션의 링에서 JoinOk가 이 방의 어떤 Chat보다 앞선다
	void join(const uint64_t id, const uint32_t roomNo)
	{
		if (roomNo >= static_cast<uint32_t>(mRoomCnt))
		{
			disconnectBad(id);
			return;
		}
		int32_t& roomIdx = mRoomOf[GetIdx(id)];
		if (roomIdx != NO_ROOM)
		{
			leave(roomIdx, id);
		}
		const ChatJoinOk joinOk{ eChatType::JoinOk, roomNo };
		{
			Room& target = mRooms[roomNo];
			std::lock_guard<std::mutex> roomLock(target.Lock);
			target.Members.push_back(id);
			SendPacket(id, reinterpret_cast<const char*>(&joinOk), sizeof(joinOk));
		}
		roomIdx = static_cast<int32_t>(roomNo);
		mSlotWatches[GetIdx(id)].bJoined = true;
		++mJoinCnt;
	}

	// 감시 스레드: 1초마다 슬롯을 돌며 입장 제한·무응답 기준을 넘은 세션을 끊는다
	// 워커와 독립이라 트래픽이 없어도 돈다. Disconnect는 아무 스레드에서나 부를 수 있고, ID에 세대가 있어 슬롯이 재사용된 뒤에 불러도 다른 연결을 끊지 않는다
	void runTimeoutWatch()
	{
		SetThreadDescription(GetCurrentThread(), L"ChatTimeoutWatch");
		while (WaitForSingleObject(mWatchStopEvent, 1000) == WAIT_TIMEOUT)
		{
			for (int i = 0; i < mSlotCnt; ++i)
			{
				SlotWatch& watch = mSlotWatches[i];
				uint64_t id = watch.ID.load();
				if (id == 0)
				{
					continue;
				}
				// 슬롯의 시각을 먼저 읽고 now를 그 뒤에 읽는다 - now를 순회 앞에서 한 번만 읽으면, 순회 중 워커가 그보다 나중 시각을 적었을 때
				// now - 시각이 음수가 되고, 부호 없는 32비트라 약 49.7일(42억 ms)로 바뀌어 막 받은 세션을 무응답으로 끊는다(실제로 겪음)
				const uint32_t acceptTime = watch.AcceptTime.load();
				const uint32_t lastRecvTime = watch.LastRecvTime.load();
				const uint32_t now = timeGetTime();
				bool bIdle = false;
				bool bJoinTimeout = false;
				if (!watch.bJoined)
				{
					bJoinTimeout = (mJoinMs > 0) && (now - acceptTime > mJoinMs);
				}
				else
				{
					bIdle = (mIdleMs > 0) && (now - lastRecvTime > mIdleMs);
				}
				// ID를 0으로 바꾸는 데 성공한 경우만 센다 - 끊기는 비동기라 다음 순회에서 같은 세션을 또 세지 않게.
				// 그사이 슬롯이 다른 연결에 넘어갔으면 ID가 달라 실패하고 아무것도 안 한다
				if ((bIdle || bJoinTimeout) && watch.ID.compare_exchange_strong(id, 0))
				{
					if (bIdle)
					{
						++mIdleCnt;
					}
					else
					{
						++mJoinTimeoutCnt;
					}
					Disconnect(id);
				}
			}
		}
	}

	void stopTimeoutWatch()
	{
		if (!mWatchThread.joinable())
		{
			return;
		}
		SetEvent(mWatchStopEvent);	// 1초 대기 중이어도 바로 깨운다
		mWatchThread.join();
		CloseHandle(mWatchStopEvent);
		mWatchStopEvent = nullptr;
	}

	void leave(const int32_t roomIdx, const uint64_t id)
	{
		Room& room = mRooms[roomIdx];
		std::lock_guard<std::mutex> roomLock(room.Lock);
		std::vector<uint64_t>& members = room.Members;
		for (size_t i = 0; i < members.size(); ++i)
		{
			if (members[i] == id)
			{
				members[i] = members.back();	// 순서는 상관없으므로 끝 원소로 메우고 줄인다
				members.pop_back();
				break;
			}
		}
	}

	// 방 순번을 매기고 방 전원(보낸 사람 포함)의 송신 링에 넣는다 - 전부 같은 락 안에서
	// 워커의 수신 처리 안이라 SendPacket은 링에 쌓기만 한다 - WSASend는 수신 처리 끝에서 받는 사람마다 한 번, 방 락 밖에서 일어난다
	void broadcast(const uint64_t id, const char* payload, const int32_t len)
	{
		const int32_t roomIdx = mRoomOf[GetIdx(id)];
		if (roomIdx == NO_ROOM)
		{
			disconnectBad(id);	// 입장 전 Chat
			return;
		}
		char msg[MAX_PAYLOAD_SIZE];
		MemCpy(msg, payload, len);	// RoomSeq를 채워야 하므로 복사 (payload는 라이브러리의 수신 버퍼라 읽기 전용)

		ChatCounter& counter = getMyCounter();
		Room& room = mRooms[roomIdx];
		const int64_t waitStart = getNowQPC();
		int64_t holdStart = 0;
		int64_t deliver = 0;
		{
			std::lock_guard<std::mutex> roomLock(room.Lock);
			holdStart = getNowQPC();
			const uint64_t seq = ++room.Seq;
			MemCpy(msg + offsetof(ChatMsg, RoomSeq), &seq, sizeof(seq));
			for (const uint64_t member : room.Members)
			{
				SendPacket(member, msg, len);	// 넘치면 라이브러리가 그 구성원을 끊는다 - 방에서는 OnRelease 때 빠진다
			}
			deliver = static_cast<int64_t>(room.Members.size());
		}
		const int64_t holdEnd = getNowQPC();
		AddSingleWriter(counter.BroadcastCnt, 1);
		AddSingleWriter(counter.DeliverCnt, deliver);
		AddSingleWriter(counter.LockWaitTicks, holdStart - waitStart);
		AddSingleWriter(counter.LockHoldTicks, holdEnd - holdStart);
	}

	int mRoomCnt = 0;
	UniqueArray<Room> mRooms;
	std::vector<int32_t> mRoomOf;	// 슬롯 번호 → 방 번호 (NO_ROOM이면 입장 전)
	int64_t mQpcFreq = 0;
	mutable std::mutex mCounterLock;	// mCounters 목록 보호 - 카운터 값은 단일 쓰기라 락 없이 올린다
	std::vector<UniquePtr<ChatCounter>> mCounters;
	std::atomic<int64_t> mJoinCnt = 0;	// 입장은 연결마다 한 번이라 공유 카운터로 충분
	std::atomic<int64_t> mBadCnt = 0;

	// 무응답·입장 감시 - SetTimeout에서 시작, 소멸자에서 멈춤
	int mSlotCnt = 0;
	UniqueArray<SlotWatch> mSlotWatches;	// 슬롯 번호 → 감시 상태
	uint32_t mIdleMs = 0;	// 0이면 무응답 판정 끔
	uint32_t mJoinMs = 0;	// 0이면 입장 제한 끔
	std::thread mWatchThread;
	HANDLE mWatchStopEvent = nullptr;
	std::atomic<int64_t> mIdleCnt = 0;	// 감시 스레드만 올린다
	std::atomic<int64_t> mJoinTimeoutCnt = 0;
};
