#pragma once
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include "GameProtocol.h"
#include "Party.h"

// 파티 번호 → 파티. 파티 표 락으로는 만들기·지우기·찾기만 하고, 표 락을 쥔 채 파티 락을 잡지 않는다(설계 3-8-2)
// 표에 올라간 파티는 닫히기 전까지 항상 파티원이 1명 이상이다(첫 파티원을 넣은 뒤에 올리므로)
class PartyTable final
{
public:
	std::shared_ptr<Party> Create(const uint64_t leaderSessionID, const std::string& leaderAccountID, Account* leaderAccount); // 방장을 넣은 파티를 표에 올린다
	std::shared_ptr<Party> Find(const uint64_t partyID); // 없으면 nullptr. 받은 뒤 파티 락을 잡고 IsClosed부터 확인한다
	void Remove(const uint64_t partyID);                 // Close한 파티를 표에서 뺀다
	void FillList(PartyListRes* outRes);                 // 모집 중인 파티만, 최대 PARTY_LIST_MAX개

private:
	uint64_t newPartyID();

	std::mutex mLock;                                              // 파티 표 락
	std::unordered_map<uint64_t, std::shared_ptr<Party>> mParties; // shared_ptr - 찾아 둔 파티가 그사이 표에서 빠져도 메모리는 남아 안전
	uint64_t mNextPartyID = 1;                                     // 0은 "파티 없음". 64비트라 운영 중에 돌아오지 않는다(32비트는 만들기·나가기 반복으로 넘칠 수 있다)
};
