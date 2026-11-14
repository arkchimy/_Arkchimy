#pragma once
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include "GameProtocol.h"

class Account;
class TurnTimer;

// 경매장 - 경매 목록과 구경꾼을 경매장 락 하나로 지킨다(설계 3-10-6)
// 락 순서: 경매장 락 → 플레이어 락(가장 안쪽, 한 번에 하나) → (마감 등록 시) 대기열 락. 파티 락과 함께 잡지 않는다
// 보내기는 넘겨받은 함수로 경매장 락 안에서 한다 - 구경꾼 모두 같은 순서로 받는다
class AuctionHouse final
{
public:
	using SendFunc = std::function<void(const uint64_t sessionID, const char* data, const int32_t len)>;
	using Clock = std::chrono::steady_clock;

	AuctionHouse(SendFunc send, TurnTimer* closeTimer, const int32_t durationSec);

	// 아래는 워커(요청한 세션의 콜백)에서. account는 요청한 세션의 계정(자기 Player에서 읽은 것)
	void Register(const uint64_t sessionID, Account* account, const std::string& accountID, const uint64_t itemUID, const int32_t startPrice);
	void Bid(const uint64_t sessionID, Account* account, const std::string& accountID, const uint64_t auctionID, const int32_t price);
	void FillList(const uint16_t itemKind, const uint64_t afterAuctionID, AuctionListRes* outRes); // afterAuctionID보다 큰 번호부터 최대 AUCTION_LIST_MAX건
	void SetWatch(const uint64_t sessionID, const bool bWatch);
	void RemoveSession(const uint64_t sessionID); // OnRelease에서 - 구경꾼·입찰 간격 기록을 지운다(경매는 남는다)

	void Close(const uint64_t auctionID);         // 마감 타이머 스레드에서 - 이미 마감됐으면 아무것도 안 한다

	// 운영자 회수(나중 단계): 그 아이템이 경매 중이면 경매를 취소하고(입찰자 환불, 판매자에게 회수 알림) 아이템을 outItem으로 넘긴다. 찾았으면 true
	bool CancelByItem(const uint64_t itemUID, ItemInfo* outItem);

private:
	struct Auction
	{
		uint64_t ID;
		ItemInfo Item;
		Account* SellerAccount;
		std::string SellerID;
		uint64_t SellerSessionID; // 알림을 보낼 세션 - 다시 접속했으면 옛 ID라 SendPacket이 실패하고 넘어간다
		int32_t StartPrice;
		Account* BidderAccount;   // nullptr이면 입찰 없음
		std::string BidderID;
		uint64_t BidderSessionID;
		int32_t Price;            // 최고 입찰가(입찰이 없으면 시작가)
		Clock::time_point Deadline;
	};

	void fillEntry(const Auction& auction, AuctionEntry* outEntry) const;
	void broadcast(const eAuctionEvent event, const Auction& auction);   // 경매장 락 안에서
	void sendResult(const uint64_t sessionID, const eAuctionResult result, const Auction& auction, const int32_t gold);
	bool returnItemToSeller(const Auction& auction);                     // 플레이어 락 - 가득 차면 버리고 로그, false
	int32_t countBySeller(const Account* seller) const;

	std::mutex mLock;                       // 경매장 락
	std::map<uint64_t, Auction> mAuctions;  // 경매 번호 순 - 목록이 등록 순서로 나온다
	std::set<uint64_t> mWatchers;           // 경매장 보기를 켠 세션
	std::unordered_map<uint64_t, Clock::time_point> mLastBidTime; // 세션마다 마지막으로 성공한 입찰 시각
	uint64_t mNextAuctionID = 1;
	SendFunc mSend;
	TurnTimer* mCloseTimer;                 // 마감 등록(경매 번호를 파티 번호 자리에 넣는다)
	const int32_t mDurationSec;
};
