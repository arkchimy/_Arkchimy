#pragma once
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>
#include "Dungeon.h"
#include "GameProtocol.h"
#include "Memory.h"

class Account;

// 파티원 한 명 - 세션 ID와 아이디, 그리고 던전에서 계정을 만질 때 쓸 계정 주소
// 계정 주소는 참가할 때 자기 워커가 자기 Player에서 읽어 넣는다 - 다른 세션의 Player를 읽지 않고, 파티 락 안에서 계정 표 락을 잡지 않으려고
// 계정은 서버가 끝날 때까지 메모리에서 내리지 않아 주소가 유효하다. 계정을 메모리에서 내리게 되면 다시 정한다
struct PartyMember
{
	uint64_t SessionID;
	std::string AccountID;
	Account* MemberAccount;
};

// 파티 하나 - 상태는 파티 락 안에서만 읽고 바꾼다(설계 3-8-2). 방장은 항상 mMembers[0](참가 순서)
class Party final
{
public:
	explicit Party(const uint64_t id);

	uint64_t GetID() const; // 만든 뒤 바뀌지 않아 락 없이
	std::mutex& GetLock();  // 파티 락

	// 아래는 모두 파티 락을 잡은 채로 부른다
	bool IsClosed() const; // 마지막 사람이 나가 지워지는 중 - 찾은 뒤 락을 잡으면 먼저 확인한다
	ePartyState GetState() const;
	bool HasMember(const uint64_t sessionID) const;
	bool IsLeader(const uint64_t sessionID) const;
	bool IsFull() const;
	int32_t GetMemberCnt() const;
	const std::vector<PartyMember>& GetMembers() const;
	int32_t FindMemberIdx(const uint64_t sessionID) const; // 파티원 순서상 번호(던전 파티원 번호와 같다), 없으면 -1
	void AddMember(const uint64_t sessionID, const std::string& accountID, Account* account);
	int32_t RemoveMember(const uint64_t sessionID); // 빠진 사람의 번호. 방장이 나가면 다음 사람이 [0]이 되어 방장이 된다
	void StartDungeon(UniquePtr<Dungeon> dungeon);  // 모집 중 → 던전
	void EndDungeon();                              // 던전 → 모집 중(다시 시작할 수 있다)
	Dungeon* GetDungeon();                          // 던전 중이 아니면 nullptr
	uint32_t GetLastTurnNo() const;                 // 지난 던전이 끝난 턴 번호 - 새 던전이 이어서 센다
	// 이 턴의 마감을 처음 넣는 것이면 마감 시각을 기억하고 true - 차례가 그대로인 알림(파티원이 빠짐)에 마감을 또 넣지 않게
	bool MarkTurnScheduled(const uint32_t turnNo, const std::chrono::steady_clock::time_point deadline);
	std::chrono::steady_clock::time_point GetTurnDeadline() const; // 지금 턴의 마감 - 차례가 그대로인 알림에 남은 시간을 싣는다
	void Close();                                   // 마지막 사람이 나갈 때
	void FillInfo(PartyInfoNotify* outNotify) const;
	void FillListEntry(PartyListEntry* outEntry) const;

private:
	std::mutex mLock;
	const uint64_t mID;
	ePartyState mState = ePartyState::Recruiting;
	bool mbClosed = false;
	std::vector<PartyMember> mMembers; // 최대 PARTY_MEMBER_MAX명, 참가 순서
	UniquePtr<Dungeon> mDungeon;       // 던전 중일 때만 - 파티 락 안에서만 쓴다
	uint32_t mLastTurnNo = 0;
	uint32_t mScheduledTurnNo = 0;     // 마지막으로 마감을 넣은 턴 번호(턴 번호는 파티 안에서 계속 늘어나므로 던전이 바뀌어도 그대로 쓴다)
	std::chrono::steady_clock::time_point mTurnDeadline{};
};
