#include <utility>
#include <vector>
#include "PartyTable.h"
#include "Assert.h"
#include "Memory.h"

std::shared_ptr<Party> PartyTable::Create(const uint64_t leaderSessionID, const std::string& leaderAccountID, Account* leaderAccount)
{
	// 첫 파티원(방장)을 넣은 뒤에 표에 올린다 - 먼저 올리면 그 틈에 목록 요청이 빈 파티를 읽거나(Assert) 다른 사람이 먼저 들어가 방장이 된다
	// 아직 아무도 볼 수 없는 객체라 표 락 밖에서 파티 락을 잡아도 락 순서와 상관없다
	std::shared_ptr<Party> party(New<Party>(newPartyID()), Deleter()); // New/Delete 규칙 - std::make_shared를 쓰지 않는다
	{
		std::lock_guard<std::mutex> partyLock(party->GetLock());
		party->AddMember(leaderSessionID, leaderAccountID, leaderAccount);
	}
	std::lock_guard<std::mutex> lock(mLock);
	const bool bInserted = mParties.emplace(party->GetID(), party).second;
	Assert(bInserted, "Create: 파티 번호가 겹침 - 64비트 번호는 돌아오지 않는다");
	return party;
}

std::shared_ptr<Party> PartyTable::Find(const uint64_t partyID)
{
	std::lock_guard<std::mutex> lock(mLock);
	const auto it = mParties.find(partyID);
	return (it != mParties.end()) ? it->second : nullptr;
}

void PartyTable::Remove(const uint64_t partyID)
{
	std::lock_guard<std::mutex> lock(mLock);
	mParties.erase(partyID);
}

void PartyTable::FillList(PartyListRes* outRes)
{
	Assert(outRes != nullptr, "FillList: outRes가 null");
	std::vector<std::shared_ptr<Party>> parties;
	{
		std::lock_guard<std::mutex> lock(mLock); // 표 락 안에서는 복사만 - 표 락을 쥔 채 파티 락을 잡지 않는다
		parties.reserve(mParties.size());
		for (const std::pair<const uint64_t, std::shared_ptr<Party>>& entry : mParties)
		{
			parties.push_back(entry.second);
		}
	}

	outRes->PartyCnt = 0;
	for (const std::shared_ptr<Party>& party : parties)
	{
		if (outRes->PartyCnt >= PARTY_LIST_MAX)
		{
			break; // 넘으면 PARTY_LIST_MAX개까지만(순서는 해시 순서라 뜻이 없다)
		}
		std::lock_guard<std::mutex> partyLock(party->GetLock());
		if (party->IsClosed() || party->GetState() != ePartyState::Recruiting)
		{
			continue;
		}
		party->FillListEntry(&outRes->Entries[outRes->PartyCnt]); // 표에 올라간 파티는 닫히기 전까지 항상 1명 이상
		++outRes->PartyCnt;
	}
}

uint64_t PartyTable::newPartyID()
{
	std::lock_guard<std::mutex> lock(mLock);
	return mNextPartyID++;
}
