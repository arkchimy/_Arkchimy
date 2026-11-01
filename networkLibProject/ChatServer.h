#pragma once
#include "NetworkLib.h"
#include "ChatProtocol.h"
#include <cstddef>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

// 채팅 컨텐츠의 누적 카운터 스냅샷 - 1초 차분으로 초당 값을 만든다
struct ChatStat
{
	int64_t joinCnt = 0;	// 방 입장 수
	int64_t broadcastCnt = 0;	// 방에 뿌린 Chat 수
	int64_t deliverCnt = 0;	// 뿌리면서 부른 SendPacket 수 (= 방 인원 합)
	int64_t lockWaitTicks = 0;	// 방 락을 잡기까지 기다린 시간(QPC 틱 합)
	int64_t lockHoldTicks = 0;	// 방 락을 잡고 있던 시간(QPC 틱 합) - 지금 구조에서는 다른 세션 송신의 WSASend까지 들어간다
	int64_t badCnt = 0;	// 잘못된 컨텐츠 패킷으로 끊은 수
};

// 방 단위 브로드캐스트 채팅 - 모아 보내기를 에코가 아닌 트래픽(내 메시지가 다른 세션들에게 가는 구조)에서 재기 위한 컨텐츠
//
// 순서 보장: 메시지 하나를 방 전원의 송신 링에 넣는 일을 방 락 하나 안에서 끝낸다
// 구성원 목록만 복사하고 락 밖에서 보내면, 워커 둘이 같은 방의 메시지를 동시에 보낼 때 받는 사람마다 순서가 달라진다
// 그래서 방 순번(roomSeq)도 같은 락 안에서 매기고, 받는 쪽은 +1씩 오는지로 순서와 누락을 검사한다
class ChatServer : public NetworkLib
{
public:
	// 기반 소멸자에서는 OnRelease(파생 함수)를 부를 수 없으므로 세션 정리를 여기서 끝낸다
	~ChatServer() override
	{
		stop();
	}

	// start 전에 부른다. maxSession은 start에 넘기는 값과 같아야 한다 - 세션별 방 번호를 슬롯 번호로 찾는다
	void setChat(int roomCnt, int maxSession)
	{
		mRoomCnt = roomCnt;
		mRooms = std::make_unique<Room[]>(roomCnt);	// Room은 mutex를 가져 이동이 안 되므로 vector 대신 배열
		mRoomOf.assign(maxSession, NO_ROOM);
		LARGE_INTEGER freq;
		QueryPerformanceFrequency(&freq);
		mQpcFreq = freq.QuadPart;
	}

	ChatStat getChatStat() const
	{
		ChatStat stat;
		stat.joinCnt = mJoinCnt.load();
		stat.badCnt = mBadCnt.load();
		std::lock_guard<std::mutex> lock(mCounterLock);
		for (const std::unique_ptr<ChatCounter>& counter : mCounters)
		{
			stat.broadcastCnt += counter->broadcast.load(std::memory_order_relaxed);
			stat.deliverCnt += counter->deliver.load(std::memory_order_relaxed);
			stat.lockWaitTicks += counter->lockWaitTicks.load(std::memory_order_relaxed);
			stat.lockHoldTicks += counter->lockHoldTicks.load(std::memory_order_relaxed);
		}
		return stat;
	}

	int64_t getQpcFreq() const { return mQpcFreq; }

protected:
	// 같은 세션의 OnAccept/OnRecv/OnRelease는 동시에 불리지 않으므로(라이브러리 콜백 규칙) 자기 슬롯의 방 번호는 락 없이 읽고 쓴다
	void OnAccept(uint64_t id) override
	{
		mRoomOf[SlotOf(id)] = NO_ROOM;
	}

	void OnRecv(uint64_t id, const char* payload, int32_t len) override
	{
		if (len < 1)
		{
			disconnectBad(id);
			return;
		}
		switch (static_cast<eChatType>(payload[0]))
		{
		case eChatType::Join:
			if (len != sizeof(ChatJoin))
			{
				disconnectBad(id);
				return;
			}
			join(id, reinterpret_cast<const ChatJoin*>(payload)->room);
			break;
		case eChatType::Chat:
			if (len < static_cast<int32_t>(sizeof(ChatMsg)))
			{
				disconnectBad(id);
				return;
			}
			broadcast(id, payload, len);
			break;
		default:
			disconnectBad(id);	// 서버가 받을 종류가 아님(JoinOk 포함)
			break;
		}
	}

	void OnRelease(uint64_t id) override
	{
		int32_t& roomIdx = mRoomOf[SlotOf(id)];
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
		std::mutex lock;	// members, seq 보호 - 메시지를 전원의 링에 넣는 동안 잡는다
		std::vector<uint64_t> members;
		uint64_t seq = 0;	// 마지막으로 매긴 방 순번
	};

	// 워커별 카운터 - 브로드캐스트마다 올리므로 워커끼리 같은 캐시 라인을 쓰지 않게 64바이트 정렬, 쓰는 스레드는 하나
	struct alignas(64) ChatCounter
	{
		std::atomic<int64_t> broadcast = 0;
		std::atomic<int64_t> deliver = 0;
		std::atomic<int64_t> lockWaitTicks = 0;
		std::atomic<int64_t> lockHoldTicks = 0;
	};

	// 이 스레드의 카운터 - 처음 부를 때 만들어 목록에 올린다 (서버 객체는 프로세스에 하나라는 전제)
	ChatCounter& myCounter()
	{
		thread_local ChatCounter* tCounter = nullptr;
		if (tCounter == nullptr)
		{
			std::unique_ptr<ChatCounter> counter = std::make_unique<ChatCounter>();
			tCounter = counter.get();
			std::lock_guard<std::mutex> lock(mCounterLock);
			mCounters.push_back(std::move(counter));
		}
		return *tCounter;
	}

	static int64_t qpcNow()
	{
		LARGE_INTEGER now;
		QueryPerformanceCounter(&now);
		return now.QuadPart;
	}

	void disconnectBad(uint64_t id)
	{
		++mBadCnt;
		Disconnect(id);
	}

	// 방을 옮긴다. JoinOk는 방 락 안에서 보낸다 - 그래야 이 세션의 링에서 JoinOk가 이 방의 어떤 Chat보다 앞선다
	void join(uint64_t id, uint32_t room)
	{
		if (room >= static_cast<uint32_t>(mRoomCnt))
		{
			disconnectBad(id);
			return;
		}
		int32_t& roomIdx = mRoomOf[SlotOf(id)];
		if (roomIdx != NO_ROOM)
		{
			leave(roomIdx, id);
		}
		ChatJoinOk ok{ eChatType::JoinOk, room };
		{
			Room& target = mRooms[room];
			std::lock_guard<std::mutex> lock(target.lock);
			target.members.push_back(id);
			SendPacket(id, reinterpret_cast<const char*>(&ok), sizeof(ok));
		}
		roomIdx = static_cast<int32_t>(room);
		++mJoinCnt;
	}

	void leave(int32_t roomIdx, uint64_t id)
	{
		Room& room = mRooms[roomIdx];
		std::lock_guard<std::mutex> lock(room.lock);
		std::vector<uint64_t>& members = room.members;
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
	void broadcast(uint64_t id, const char* payload, int32_t len)
	{
		int32_t roomIdx = mRoomOf[SlotOf(id)];
		if (roomIdx == NO_ROOM)
		{
			disconnectBad(id);	// 입장 전 Chat
			return;
		}
		char msg[MAX_PAYLOAD_SIZE];
		memcpy(msg, payload, len);	// roomSeq를 채워야 하므로 복사 (payload는 라이브러리의 수신 버퍼라 읽기 전용)

		ChatCounter& counter = myCounter();
		Room& room = mRooms[roomIdx];
		int64_t waitStart = qpcNow();
		int64_t holdStart = 0;
		int64_t deliver = 0;
		{
			std::lock_guard<std::mutex> lock(room.lock);
			holdStart = qpcNow();
			uint64_t seq = ++room.seq;
			memcpy(msg + offsetof(ChatMsg, roomSeq), &seq, sizeof(seq));
			for (uint64_t member : room.members)
			{
				SendPacket(member, msg, len);	// 넘치면 라이브러리가 그 구성원을 끊는다 - 방에서는 OnRelease 때 빠진다
			}
			deliver = static_cast<int64_t>(room.members.size());
		}
		int64_t holdEnd = qpcNow();
		addSingleWriter(counter.broadcast, 1);
		addSingleWriter(counter.deliver, deliver);
		addSingleWriter(counter.lockWaitTicks, holdStart - waitStart);
		addSingleWriter(counter.lockHoldTicks, holdEnd - holdStart);
	}

	int mRoomCnt = 0;
	std::unique_ptr<Room[]> mRooms;
	std::vector<int32_t> mRoomOf;	// 슬롯 번호 → 방 번호 (NO_ROOM이면 입장 전)
	int64_t mQpcFreq = 0;
	mutable std::mutex mCounterLock;	// mCounters 목록 보호 - 카운터 값은 단일 쓰기라 락 없이 올린다
	std::vector<std::unique_ptr<ChatCounter>> mCounters;
	std::atomic<int64_t> mJoinCnt = 0;	// 입장은 연결마다 한 번이라 공유 카운터로 충분
	std::atomic<int64_t> mBadCnt = 0;
};
