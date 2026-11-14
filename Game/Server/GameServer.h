#pragma once
#include <atomic>
#include <functional>
#include <ostream>
#include <vector>
#include "NetworkLib.h" // 소켓 헤더를 다른 프로젝트 헤더보다 먼저(README_ERRORS 1-26)
#include "AccountTable.h"
#include "AdminProtocol.h"
#include "AuctionHouse.h"
#include "DbThread.h"
#include "GameData.h"
#include "GameDb.h"
#include "GameProtocol.h"
#include "GameRule.h"
#include "PartyTable.h"
#include "Player.h"
#include "TurnTimer.h"

// 게임 서버 - TRPG 던전(파티) + 경매장. 로그인·인벤토리·장비·파티·던전
class GameServer final : public NetworkLib
{
public:
	// Player 배열을 maxSession개 만든다 - Start에 넘길 MaxSession과 같은 값. gameData·gameDb는 서버보다 오래 살아야 한다
	// gameDb의 DB 스레드는 이미 돌고 있어야 한다. lastAuctionID: DB의 가장 큰 경매 번호(다시 켜도 번호가 겹치지 않게)
	GameServer(const int32_t maxSession, const GameData* gameData, const GameRule& rule, GameDb* gameDb, const uint64_t lastAuctionID);
	~GameServer() override;

	// 서버를 멈출 때 라이브러리 Stop() 대신 이것을 부른다 - 워커 밖 스레드(턴 타이머, 경매 마감 타이머)를 멈추고,
	// 새 요청을 받지 않게 한 뒤 DB 스레드가 남은 저장을 끝내게 하고, 마지막에 라이브러리를 멈춘다
	// (DB 스레드는 로그인 결과를 보내고 UnholdSession을 부르므로 라이브러리보다 먼저 멈춘다)
	void Shutdown();

	int64_t GetBadPacketCnt() const // 컨텐츠가 잘못된 패킷(짧음, 모르는 종류, 종류와 맞지 않는 길이)으로 끊은 수 (누적)
	{
		return mBadPacketCnt;
	}
	int64_t GetDbPendingCnt() const;
	int64_t GetDbFailCnt() const;

	// 운영 명령(운영자 서버의 워커에서) - 실제 일은 DB 스레드가 하고, 결과를 reply로 돌려준다(DB 스레드에서 불림)
	// DB 스레드에서 하는 이유: 이번 실행에서 읽지 않은 계정은 DB에만 있는데, 계정을 읽는 로그인도 DB 스레드라 그동안 그 계정이 메모리에 올라오지 않는다
	void AdminRecall(const uint64_t itemUID, const std::string& note, std::function<void(const AdminRecallRes&)> reply);
	void AdminBan(const std::string& accountID, const int32_t minutes, const std::string& note, std::function<void(const AdminBanRes&)> reply);

protected:
	void OnAccept(const uint64_t id, const SOCKADDR_IN& remoteAddr) override;
	void OnRecv(const uint64_t id, const char* payload, const int32_t len) override;
	void OnRelease(const uint64_t id) override;

private:
	Player& getPlayer(const uint64_t id);         // 세션 ID의 하위 32비트(인덱스)로 같은 칸을 찾는다
	void onRecvPacket(const uint64_t id, const char* payload, const int32_t len); // 종류로 나눠 처리(OnRecv가 종료 중이 아닐 때만)
	void onLoginReq(const uint64_t id, const char* payload, const int32_t len);
	bool postExternal(DbThread::Job job); // 운영 명령을 DB 스레드에 넣는다 - 종료 중이면 false(OnRecv와 같은 처리 중 수로 Shutdown과 맞춘다)
	void recallOnDbThread(DbConn& conn, const uint64_t itemUID, const std::string& note, AdminRecallRes* outRes);
	void banOnDbThread(DbConn& conn, const std::string& accountID, const int32_t minutes, const std::string& note, AdminBanRes* outRes);
	void finishLogin(const uint64_t id, const std::string& accountID, UniquePtr<Account> loaded); // 워커(이미 읽은 계정) 또는 DB 스레드(막 읽은 계정)
	void onInventoryReq(const uint64_t id, const int32_t len);
	void onPartyCreateReq(const uint64_t id, const int32_t len);
	void onPartyListReq(const uint64_t id, const int32_t len);
	void onPartyJoinReq(const uint64_t id, const char* payload, const int32_t len);
	void onPartyLeaveReq(const uint64_t id, const int32_t len);
	void onPartyStartReq(const uint64_t id, const int32_t len);
	void onDungeonActionReq(const uint64_t id, const char* payload, const int32_t len);
	void onEquipReq(const uint64_t id, const char* payload, const int32_t len, const bool bEquip); // 착용과 해제를 같이
	void onAuctionRegisterReq(const uint64_t id, const char* payload, const int32_t len);
	void onAuctionListReq(const uint64_t id, const char* payload, const int32_t len);
	void onAuctionBidReq(const uint64_t id, const char* payload, const int32_t len);
	void onAuctionWatchReq(const uint64_t id, const char* payload, const int32_t len);
	bool isInDungeon(Player& player);// 파티 락으로 확인하고 놓는다 - 놓은 뒤 바뀔 수 있으니, 확인과 변경이 함께여야 하면 파티 락을 쥔 채 처리한다
	void sendInventory(const uint64_t id, Account& account);

	// 아래 셋은 파티 락을 잡은 채로 부른다(onTurnExpired는 스스로 파티 락을 잡는다)
	UniquePtr<Dungeon> createDungeon(const Party& party);                                   // 파티원마다 플레이어 락으로 장비 능력치를 읽어 던전 상태를 만든다
	void broadcastDungeon(Party& party, const std::vector<DungeonEventNotify>& events);     // 이벤트 → 차례 알림(끝났으면 finishDungeon)
	void finishDungeon(Party& party);                                                       // 결과를 알리고 모집 중으로 되돌린다
	void onTurnExpired(const uint64_t partyID, const uint32_t turnNo);                      // 타이머 스레드 - 턴 번호가 그대로면 자동 방어(설계 3-9-2)

	// 내가 지금 정말 들어가 있는 파티 - Player의 파티 번호로 찾아 파티 락 안에서 확인하고, 아니면 번호를 0으로 고친다(설계 3-8-3)
	// 돌려받은 뒤 다시 락을 잡아 쓸 때도 IsClosed·HasMember를 다시 확인해야 한다(그사이 바뀔 수 있다)
	std::shared_ptr<Party> findMyParty(Player& player);
	void leaveParty(Player& player, const bool bNotifyMe); // 파티에서 나간다 - 나가기 요청과 OnRelease가 함께 쓴다
	void broadcastPartyInfo(const Party& party);          // 파티 락을 잡은 채로 부른다 - 파티원 전원에게 같은 상태를 같은 순서로

	bool isLoggedIn(const uint64_t id);          // 로그인 전이면 잘못된 패킷으로 끊고 false
	void disconnectBadPacket(const uint64_t id); // 잘못된 패킷 - 세고 끊는다

	std::vector<Player> mPlayers;           // 세션과 같은 인덱스 - 같은 인덱스는 이전 세션의 OnRelease가 끝난 뒤에만 다시 쓰인다
	AccountTable mAccountTable;
	PartyTable mPartyTable;
	const GameData* mGameData; // 시작 전에 읽고 그 뒤로는 읽기만 해 락이 없다
	const GameRule mRule;
	GameDb* mGameDb;           // 저장 요청을 DB 스레드로 넘긴다
	std::atomic<bool> mbClosing = false;          // Shutdown 중 - 새 패킷을 처리하지 않아 DB 요청이 더 생기지 않는다
	std::atomic<int32_t> mRecvInProgressCnt = 0;  // 지금 패킷을 처리 중인 워커 수 - Shutdown이 0이 되기를 기다린 뒤 DB 스레드를 멈춘다
	TurnTimer mTurnTimer;      // 워커 밖 스레드 - 소멸자에서 라이브러리 Stop()보다 먼저 멈춘다
	TurnTimer mAuctionTimer;   // 경매 마감(경매 번호를 파티 번호 자리에) - mAuctionHouse보다 먼저 만들어진다
	AuctionHouse mAuctionHouse;
	std::atomic<int64_t> mBadPacketCnt = 0; // 서로 다른 세션의 OnRecv가 여러 워커에서 동시에 올린다
};

// 모니터링 출력 - std::cout << server; 한 줄(줄바꿈은 부르는 쪽이 붙인다)
std::ostream& operator<<(std::ostream& out, const GameServer& server);
