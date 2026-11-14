#include "Party.h"
#include "Assert.h"
#include "Memory.h"

// 아이디를 고정 길이 칸에 넣는다 - 아이디는 로그인 때 15바이트 이하로 확인했으므로 항상 널로 끝난다
static void copyAccountID(char (&outID)[ACCOUNT_ID_LEN], const std::string& id)
{
	Assert(id.size() < ACCOUNT_ID_LEN, "copyAccountID: 로그인 검사를 통과한 아이디가 너무 김");
	MemSet(outID, 0, sizeof(outID));
	MemCpy(outID, id.data(), id.size());
}

Party::Party(const uint64_t id)
	: mID(id)
{
	mMembers.reserve(PARTY_MEMBER_MAX);
}

uint64_t Party::GetID() const
{
	return mID;
}

std::mutex& Party::GetLock()
{
	return mLock;
}

bool Party::IsClosed() const
{
	return mbClosed;
}

ePartyState Party::GetState() const
{
	return mState;
}

bool Party::HasMember(const uint64_t sessionID) const
{
	for (const PartyMember& member : mMembers)
	{
		if (member.SessionID == sessionID)
		{
			return true;
		}
	}
	return false;
}

bool Party::IsLeader(const uint64_t sessionID) const
{
	return !mMembers.empty() && mMembers[0].SessionID == sessionID;
}

bool Party::IsFull() const
{
	return static_cast<int32_t>(mMembers.size()) >= PARTY_MEMBER_MAX;
}

int32_t Party::GetMemberCnt() const
{
	return static_cast<int32_t>(mMembers.size());
}

const std::vector<PartyMember>& Party::GetMembers() const
{
	return mMembers;
}

int32_t Party::FindMemberIdx(const uint64_t sessionID) const
{
	for (size_t i = 0; i < mMembers.size(); ++i)
	{
		if (mMembers[i].SessionID == sessionID)
		{
			return static_cast<int32_t>(i);
		}
	}
	return -1;
}

void Party::AddMember(const uint64_t sessionID, const std::string& accountID, Account* account)
{
	Assert(!mbClosed && !IsFull() && !HasMember(sessionID) && account != nullptr, "AddMember: 닫혔거나 가득 찼거나 이미 파티원이거나 계정이 없음 - 부르는 쪽이 먼저 확인해야 한다");
	mMembers.push_back(PartyMember{ sessionID, accountID, account });
}

int32_t Party::RemoveMember(const uint64_t sessionID)
{
	const int32_t idx = FindMemberIdx(sessionID);
	Assert(idx >= 0, "RemoveMember: 파티원이 아님 - 부르는 쪽이 HasMember로 먼저 확인해야 한다");
	mMembers.erase(mMembers.begin() + idx); // 순서를 유지해 지운다 - 방장([0])이 나가면 다음 사람이 방장
	return idx;
}

void Party::StartDungeon(UniquePtr<Dungeon> dungeon)
{
	Assert(mState == ePartyState::Recruiting && dungeon != nullptr, "StartDungeon: 모집 중이 아니거나 던전이 없음");
	mState = ePartyState::InDungeon;
	mDungeon = std::move(dungeon);
}

void Party::EndDungeon()
{
	Assert(mState == ePartyState::InDungeon, "EndDungeon: 던전 중이 아님");
	mState = ePartyState::Recruiting;
	mLastTurnNo = mDungeon->GetTurnNo();
	mDungeon.reset();
}

Dungeon* Party::GetDungeon()
{
	return mDungeon.get();
}

uint32_t Party::GetLastTurnNo() const
{
	return mLastTurnNo;
}

void Party::Close()
{
	Assert(mMembers.empty(), "Close: 파티원이 남아 있음");
	mbClosed = true;
}

void Party::FillInfo(PartyInfoNotify* outNotify) const
{
	Assert(outNotify != nullptr, "FillInfo: outNotify가 null");
	MemSet(outNotify, 0, sizeof(*outNotify));
	outNotify->Type = eGameType::PartyInfoNotify;
	outNotify->PartyID = mID;
	outNotify->State = mState;
	outNotify->LeaderIdx = 0;
	outNotify->MemberCnt = static_cast<uint8_t>(mMembers.size());
	for (size_t i = 0; i < mMembers.size(); ++i)
	{
		copyAccountID(outNotify->MemberIDs[i], mMembers[i].AccountID);
	}
}

void Party::FillListEntry(PartyListEntry* outEntry) const
{
	Assert(outEntry != nullptr && !mMembers.empty(), "FillListEntry: outEntry가 null이거나 빈 파티");
	outEntry->PartyID = mID;
	copyAccountID(outEntry->LeaderID, mMembers[0].AccountID);
	outEntry->MemberCnt = static_cast<uint8_t>(mMembers.size());
}
