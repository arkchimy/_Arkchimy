#pragma once
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>
#include "GameProtocol.h"

// 파티원 한 명 - 계정 주소가 아니라 세션 ID와 아이디로 들고 있는다(설계 3-8-3, 나중에 계정을 메모리에서 내려도 안전하게)
struct PartyMember
{
	uint64_t SessionID;
	std::string AccountID;
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
	void AddMember(const uint64_t sessionID, const std::string& accountID);
	void RemoveMember(const uint64_t sessionID); // 방장이 나가면 다음 사람이 [0]이 되어 방장이 된다
	void Start();                                // 모집 중 → 던전
	void Close();                                // 마지막 사람이 나갈 때
	void FillInfo(PartyInfoNotify* outNotify) const;
	void FillListEntry(PartyListEntry* outEntry) const;

private:
	std::mutex mLock;
	const uint64_t mID;
	ePartyState mState = ePartyState::Recruiting;
	bool mbClosed = false;
	std::vector<PartyMember> mMembers; // 최대 PARTY_MEMBER_MAX명, 참가 순서
};
