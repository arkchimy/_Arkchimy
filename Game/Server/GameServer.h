#pragma once
#include <atomic>
#include <ostream>
#include <vector>
#include "NetworkLib.h"
#include "AccountTable.h"
#include "GameData.h"
#include "GameProtocol.h"
#include "GameRule.h"
#include "PartyTable.h"
#include "Player.h"
#include "TurnTimer.h"

// 게임 서버 - TRPG 던전(파티) + 경매장. 지금은 로그인·인벤토리·파티까지
class GameServer final : public NetworkLib
{
public:
	// Player 배열을 maxSession개 만든다 - Start에 넘길 MaxSession과 같은 값. gameData는 서버보다 오래 살아야 한다
	GameServer(const int32_t maxSession, const GameData* gameData, const GameRule& rule);
	~GameServer() override;

	int64_t GetBadPacketCnt() const // 컨텐츠가 잘못된 패킷(짧음, 모르는 종류, 종류와 맞지 않는 길이)으로 끊은 수 (누적)
	{
		return mBadPacketCnt;
	}

protected:
	void OnAccept(const uint64_t id, const SOCKADDR_IN& remoteAddr) override;
	void OnRecv(const uint64_t id, const char* payload, const int32_t len) override;
	void OnRelease(const uint64_t id) override;

private:
	Player& getPlayer(const uint64_t id);         // 세션 ID의 하위 32비트(인덱스)로 같은 칸을 찾는다
	void onLoginReq(const uint64_t id, const char* payload, const int32_t len);
	void onInventoryReq(const uint64_t id, const int32_t len);
	void onPartyCreateReq(const uint64_t id, const int32_t len);
	void onPartyListReq(const uint64_t id, const int32_t len);
	void onPartyJoinReq(const uint64_t id, const char* payload, const int32_t len);
	void onPartyLeaveReq(const uint64_t id, const int32_t len);
	void onPartyStartReq(const uint64_t id, const int32_t len);
	void onDungeonActionReq(const uint64_t id, const char* payload, const int32_t len);
	void onEquipReq(const uint64_t id, const char* payload, const int32_t len, const bool bEquip); // 착용과 해제를 같이
	bool isInDungeon(Player& player); // 파티 락으로 확인하고 놓는다 - 그 뒤 플레이어 락(파티 락과 함께 잡지 않음)
	void sendInventory(const uint64_t id, Account& account);

	// 아래 셋은 파티 락을 잡은 채로 부른다
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
	TurnTimer mTurnTimer;      // 워커 밖 스레드 - 소멸자에서 라이브러리 Stop()보다 먼저 멈춘다
	std::atomic<int64_t> mBadPacketCnt = 0; // 서로 다른 세션의 OnRecv가 여러 워커에서 동시에 올린다
};

// 모니터링 출력 - std::cout << server; 한 줄(줄바꿈은 부르는 쪽이 붙인다)
std::ostream& operator<<(std::ostream& out, const GameServer& server);
