#include <cstdio>
#include <utility>
#include "AuctionHouse.h"
#include "Account.h"
#include "Assert.h"
#include "Memory.h"
#include "TurnTimer.h"

static void copyID(char (&outID)[ACCOUNT_ID_LEN], const std::string& id)
{
	MemSet(outID, 0, sizeof(outID));
	MemCpy(outID, id.data(), (id.size() < ACCOUNT_ID_LEN) ? id.size() : ACCOUNT_ID_LEN - 1);
}

AuctionHouse::AuctionHouse(SendFunc send, TurnTimer* closeTimer, const int32_t durationSec)
	: mSend(std::move(send))
	, mCloseTimer(closeTimer)
	, mDurationSec(durationSec)
{
	Assert(mSend && closeTimer != nullptr && durationSec > 0, "AuctionHouse: 보내기 함수·마감 타이머·기간이 없음");
}

void AuctionHouse::Register(const uint64_t sessionID, Account* account, const std::string& accountID, const uint64_t itemUID, const int32_t startPrice)
{
	Assert(account != nullptr, "Register: account가 null");
	AuctionRegisterRes res{};
	res.Type = eGameType::AuctionRegisterRes;
	std::lock_guard<std::mutex> lock(mLock);
	if (startPrice < 1 || startPrice > AUCTION_PRICE_MAX)
	{
		res.Result = eAuctionRegisterResult::BadPrice; // 밖에서 온 값
	}
	else if (mAuctions.size() >= AUCTION_TOTAL_MAX)
	{
		res.Result = eAuctionRegisterResult::HouseFull;
	}
	else if (countBySeller(account) >= AUCTION_PER_PLAYER)
	{
		res.Result = eAuctionRegisterResult::TooMany;
	}
	else
	{
		ItemInfo item{};
		eTakeItemResult take;
		{
			std::lock_guard<std::mutex> playerLock(account->GetLock()); // 경매장 → 플레이어(가장 안쪽)
			take = account->TakeItem(itemUID, &item);                   // 아이템은 계정에서 빠져 경매장이 보관 - 한곳에만 있다(설계 3-10-1)
		}
		if (take == eTakeItemResult::NotFound)
		{
			res.Result = eAuctionRegisterResult::NotFound;
		}
		else if (take == eTakeItemResult::Equipped)
		{
			res.Result = eAuctionRegisterResult::Equipped;
		}
		else
		{
			const uint64_t auctionID = mNextAuctionID++;
			const Clock::time_point deadline = Clock::now() + std::chrono::seconds(mDurationSec);
			const Auction auction{ auctionID, item, account, accountID, sessionID, startPrice, nullptr, std::string(), 0, startPrice, deadline };
			mAuctions.emplace(auctionID, auction);
			mCloseTimer->Add(deadline, auctionID, 0); // 경매장 → 대기열 락. 마감 콜백은 대기열 락을 놓은 뒤 경매장 락
			res.Result = eAuctionRegisterResult::Ok;
			res.AuctionID = auctionID;
			mSend(sessionID, reinterpret_cast<const char*>(&res), sizeof(res));
			broadcast(eAuctionEvent::Registered, auction);
			return;
		}
	}
	mSend(sessionID, reinterpret_cast<const char*>(&res), sizeof(res));
}

void AuctionHouse::Bid(const uint64_t sessionID, Account* account, const std::string& accountID, const uint64_t auctionID, const int32_t price)
{
	Assert(account != nullptr, "Bid: account가 null");
	AuctionBidRes res{};
	res.Type = eGameType::AuctionBidRes;
	std::lock_guard<std::mutex> lock(mLock);
	const auto it = mAuctions.find(auctionID);
	if (it == mAuctions.end())
	{
		res.Result = eAuctionBidResult::NotFound;
		mSend(sessionID, reinterpret_cast<const char*>(&res), sizeof(res));
		return;
	}
	Auction& auction = it->second;
	const bool bHasBid = (auction.BidderAccount != nullptr);
	const Clock::time_point now = Clock::now();
	Clock::time_point& lastBid = mLastBidTime[sessionID]; // 세션마다 - OnRelease에서 지운다
	const int32_t raise = (auction.Price * AUCTION_RAISE_PERCENT / 100 > 1) ? auction.Price * AUCTION_RAISE_PERCENT / 100 : 1;
	if (now - lastBid < std::chrono::milliseconds(AUCTION_BID_INTERVAL_MS))
	{
		res.Result = eAuctionBidResult::TooFast; // 몰아 보내는 입찰 - 구경꾼 전원에게 가는 알림이 폭주하지 않게
	}
	else if (auction.SellerAccount == account)
	{
		res.Result = eAuctionBidResult::OwnAuction;
	}
	else if (auction.BidderAccount == account)
	{
		res.Result = eAuctionBidResult::AlreadyTop; // 내 입찰을 내가 올리지 않는다
	}
	else if (price > AUCTION_PRICE_MAX || (bHasBid ? price < auction.Price + raise : price < auction.StartPrice))
	{
		res.Result = eAuctionBidResult::TooLow; // 첫 입찰은 시작가 이상, 그 뒤는 현재가 + 5%(최소 1) 이상
	}
	else
	{
		bool bPaid = false;
		{
			std::lock_guard<std::mutex> playerLock(account->GetLock()); // 경매장 → 플레이어 - 골드를 미리 뺀다(설계 3-10-2)
			bPaid = account->SpendGold(price);
		}
		if (!bPaid)
		{
			res.Result = eAuctionBidResult::NotEnoughGold;
		}
		else
		{
			if (bHasBid)
			{
				// 이전 입찰자에게 돌려준다 - 새 입찰자의 플레이어 락을 놓은 뒤(플레이어 락 두 개를 함께 잡지 않는다)
				std::lock_guard<std::mutex> prevLock(auction.BidderAccount->GetLock());
				auction.BidderAccount->AddGold(auction.Price);
			}
			const uint64_t prevSessionID = auction.BidderSessionID;
			const int32_t prevPrice = auction.Price;
			auction.BidderAccount = account;
			auction.BidderID = accountID;
			auction.BidderSessionID = sessionID;
			auction.Price = price;
			lastBid = now; // 성공한 입찰만 센다 - 거절은 나에게만 답해 알림이 늘지 않는다
			res.Result = eAuctionBidResult::Ok;
			mSend(sessionID, reinterpret_cast<const char*>(&res), sizeof(res));
			if (bHasBid)
			{
				sendResult(prevSessionID, eAuctionResult::Outbid, auction, prevPrice);
			}
			broadcast(eAuctionEvent::Bid, auction);
			return;
		}
	}
	mSend(sessionID, reinterpret_cast<const char*>(&res), sizeof(res));
}

void AuctionHouse::FillList(const uint16_t itemKind, const uint64_t afterAuctionID, AuctionListRes* outRes)
{
	Assert(outRes != nullptr, "FillList: outRes가 null");
	std::lock_guard<std::mutex> lock(mLock);
	outRes->Cnt = 0;
	for (auto it = mAuctions.upper_bound(afterAuctionID); it != mAuctions.end() && outRes->Cnt < AUCTION_LIST_MAX; ++it)
	{
		if (itemKind == 0 || it->second.Item.ItemKind == itemKind)
		{
			fillEntry(it->second, &outRes->Entries[outRes->Cnt]);
			++outRes->Cnt;
		}
	}
}

void AuctionHouse::SetWatch(const uint64_t sessionID, const bool bWatch)
{
	std::lock_guard<std::mutex> lock(mLock);
	if (bWatch)
	{
		mWatchers.insert(sessionID);
	}
	else
	{
		mWatchers.erase(sessionID);
	}
}

void AuctionHouse::RemoveSession(const uint64_t sessionID)
{
	std::lock_guard<std::mutex> lock(mLock);
	mWatchers.erase(sessionID);
	mLastBidTime.erase(sessionID);
}

void AuctionHouse::Close(const uint64_t auctionID)
{
	std::lock_guard<std::mutex> lock(mLock);
	const auto it = mAuctions.find(auctionID);
	if (it == mAuctions.end())
	{
		return; // 회수로 이미 취소됨
	}
	const Auction auction = it->second;
	mAuctions.erase(it);

	if (auction.BidderAccount == nullptr)
	{
		const bool bReturned = returnItemToSeller(auction); // 입찰 없음 - 아이템을 돌려준다
		sendResult(auction.SellerSessionID, bReturned ? eAuctionResult::Unsold : eAuctionResult::Discarded, auction, 0);
		broadcast(eAuctionEvent::Closed, auction);
		return;
	}

	bool bDelivered = false;
	{
		std::lock_guard<std::mutex> winnerLock(auction.BidderAccount->GetLock()); // 경매장 → 플레이어(한 번에 하나)
		if (auction.BidderAccount->IsInventoryFull())
		{
			auction.BidderAccount->AddGold(auction.Price); // 구매자 인벤토리가 가득 차 거래 취소 - 골드 환불(설계 3-10-3)
		}
		else
		{
			auction.BidderAccount->AddItem(auction.Item);
			bDelivered = true;
		}
	}
	if (bDelivered)
	{
		{
			std::lock_guard<std::mutex> sellerLock(auction.SellerAccount->GetLock());
			auction.SellerAccount->AddGold(auction.Price);
		}
		sendResult(auction.BidderSessionID, eAuctionResult::Won, auction, auction.Price);
		sendResult(auction.SellerSessionID, eAuctionResult::Sold, auction, auction.Price);
		broadcast(eAuctionEvent::Closed, auction);
	}
	else
	{
		const bool bReturned = returnItemToSeller(auction);
		sendResult(auction.BidderSessionID, eAuctionResult::Cancelled, auction, auction.Price);
		sendResult(auction.SellerSessionID, bReturned ? eAuctionResult::Unsold : eAuctionResult::Discarded, auction, 0);
		broadcast(eAuctionEvent::Cancelled, auction); // 낙찰로 보이지 않게 - 입찰은 무효
	}
}

bool AuctionHouse::CancelByItem(const uint64_t itemUID, ItemInfo* outItem)
{
	Assert(outItem != nullptr, "CancelByItem: outItem이 null");
	std::lock_guard<std::mutex> lock(mLock);
	for (auto it = mAuctions.begin(); it != mAuctions.end(); ++it)
	{
		if (it->second.Item.ItemUID != itemUID)
		{
			continue;
		}
		const Auction auction = it->second;
		mAuctions.erase(it);
		if (auction.BidderAccount != nullptr)
		{
			{
				std::lock_guard<std::mutex> bidderLock(auction.BidderAccount->GetLock());
				auction.BidderAccount->AddGold(auction.Price); // 입찰자 환불
			}
			sendResult(auction.BidderSessionID, eAuctionResult::Cancelled, auction, auction.Price);
		}
		sendResult(auction.SellerSessionID, eAuctionResult::Recalled, auction, 0);
		broadcast(eAuctionEvent::Cancelled, auction);
		*outItem = auction.Item;
		return true;
	}
	return false;
}

void AuctionHouse::fillEntry(const Auction& auction, AuctionEntry* outEntry) const
{
	AuctionEntry entry{};
	entry.AuctionID = auction.ID;
	entry.ItemKind = auction.Item.ItemKind;
	copyID(entry.SellerID, auction.SellerID);
	entry.Price = auction.Price;
	entry.bHasBid = (auction.BidderAccount != nullptr) ? 1 : 0;
	copyID(entry.BidderID, auction.BidderID);
	const long long remain = std::chrono::duration_cast<std::chrono::seconds>(auction.Deadline - Clock::now()).count();
	entry.RemainSec = static_cast<uint16_t>((remain > 0) ? remain : 0);
	*outEntry = entry;
}

void AuctionHouse::broadcast(const eAuctionEvent event, const Auction& auction)
{
	AuctionNotify notify{};
	notify.Type = eGameType::AuctionNotify;
	notify.Event = event;
	fillEntry(auction, &notify.Entry);
	for (const uint64_t watcher : mWatchers)
	{
		mSend(watcher, reinterpret_cast<const char*>(&notify), sizeof(notify)); // 끊긴 세션이면 실패하고 넘어간다
	}
}

void AuctionHouse::sendResult(const uint64_t sessionID, const eAuctionResult result, const Auction& auction, const int32_t gold)
{
	AuctionResultNotify notify{};
	notify.Type = eGameType::AuctionResultNotify;
	notify.Result = result;
	notify.AuctionID = auction.ID;
	notify.Item = auction.Item;
	notify.Gold = gold;
	mSend(sessionID, reinterpret_cast<const char*>(&notify), sizeof(notify));
}

bool AuctionHouse::returnItemToSeller(const Auction& auction)
{
	std::lock_guard<std::mutex> sellerLock(auction.SellerAccount->GetLock());
	if (auction.SellerAccount->IsInventoryFull())
	{
		// 판매자도 가득 차면 버린다(설계 3-10-3) - DB 로그 단계에서 기록으로 바꾼다
		fprintf(stderr, "[auction] 판매자 %s 인벤토리가 가득 차 아이템 #%llu(종류 %u)를 버림\n", auction.SellerID.c_str(),
			static_cast<unsigned long long>(auction.Item.ItemUID), auction.Item.ItemKind);
		return false;
	}
	auction.SellerAccount->AddItem(auction.Item);
	return true;
}

int32_t AuctionHouse::countBySeller(const Account* seller) const
{
	int32_t cnt = 0;
	for (const std::pair<const uint64_t, Auction>& entry : mAuctions)
	{
		cnt += (entry.second.SellerAccount == seller) ? 1 : 0;
	}
	return cnt;
}
