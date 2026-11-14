#include <cstdio>
#include <cstdlib>
#include "GameDb.h"
#include "Account.h"
#include "Assert.h"
#include "DbThread.h"
#include "ItemUID.h"

enum : int32_t
{
	START_GOLD = 100, // 처음 로그인한 아이디의 골드(설계 3-7)
};

// 테이블 - 아이디 열은 VARBINARY(클라가 보낸 바이트 그대로, 대소문자 구분 = 메모리 계정 표와 같은 비교)
static const char* const SCHEMA[] = {
	"CREATE TABLE IF NOT EXISTS accounts ("
	" account_id VARBINARY(15) NOT NULL PRIMARY KEY,"
	" gold BIGINT NOT NULL,"
	" ban_until DATETIME NULL," // 이 시각까지 로그인 차단(운영 툴 제재)
	" created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP)",

	// 아이템 한 개 = 한 줄. 경매 중이면 auction_id가 서고 account_id는 판매자 그대로(인벤토리를 읽을 때 뺀다)
	"CREATE TABLE IF NOT EXISTS items ("
	" item_uid BIGINT UNSIGNED NOT NULL PRIMARY KEY,"
	" account_id VARBINARY(15) NOT NULL,"
	" item_kind SMALLINT UNSIGNED NOT NULL,"
	" equip_slot TINYINT NULL,"
	" auction_id BIGINT UNSIGNED NULL,"
	" INDEX idx_items_account (account_id))",

	"CREATE TABLE IF NOT EXISTS auctions ("
	" auction_id BIGINT UNSIGNED NOT NULL PRIMARY KEY,"
	" item_uid BIGINT UNSIGNED NOT NULL,"
	" item_kind SMALLINT UNSIGNED NOT NULL,"
	" seller_id VARBINARY(15) NOT NULL,"
	" start_price INT NOT NULL,"
	" bidder_id VARBINARY(15) NULL,"
	" price INT NOT NULL,"
	" state VARCHAR(10) NOT NULL," // open, sold, unsold, cancelled, recalled
	" created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,"
	" closed_at DATETIME NULL)",

	// 골드·아이템이 움직일 때마다 한 줄 - 운영 툴이 직접 읽는다
	"CREATE TABLE IF NOT EXISTS logs ("
	" log_id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,"
	" at DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),"
	" account_id VARBINARY(15) NOT NULL,"
	" kind VARCHAR(16) NOT NULL,"
	" item_uid BIGINT UNSIGNED NULL,"
	" item_kind SMALLINT UNSIGNED NULL,"
	" gold_delta BIGINT NOT NULL DEFAULT 0,"
	" auction_id BIGINT UNSIGNED NULL,"
	" detail VARBINARY(64) NULL," // 운영자 메모 등 - 바이트 그대로
	" INDEX idx_logs_account (account_id, at),"
	" INDEX idx_logs_item (item_uid))",
};

static std::string toSqlNumber(const uint64_t value)
{
	return std::to_string(value);
}

static std::string toSqlNumber(const int64_t value)
{
	return std::to_string(value);
}

GameDb::GameDb(DbThread* dbThread)
	: mDbThread(dbThread)
{
	Assert(dbThread != nullptr, "GameDb: dbThread가 null");
}

DbThread& GameDb::GetThread()
{
	return *mDbThread;
}

std::string GameDb::ToHexLiteral(const std::string& bytes)
{
	constexpr char DIGITS[] = "0123456789ABCDEF";
	std::string out = "X'";
	for (const char c : bytes)
	{
		const uint8_t byte = static_cast<uint8_t>(c);
		out.push_back(DIGITS[byte >> 4]);
		out.push_back(DIGITS[byte & 0x0F]);
	}
	out.push_back('\'');
	return out;
}

std::string GameDb::makeLogSql(const std::string& id, const char* kind, const uint64_t itemUID, const uint16_t itemKind, const int64_t goldDelta,
	const uint64_t auctionID, const std::string& detail)
{
	// detail은 Hex로 넣어 어떤 바이트든 안전하다. 열 크기(64)를 넘으면 엄격 모드에서 문장이 실패하므로 자른다
	const std::string shortDetail = detail.substr(0, 64);
	return "INSERT INTO logs (account_id, kind, item_uid, item_kind, gold_delta, auction_id, detail) VALUES (" + ToHexLiteral(id) + ", '" + kind + "', "
		+ (itemUID != 0 ? toSqlNumber(itemUID) : std::string("NULL")) + ", " + (itemUID != 0 ? std::to_string(itemKind) : std::string("NULL")) + ", "
		+ toSqlNumber(goldDelta) + ", " + (auctionID != 0 ? toSqlNumber(auctionID) : std::string("NULL")) + ", "
		+ (shortDetail.empty() ? std::string("NULL") : ToHexLiteral(shortDetail)) + ")";
}

bool GameDb::Prepare(DbConn& conn, uint64_t* outLastItemUID, uint64_t* outLastAuctionID, std::string* outError)
{
	Assert(outLastItemUID != nullptr && outLastAuctionID != nullptr && outError != nullptr, "Prepare: out 매개변수가 null");
	for (const char* sql : SCHEMA)
	{
		if (!conn.Exec(sql))
		{
			*outError = "테이블 만들기 실패";
			return false;
		}
	}

	// 지난 실행이 끝날 때 열려 있던 경매 - 메모리의 경매장은 사라졌으므로 취소하고 맡긴 것을 돌려준다(입찰자 환불, 아이템은 판매자에게)
	std::vector<std::vector<std::string>> rows;
	if (!conn.Query("SELECT auction_id, item_uid, item_kind, seller_id, IFNULL(bidder_id, ''), price FROM auctions WHERE state = 'open'", &rows))
	{
		*outError = "열린 경매 읽기 실패";
		return false;
	}
	for (const std::vector<std::string>& row : rows)
	{
		const uint64_t auctionID = std::strtoull(row[0].c_str(), nullptr, 10);
		const ItemInfo item{ std::strtoull(row[1].c_str(), nullptr, 10), static_cast<uint16_t>(std::strtoul(row[2].c_str(), nullptr, 10)) };
		const std::string& sellerID = row[3];
		const std::string& bidderID = row[4];
		const int32_t price = std::atoi(row[5].c_str());
		std::vector<std::vector<std::string>> countRows;
		if (!conn.Query("SELECT COUNT(*) FROM items WHERE account_id = " + ToHexLiteral(sellerID) + " AND auction_id IS NULL", &countRows) || countRows.empty())
		{
			*outError = "판매자 인벤토리 읽기 실패";
			return false;
		}
		const bool bReturned = std::atoi(countRows[0][0].c_str()) < INVENTORY_MAX;
		std::vector<std::string> statements;
		if (!bidderID.empty())
		{
			statements.push_back("UPDATE accounts SET gold = gold + " + std::to_string(price) + " WHERE account_id = " + ToHexLiteral(bidderID));
			statements.push_back(makeLogSql(bidderID, "auction_refund", item.ItemUID, item.ItemKind, price, auctionID, "restart"));
		}
		statements.push_back(bReturned ? "UPDATE items SET auction_id = NULL WHERE item_uid = " + toSqlNumber(item.ItemUID)
			: "DELETE FROM items WHERE item_uid = " + toSqlNumber(item.ItemUID));
		statements.push_back(makeLogSql(sellerID, bReturned ? "auction_return" : "discard", item.ItemUID, item.ItemKind, 0, auctionID, "restart"));
		statements.push_back("UPDATE auctions SET state = 'cancelled', closed_at = NOW() WHERE auction_id = " + toSqlNumber(auctionID));
		if (!DbThread::RunTransaction(conn, statements, "재시작 경매 정리"))
		{
			*outError = "재시작 경매 정리 실패";
			return false;
		}
	}
	if (!rows.empty())
	{
		printf("[db] 지난 실행에서 열려 있던 경매 %zu건을 취소하고 돌려줌\n", rows.size());
	}

	// 번호는 지운 아이템(로그에만 남음)과도 겹치지 않게 - 로그로 아이템 이력을 따라갈 수 있어야 한다
	if (!conn.Query("SELECT GREATEST(IFNULL((SELECT MAX(item_uid) FROM items), 0), IFNULL((SELECT MAX(item_uid) FROM logs), 0),"
		" IFNULL((SELECT MAX(item_uid) FROM auctions), 0)), IFNULL((SELECT MAX(auction_id) FROM auctions), 0)", &rows) || rows.empty())
	{
		*outError = "번호 시작값 읽기 실패";
		return false;
	}
	*outLastItemUID = std::strtoull(rows[0][0].c_str(), nullptr, 10);
	*outLastAuctionID = std::strtoull(rows[0][1].c_str(), nullptr, 10);
	return true;
}

UniquePtr<Account> GameDb::LoadOrCreate(DbConn& conn, const std::string& id, const uint16_t startPotionKind)
{
	UniquePtr<Account> account = MakeUnique<Account>(id);
	bool bOk = false;
	{
		std::lock_guard<std::mutex> playerLock(account->GetLock()); // 아직 아무도 모르는 계정이지만 Account의 약속(락을 잡고 고친다)을 지킨다
		std::vector<std::vector<std::string>> rows;
		bOk = conn.Query("SELECT gold, IFNULL(UNIX_TIMESTAMP(ban_until), 0) FROM accounts WHERE account_id = " + ToHexLiteral(id), &rows);
		if (bOk && rows.empty())
		{
			// 처음 보는 아이디 - 계정과 시작 회복약을 한 트랜잭션으로 만든다
			const ItemInfo potion{ NewItemUID(), startPotionKind };
			const std::vector<std::string> statements = {
				"INSERT INTO accounts (account_id, gold) VALUES (" + ToHexLiteral(id) + ", " + std::to_string(START_GOLD) + ")",
				"INSERT INTO items (item_uid, account_id, item_kind) VALUES (" + toSqlNumber(potion.ItemUID) + ", " + ToHexLiteral(id) + ", " + std::to_string(potion.ItemKind) + ")",
				makeLogSql(id, "create", potion.ItemUID, potion.ItemKind, START_GOLD, 0, ""),
			};
			bOk = DbThread::RunTransaction(conn, statements, "계정 만들기");
			if (bOk)
			{
				account->AddGold(START_GOLD);
				account->AddItem(potion);
			}
		}
		else if (bOk)
		{
			account->AddGold(std::strtoll(rows[0][0].c_str(), nullptr, 10));
			account->SetBanUntil(std::strtoll(rows[0][1].c_str(), nullptr, 10));
			// 인벤토리 - 경매 중인 아이템은 뺀다. 칸 수를 넘는 줄은 정상 경로로는 생기지 않지만, 생겨도 AddItem Assert가 나지 않게 앞에서 자른다
			bOk = conn.Query("SELECT item_uid, item_kind, IFNULL(equip_slot, -1) FROM items WHERE account_id = " + ToHexLiteral(id)
				+ " AND auction_id IS NULL ORDER BY item_uid LIMIT " + std::to_string(INVENTORY_MAX), &rows);
			for (size_t i = 0; bOk && i < rows.size(); ++i)
			{
				const ItemInfo item{ std::strtoull(rows[i][0].c_str(), nullptr, 10), static_cast<uint16_t>(std::strtoul(rows[i][1].c_str(), nullptr, 10)) };
				account->AddItem(item);
				const int32_t slot = std::atoi(rows[i][2].c_str());
				if (slot >= 0 && slot < static_cast<int32_t>(eEquipSlot::Count))
				{
					account->RestoreEquip(static_cast<eEquipSlot>(slot), item.ItemUID);
				}
			}
		}
	}
	if (!bOk)
	{
		account.reset(); // DB 오류 - 반쯤 읽은 계정을 쓰지 않는다
	}
	return account;
}

void GameDb::SaveDungeonReward(const std::string& id, const int64_t gold, const std::vector<ItemInfo>& items)
{
	std::vector<std::string> statements;
	statements.push_back("UPDATE accounts SET gold = gold + " + toSqlNumber(gold) + " WHERE account_id = " + ToHexLiteral(id));
	statements.push_back(makeLogSql(id, "dungeon_gold", 0, 0, gold, 0, ""));
	for (const ItemInfo& item : items)
	{
		statements.push_back("INSERT INTO items (item_uid, account_id, item_kind) VALUES (" + toSqlNumber(item.ItemUID) + ", " + ToHexLiteral(id) + ", "
			+ std::to_string(item.ItemKind) + ")");
		statements.push_back(makeLogSql(id, "loot", item.ItemUID, item.ItemKind, 0, 0, ""));
	}
	mDbThread->PostTransaction(std::move(statements), "던전 보상");
}

void GameDb::SaveItemAdded(const std::string& id, const ItemInfo& item, const char* kind)
{
	mDbThread->PostTransaction({
		"INSERT INTO items (item_uid, account_id, item_kind) VALUES (" + toSqlNumber(item.ItemUID) + ", " + ToHexLiteral(id) + ", " + std::to_string(item.ItemKind) + ")",
		makeLogSql(id, kind, item.ItemUID, item.ItemKind, 0, 0, ""),
		}, "아이템 추가");
}

void GameDb::SaveItemRemoved(const std::string& id, const ItemInfo& item, const char* kind)
{
	mDbThread->PostTransaction({
		"DELETE FROM items WHERE item_uid = " + toSqlNumber(item.ItemUID),
		makeLogSql(id, kind, item.ItemUID, item.ItemKind, 0, 0, ""),
		}, "아이템 제거");
}

void GameDb::SaveEquip(const std::string& id, const eEquipSlot slot, const uint64_t itemUID)
{
	const std::string slotNo = std::to_string(static_cast<int32_t>(slot));
	std::vector<std::string> statements = { "UPDATE items SET equip_slot = NULL WHERE account_id = " + ToHexLiteral(id) + " AND equip_slot = " + slotNo };
	if (itemUID != 0)
	{
		statements.push_back("UPDATE items SET equip_slot = " + slotNo + " WHERE item_uid = " + toSqlNumber(itemUID));
	}
	mDbThread->PostTransaction(std::move(statements), "장비"); // 장비는 골드·아이템이 움직이지 않아 로그를 남기지 않는다
}

void GameDb::SaveAuctionRegister(const uint64_t auctionID, const std::string& sellerID, const ItemInfo& item, const int32_t startPrice)
{
	mDbThread->PostTransaction({
		"INSERT INTO auctions (auction_id, item_uid, item_kind, seller_id, start_price, price, state) VALUES (" + toSqlNumber(auctionID) + ", "
			+ toSqlNumber(item.ItemUID) + ", " + std::to_string(item.ItemKind) + ", " + ToHexLiteral(sellerID) + ", " + std::to_string(startPrice) + ", "
			+ std::to_string(startPrice) + ", 'open')",
		"UPDATE items SET auction_id = " + toSqlNumber(auctionID) + " WHERE item_uid = " + toSqlNumber(item.ItemUID),
		makeLogSql(sellerID, "auction_register", item.ItemUID, item.ItemKind, 0, auctionID, ""),
		}, "경매 등록");
}

void GameDb::SaveBid(const uint64_t auctionID, const std::string& bidderID, const int32_t price, const std::string& prevBidderID, const int32_t prevPrice)
{
	std::vector<std::string> statements = {
		"UPDATE accounts SET gold = gold - " + std::to_string(price) + " WHERE account_id = " + ToHexLiteral(bidderID),
		"UPDATE auctions SET bidder_id = " + ToHexLiteral(bidderID) + ", price = " + std::to_string(price) + " WHERE auction_id = " + toSqlNumber(auctionID),
		makeLogSql(bidderID, "bid", 0, 0, -static_cast<int64_t>(price), auctionID, ""),
	};
	if (!prevBidderID.empty())
	{
		statements.push_back("UPDATE accounts SET gold = gold + " + std::to_string(prevPrice) + " WHERE account_id = " + ToHexLiteral(prevBidderID));
		statements.push_back(makeLogSql(prevBidderID, "outbid_refund", 0, 0, prevPrice, auctionID, ""));
	}
	mDbThread->PostTransaction(std::move(statements), "입찰"); // 새 입찰자 차감과 이전 입찰자 환불을 함께
}

void GameDb::SaveAuctionSold(const uint64_t auctionID, const ItemInfo& item, const std::string& sellerID, const std::string& winnerID, const int32_t price)
{
	// 낙찰 = 아이템 이동 + 판매자 골드 + 경매 닫기를 한 번에(설계 3-3) - 낙찰자 골드는 입찰 때 이미 뺐다
	mDbThread->PostTransaction({
		"UPDATE items SET account_id = " + ToHexLiteral(winnerID) + ", auction_id = NULL, equip_slot = NULL WHERE item_uid = " + toSqlNumber(item.ItemUID),
		"UPDATE accounts SET gold = gold + " + std::to_string(price) + " WHERE account_id = " + ToHexLiteral(sellerID),
		"UPDATE auctions SET state = 'sold', closed_at = NOW() WHERE auction_id = " + toSqlNumber(auctionID),
		makeLogSql(winnerID, "auction_won", item.ItemUID, item.ItemKind, 0, auctionID, ""),
		makeLogSql(sellerID, "auction_sold", item.ItemUID, item.ItemKind, price, auctionID, ""),
		}, "낙찰");
}

void GameDb::SaveAuctionEnded(const uint64_t auctionID, const ItemInfo& item, const std::string& sellerID, const char* state, const bool bReturned,
	const std::string& bidderID, const int32_t refund, const std::string& detail)
{
	std::vector<std::string> statements;
	if (!bidderID.empty())
	{
		statements.push_back("UPDATE accounts SET gold = gold + " + std::to_string(refund) + " WHERE account_id = " + ToHexLiteral(bidderID));
		statements.push_back(makeLogSql(bidderID, "auction_refund", item.ItemUID, item.ItemKind, refund, auctionID, ""));
	}
	statements.push_back(bReturned ? "UPDATE items SET auction_id = NULL WHERE item_uid = " + toSqlNumber(item.ItemUID)
		: "DELETE FROM items WHERE item_uid = " + toSqlNumber(item.ItemUID));
	const bool bRecalled = (std::string(state) == "recalled");
	statements.push_back(makeLogSql(sellerID, bReturned ? "auction_return" : (bRecalled ? "recall" : "discard"), item.ItemUID, item.ItemKind, 0, auctionID,
		detail.empty() ? std::string(state) : detail));
	statements.push_back("UPDATE auctions SET state = '" + std::string(state) + "', closed_at = NOW() WHERE auction_id = " + toSqlNumber(auctionID));
	mDbThread->PostTransaction(std::move(statements), "경매 종료");
}

std::vector<std::string> GameDb::MakeRecallSql(const std::string& id, const ItemInfo& item, const std::string& adminNote)
{
	const std::vector<std::string> statements = {
		"DELETE FROM items WHERE item_uid = " + toSqlNumber(item.ItemUID),
		makeLogSql(id, "recall", item.ItemUID, item.ItemKind, 0, 0, adminNote),
	};
	return statements;
}

std::vector<std::string> GameDb::MakeBanSql(const std::string& id, const int64_t banUntil, const std::string& adminNote)
{
	const std::vector<std::string> statements = {
		"UPDATE accounts SET ban_until = " + (banUntil > 0 ? "FROM_UNIXTIME(" + toSqlNumber(banUntil) + ")" : std::string("NULL")) + " WHERE account_id = " + ToHexLiteral(id),
		makeLogSql(id, banUntil > 0 ? "ban" : "unban", 0, 0, 0, 0, adminNote),
	};
	return statements;
}

void GameDb::SaveRecall(const std::string& id, const ItemInfo& item, const std::string& adminNote)
{
	mDbThread->PostTransaction(MakeRecallSql(id, item, adminNote), "회수");
}

void GameDb::SaveBan(const std::string& id, const int64_t banUntil, const std::string& adminNote)
{
	mDbThread->PostTransaction(MakeBanSql(id, banUntil, adminNote), "제재");
}
