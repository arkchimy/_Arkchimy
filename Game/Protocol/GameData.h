#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "GameProtocol.h"

// 시나리오 데이터(Game/Data의 CSV) - 서버와 클라가 같이 읽는다. 시나리오 내용은 코드가 아니라 이 파일들에 있다(설계 3-9-5)
// 시작할 때 한 번 읽고 그 뒤로는 읽기만 하므로 락이 없다

enum class eItemGrade : uint8_t
{
	Common = 0,
	Rare = 1,
	Epic = 2,
	Count = 3,
};

enum class eRoomType : uint8_t
{
	Investigate = 0, // 조사 대상이 있는 방 - 한 라운드 뒤 다음 방
	Trap = 1,        // 들어가자마자 전원 함정 판정, 바로 다음 방
	Battle = 2,
	Boss = 3,
};

enum class eRoomReward : uint8_t
{
	None = 0,
	Potion = 1,    // 조사에 성공한 사람에게 회복약
	ExtraLoot = 2, // 한 명이라도 조사에 성공하면 보스 전리품 1개 추가
	Loot = 3,      // 보스를 잡으면 전리품 분배
};

struct ItemData
{
	uint16_t Kind;
	std::string Name; // UTF-8 - 화면에 쓸 때는 클라가 콘솔 코드 페이지로 바꾼다
	bool bEquippable;
	eEquipSlot Slot;  // bEquippable일 때만 뜻이 있다
	eItemGrade Grade;
	int32_t Attack;
	int32_t Defense;
	int32_t Investigate;
	int32_t WeaponDamage;
	int32_t Heal;     // 1이면 회복약(0/1만 허용)
};

struct MonsterData
{
	uint16_t Kind;
	std::string Name;
	int32_t HpBase;
	int32_t HpPerMember;  // 체력 = HpBase + HpPerMember × 파티 인원
	int32_t Defense;
	int32_t AttackBonus;
	int32_t DamageDie;    // 피해 d(DamageDie)
	int32_t SpecialEvery; // 0이면 특수 공격 없음, N이면 N라운드마다 전원 피해
	int32_t SpecialDie;
};

struct RoomData
{
	int32_t Room;
	std::string Name;
	eRoomType Type;
	int32_t Difficulty;      // 조사·함정 판정 기준
	uint16_t MonsterKind;    // 0이면 몬스터 없음
	bool bMonsterPerMember;  // true면 몬스터 수 = 파티 인원
	int32_t MonsterCount;    // bMonsterPerMember가 아닐 때의 몬스터 수
	eRoomReward Reward;
};

class GameData final
{
public:
	// dir의 items·monsters·rooms·loot.csv를 읽는다. 실패하면 outError에 "파일 몇째 줄: 이유" - 밖에서 오는 값이라 멈추지 않고 알린다
	bool Load(const std::string& dir, std::string* outError);

	const ItemData* FindItem(const uint16_t kind) const;       // 없으면 nullptr
	const MonsterData* FindMonster(const uint16_t kind) const; // 없으면 nullptr
	const std::vector<ItemData>& GetItems() const;
	const std::vector<RoomData>& GetRooms() const;              // 방 번호 순
	int32_t GetGradeChance(const eItemGrade grade) const;       // 전리품 등급 확률(%)
	uint16_t GetPotionKind() const;                             // 첫 회복약 종류 - Load가 하나는 있는지 확인한다

private:
	uint16_t mPotionKind = 0;
	std::vector<ItemData> mItems;
	std::vector<MonsterData> mMonsters;
	std::vector<RoomData> mRooms;
	int32_t mGradeChance[static_cast<int32_t>(eItemGrade::Count)]{};
};
