#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "GameProtocol.h"
#include "Memory.h"

class Account;
class DbConn;
class DbThread;

// 게임 데이터를 DB에 쓰는 문장을 만들어 DB 스레드에 넘긴다(설계 3-3: 바뀔 때마다 바로 쓴다, 접속 중에는 메모리가 기준)
// 쓰기 함수는 메모리를 바꾼 락(플레이어 락·경매장 락) 안에서 부른다 - 같은 데이터의 변경이 DB에도 바꾼 순서대로 들어간다
// 한 번의 변경은 트랜잭션 하나(데이터 + 로그) - 로그와 실제 데이터가 어긋나지 않는다
// 아이디는 클라가 보낸 바이트라 X'16진수'로 넣는다(이스케이프 없이 어떤 바이트든 안전하고, 열은 VARBINARY라 그대로 비교된다)
class GameDb final
{
public:
	explicit GameDb(DbThread* dbThread);
	DbThread& GetThread();

	// 서버 시작 전(main 스레드): 테이블을 만들고, 지난 실행에서 끝나지 않은 경매를 취소·환불하고, 번호 시작값을 읽는다
	static bool Prepare(DbConn& conn, uint64_t* outLastItemUID, uint64_t* outLastAuctionID, std::string* outError);
	// DB 스레드에서(로그인): 계정을 읽고, 없으면 시작 골드·회복약으로 만든다. DB 오류면 nullptr
	static UniquePtr<Account> LoadOrCreate(DbConn& conn, const std::string& id, const uint16_t startPotionKind);

	// 아래는 메모리를 바꾼 락 안에서
	void SaveDungeonReward(const std::string& id, const int64_t gold, const std::vector<ItemInfo>& items); // 보스 처치 보상
	void SaveItemAdded(const std::string& id, const ItemInfo& item, const char* kind);                      // 던전에서 찾은 회복약 등
	void SaveItemRemoved(const std::string& id, const ItemInfo& item, const char* kind);                    // 회복약 사용 등
	void SaveEquip(const std::string& id, const eEquipSlot slot, const uint64_t itemUID);                   // itemUID 0이면 해제
	void SaveAuctionRegister(const uint64_t auctionID, const std::string& sellerID, const ItemInfo& item, const int32_t startPrice);
	void SaveBid(const uint64_t auctionID, const std::string& bidderID, const int32_t price, const std::string& prevBidderID, const int32_t prevPrice); // 이전 입찰이 없으면 prevBidderID 빈 문자열
	void SaveAuctionSold(const uint64_t auctionID, const ItemInfo& item, const std::string& sellerID, const std::string& winnerID, const int32_t price);
	// 입찰 없음·거래 취소·회수: 입찰자가 있으면 환불, 아이템은 판매자에게(bReturned) 또는 버림·회수(삭제)
	// detail: 로그에 남길 말(비면 state) - 회수면 운영자 메모
	void SaveAuctionEnded(const uint64_t auctionID, const ItemInfo& item, const std::string& sellerID, const char* state, const bool bReturned,
		const std::string& bidderID, const int32_t refund, const std::string& detail = std::string());
	void SaveRecall(const std::string& id, const ItemInfo& item, const std::string& adminNote);  // 운영자 회수 - 인벤토리의 아이템
	void SaveBan(const std::string& id, const int64_t banUntil, const std::string& adminNote);    // banUntil: 유닉스 시각(초), 0이면 해제

	// 이번 실행에서 읽지 않은 계정은 메모리가 없어 DB 스레드가 바로 실행한다(대기열 뒤로 넘기면 그사이 그 계정의 로그인이 옛 값을 읽는다)
	static std::vector<std::string> MakeRecallSql(const std::string& id, const ItemInfo& item, const std::string& adminNote);
	static std::vector<std::string> MakeBanSql(const std::string& id, const int64_t banUntil, const std::string& adminNote);

	static std::string ToHexLiteral(const std::string& bytes); // X'...' 리터럴

private:
	static std::string makeLogSql(const std::string& id, const char* kind, const uint64_t itemUID, const uint16_t itemKind, const int64_t goldDelta,
		const uint64_t auctionID, const std::string& detail);

	DbThread* mDbThread;
};
