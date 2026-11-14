#pragma once
#include <cstdint>
#include <random>
#include <vector>
#include "GameData.h"
#include "GameProtocol.h"

class Account;

// 던전 파티원 한 명의 전투 값 - 시작할 때 계정 장비에서 읽어 복사한다(설계 3-9-1). 전투 중에는 계정을 보지 않는다
struct DungeonMember
{
	Account* MemberAccount; // 회복약을 꺼내고 넣을 때만(플레이어 락) - 계정은 지워지지 않으므로 주소가 그대로다
	int32_t Hp;
	int32_t MaxHp;
	int32_t Attack;
	int32_t Defense;
	int32_t Investigate;
	int32_t WeaponDamage;
	bool bDefending;    // 이번 라운드 방어 중 - 자기 차례가 올 때 내린다
	bool bInvestigated; // 이 방에서 이미 조사함
};

enum class eActResult : uint8_t
{
	Done = 0,     // 처리하고 차례가 넘어감(또는 방·던전이 끝남)
	Rejected = 1, // 할 수 없는 행동 - 차례는 그대로(ActionRejected 이벤트를 보냄)
	Ignored = 2,  // 내 차례가 아니거나 이미 넘어간 턴 - 아무것도 보내지 않음
};

// 던전 한 판의 규칙 - 네트워크를 모르고, 판정 결과를 이벤트 목록으로 돌려준다(보내는 것은 GameServer)
// 모든 함수는 이 던전을 가진 파티의 파티 락 안에서만 부른다. 회복약은 계정에 있으므로 그때만 플레이어 락을 잠깐 잡는다(파티 → 플레이어)
// 파티원 번호는 파티의 파티원 순서와 같다 - 파티원이 빠지면 같은 번호로 RemoveMember를 부른다
class Dungeon final
{
public:
	// lastTurnNo: 이 파티의 지난 던전이 끝난 턴 번호 - 턴 번호를 파티 안에서 계속 이어 매긴다
	// (새 던전이 1부터 다시 세면, 지난 던전의 남은 마감 "턴 5"가 새 던전의 턴 5에 잘못 걸린다)
	Dungeon(const GameData* gameData, std::vector<DungeonMember> members, const uint32_t lastTurnNo);

	void Begin(std::vector<DungeonEventNotify>* outEvents); // 첫 방에 들어간다
	eActResult Act(const int32_t memberIdx, const uint32_t turnNo, const eDungeonAction action, const int32_t target,
		std::vector<DungeonEventNotify>* outEvents);
	void RemoveMember(const int32_t memberIdx, std::vector<DungeonEventNotify>* outEvents); // 나가기·끊김 - 차례였으면 다음 사람에게
	bool AutoDefend(const uint32_t turnNo, std::vector<DungeonEventNotify>* outEvents);   // 시간 초과 - 턴 번호가 지금 턴일 때만 지금 차례인 사람을 방어로 넘긴다(아니면 false, 그사이 행동함)

	bool IsOver() const;
	bool IsSuccess() const;
	bool HasExtraLoot() const; // 숨은 상자를 찾음 - 보상 단계에서 전리품 1개 추가
	// 보스를 잡았을 때만 - 전리품을 파티 인원만큼(숨은 상자면 +1) 굴리고, 아이템마다 파티원 전원이 d20을 굴려 가장 높은 사람에게 준다(설계 3-9-6)
	// 결과는 파티원 번호별 아이템 종류 목록. 계정에 넣는 일은 부르는 쪽이 플레이어 락으로 한다
	std::vector<std::vector<uint16_t>> RollLoot();
	void FillTurn(DungeonTurnNotify* outNotify) const;
	uint32_t GetTurnNo() const;

private:
	struct DungeonMonster
	{
		const MonsterData* Data;
		int32_t Hp;
	};

	void enterRoom(const int32_t roomIdx, std::vector<DungeonEventNotify>* outEvents);
	void startTurn(const int32_t actorIdx);                                    // 차례를 정하고 턴 번호를 올린다
	void advanceTurn(std::vector<DungeonEventNotify>* outEvents);              // 다음 살아 있는 파티원 - 없으면 라운드 끝
	void endRound(std::vector<DungeonEventNotify>* outEvents);                 // 조사 방은 다음 방, 전투 방은 몬스터 행동
	void runMonsters(std::vector<DungeonEventNotify>* outEvents);
	void damageMember(const int32_t memberIdx, const int32_t damage, std::vector<DungeonEventNotify>* outEvents); // 쓰러지면 Down
	void finish(const bool bSuccess);
	bool isAllMembersDown() const;
	bool isAllMonstersDown() const;
	int32_t findAliveMemberFrom(const int32_t startIdx) const; // startIdx부터 살아 있는 첫 파티원, 없으면 -1
	int32_t rollDie(const int32_t sides);                       // 1–sides
	DungeonEventNotify makeEvent(const eDungeonEvent event, const uint8_t actor) const;
	uint16_t findPotionKind() const;                            // 회복량이 있는 첫 아이템 종류(데이터 파일)
	uint16_t rollLootKind();                                    // loot.csv 등급 확률로 등급을 고르고, 그 등급의 장비 중 하나(같은 확률). 없으면 0
	int32_t rollLootWinner();                                   // 파티원 전원이 d20 - 가장 높은 사람, 같으면 그 사람들끼리 다시

	const GameData* mGameData;
	std::vector<DungeonMember> mMembers;
	std::vector<DungeonMonster> mMonsters;
	int32_t mRoomIdx = 0;
	int32_t mActorIdx = 0;
	uint32_t mTurnNo = 0;
	int32_t mRoundInRoom = 0; // 이 방에서 끝난 라운드 수 - 보스 특수 공격 주기
	bool mbOver = false;
	bool mbSuccess = false;
	bool mbExtraLoot = false;
	std::mt19937 mRng;        // 파티 락 안에서만 쓰므로 던전마다 하나
};
