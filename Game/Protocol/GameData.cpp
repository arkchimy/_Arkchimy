#include <charconv>
#include <fstream>
#include <sstream>
#include "GameData.h"

// 서버와 클라가 같이 쓰므로 라이브러리 헤더(Assert.h, Memory.h)에 기대지 않는다 - 잘못된 값은 모두 outError로 알린다

namespace
{
	using CsvRow = std::vector<std::string>;

	// 파일 전체를 줄·쉼표로 나눈다. 첫 줄(열 이름)은 열 개수 확인에만 쓰고 결과에서 뺀다
	// 문자열에 쉼표를 쓰지 않는 단순 CSV만 읽는다(설계 3-9-5)
	bool readCsv(const std::string& path, const size_t columnCnt, std::vector<CsvRow>* outRows, std::string* outError)
	{
		std::ifstream file(path, std::ios::binary);
		if (!file)
		{
			*outError = path + ": 파일을 열 수 없음";
			return false;
		}
		std::stringstream buffer;
		buffer << file.rdbuf();
		std::string text = buffer.str();
		if (text.size() >= 3 && text.compare(0, 3, "\xEF\xBB\xBF") == 0)
		{
			text.erase(0, 3); // 편집기가 붙인 UTF-8 BOM
		}

		std::istringstream lines(text);
		std::string line;
		int32_t lineNo = 0;
		bool bHeader = true;
		while (std::getline(lines, line))
		{
			++lineNo;
			if (!line.empty() && line.back() == '\r')
			{
				line.pop_back();
			}
			if (line.empty())
			{
				continue;
			}
			CsvRow row;
			std::istringstream cells(line);
			std::string cell;
			while (std::getline(cells, cell, ','))
			{
				row.push_back(cell);
			}
			if (row.size() != columnCnt)
			{
				*outError = path + " " + std::to_string(lineNo) + "째 줄: 열이 " + std::to_string(columnCnt) + "개여야 하는데 " + std::to_string(row.size()) + "개";
				return false;
			}
			if (bHeader)
			{
				bHeader = false;
				continue;
			}
			outRows->push_back(row);
		}
		return true;
	}

	bool toInt(const std::string& text, int32_t* outValue)
	{
		const char* const end = text.data() + text.size();
		const std::from_chars_result result = std::from_chars(text.data(), end, *outValue);
		return result.ec == std::errc() && result.ptr == end;
	}

	// 여러 칸을 숫자로 읽는다 - 하나라도 숫자가 아니면 false
	bool toInts(const CsvRow& row, const std::vector<size_t>& columns, std::vector<int32_t>* outValues)
	{
		for (const size_t column : columns)
		{
			int32_t value = 0;
			if (!toInt(row[column], &value))
			{
				return false;
			}
			outValues->push_back(value);
		}
		return true;
	}

	bool toSlot(const std::string& text, bool* outbEquippable, eEquipSlot* outSlot)
	{
		*outbEquippable = true;
		if (text == "Weapon")
		{
			*outSlot = eEquipSlot::Weapon;
		}
		else if (text == "Head")
		{
			*outSlot = eEquipSlot::Head;
		}
		else if (text == "Accessory")
		{
			*outSlot = eEquipSlot::Accessory;
		}
		else if (text == "Armor")
		{
			*outSlot = eEquipSlot::Armor;
		}
		else if (text == "None")
		{
			*outbEquippable = false;
			*outSlot = eEquipSlot::Weapon;
		}
		else
		{
			return false;
		}
		return true;
	}

	bool toGrade(const std::string& text, eItemGrade* outGrade)
	{
		if (text == "Common")
		{
			*outGrade = eItemGrade::Common;
		}
		else if (text == "Rare")
		{
			*outGrade = eItemGrade::Rare;
		}
		else if (text == "Epic")
		{
			*outGrade = eItemGrade::Epic;
		}
		else
		{
			return false;
		}
		return true;
	}

	bool toRoomType(const std::string& text, eRoomType* outType)
	{
		if (text == "Investigate")
		{
			*outType = eRoomType::Investigate;
		}
		else if (text == "Trap")
		{
			*outType = eRoomType::Trap;
		}
		else if (text == "Battle")
		{
			*outType = eRoomType::Battle;
		}
		else if (text == "Boss")
		{
			*outType = eRoomType::Boss;
		}
		else
		{
			return false;
		}
		return true;
	}

	bool toRoomReward(const std::string& text, eRoomReward* outReward)
	{
		if (text == "None")
		{
			*outReward = eRoomReward::None;
		}
		else if (text == "Potion")
		{
			*outReward = eRoomReward::Potion;
		}
		else if (text == "ExtraLoot")
		{
			*outReward = eRoomReward::ExtraLoot;
		}
		else if (text == "Loot")
		{
			*outReward = eRoomReward::Loot;
		}
		else
		{
			return false;
		}
		return true;
	}

	bool inRange(const int32_t value, const int32_t minValue, const int32_t maxValue)
	{
		return value >= minValue && value <= maxValue;
	}

	std::string badValue(const std::string& file, const size_t rowIdx)
	{
		return file + " " + std::to_string(rowIdx + 2) + "째 줄: 값이 잘못됨"; // +2 = 열 이름 줄 + 1부터 세기
	}
}

// 값 범위 - 판정·피해가 프로토콜 필드(int8 보너스, uint8 기준값, int16 체력·피해)를 넘지 않게 작게 묶는다
// 플레이어 쪽 합(규칙 기본값 + 장비 4칸)도 같은 필드에 들어가므로 server.cfg 규칙도 같은 범위로 확인한다(main의 loadGameRule)
enum : int32_t
{
	STAT_MAX = 20,          // 아이템 능력치·몬스터 공격 보너스·주사위 면 수
	DEFENSE_MAX = 40,       // 몬스터 방어·방 난이도
	MONSTER_HP_MAX = 1000,  // HpBase, HpPerMember 각각 - 합(최대 5000)이 int16 안
};

bool GameData::Load(const std::string& dir, std::string* outError)
{
	if (outError == nullptr)
	{
		return false;
	}
	std::vector<CsvRow> rows;
	const std::string itemsPath = dir + "/items.csv";
	if (!readCsv(itemsPath, 9, &rows, outError))
	{
		return false;
	}
	for (size_t i = 0; i < rows.size(); ++i)
	{
		const CsvRow& row = rows[i];
		std::vector<int32_t> values;
		ItemData item{};
		if (!toInts(row, { 0, 4, 5, 6, 7, 8 }, &values) || !toSlot(row[2], &item.bEquippable, &item.Slot) || !toGrade(row[3], &item.Grade)
			|| !inRange(values[0], 1, UINT16_MAX) || !inRange(values[1], 0, STAT_MAX) || !inRange(values[2], 0, STAT_MAX)
			|| !inRange(values[3], 0, STAT_MAX) || !inRange(values[4], 0, STAT_MAX) || !inRange(values[5], 0, 1) || FindItem(static_cast<uint16_t>(values[0])) != nullptr)
		{
			*outError = badValue(itemsPath, i) + " (번호 1~65535·겹치지 않음, 능력치 0~" + std::to_string(STAT_MAX) + ", Heal 0/1)";
			return false;
		}
		item.Kind = static_cast<uint16_t>(values[0]);
		item.Name = row[1];
		item.Attack = values[1];
		item.Defense = values[2];
		item.Investigate = values[3];
		item.WeaponDamage = values[4];
		item.Heal = values[5];
		mItems.push_back(item);
	}

	rows.clear();
	const std::string monstersPath = dir + "/monsters.csv";
	if (!readCsv(monstersPath, 9, &rows, outError))
	{
		return false;
	}
	for (size_t i = 0; i < rows.size(); ++i)
	{
		const CsvRow& row = rows[i];
		std::vector<int32_t> values;
		// 순서: Kind, HpBase, HpPerMember, Defense, AttackBonus, DamageDie, SpecialEvery, SpecialDie
		if (!toInts(row, { 0, 2, 3, 4, 5, 6, 7, 8 }, &values) || !inRange(values[0], 1, UINT16_MAX) || !inRange(values[1], 1, MONSTER_HP_MAX)
			|| !inRange(values[2], 0, MONSTER_HP_MAX) || !inRange(values[3], 0, DEFENSE_MAX) || !inRange(values[4], 0, STAT_MAX)
			|| !inRange(values[5], 1, STAT_MAX) || !inRange(values[6], 0, STAT_MAX) || !inRange(values[7], (values[6] > 0) ? 1 : 0, STAT_MAX)
			|| FindMonster(static_cast<uint16_t>(values[0])) != nullptr)
		{
			// 체력 0 이하 몬스터는 방에서 못 나가게 되고, 특수 공격 주기가 있는데 주사위가 0이면 굴릴 수 없다
			*outError = badValue(monstersPath, i) + " (체력 1 이상, 주사위 1~" + std::to_string(STAT_MAX) + ", 특수 공격이 있으면 SpecialDie 1 이상)";
			return false;
		}
		mMonsters.push_back(MonsterData{ static_cast<uint16_t>(values[0]), row[1], values[1], values[2], values[3], values[4], values[5], values[6], values[7] });
	}

	rows.clear();
	const std::string roomsPath = dir + "/rooms.csv";
	if (!readCsv(roomsPath, 7, &rows, outError))
	{
		return false;
	}
	for (size_t i = 0; i < rows.size(); ++i)
	{
		const CsvRow& row = rows[i];
		std::vector<int32_t> values;
		RoomData room{};
		room.bMonsterPerMember = (row[5] == "PartySize");
		if (!toInts(row, { 0, 3, 4 }, &values) || !toRoomType(row[2], &room.Type) || !toRoomReward(row[6], &room.Reward)
			|| (!room.bMonsterPerMember && !toInt(row[5], &room.MonsterCount)) || values[0] != static_cast<int32_t>(i) + 1
			|| !inRange(values[1], 0, DEFENSE_MAX) || !inRange(values[2], 0, UINT16_MAX))
		{
			*outError = badValue(roomsPath, i) + " (방 번호는 1부터 차례로, 난이도 0~" + std::to_string(DEFENSE_MAX) + ")";
			return false;
		}
		if (room.Type == eRoomType::Boss && i + 1 != rows.size())
		{
			*outError = badValue(roomsPath, i) + " (보스 방은 마지막에만 - 보스를 잡으면 던전이 끝난다)";
			return false;
		}
		room.Room = values[0];
		room.Name = row[1];
		room.Difficulty = values[1];
		room.MonsterKind = static_cast<uint16_t>(values[2]);
		const bool bHasMonster = (room.Type == eRoomType::Battle || room.Type == eRoomType::Boss);
		const int32_t monsterCnt = room.bMonsterPerMember ? PARTY_MEMBER_MAX : room.MonsterCount;
		if (bHasMonster && (FindMonster(room.MonsterKind) == nullptr || monsterCnt <= 0 || monsterCnt > DUNGEON_MONSTER_MAX))
		{
			*outError = badValue(roomsPath, i) + " (없는 몬스터이거나 몬스터 수가 1~" + std::to_string(DUNGEON_MONSTER_MAX) + "이 아님)"; // 문자열에는 – 대신 ~ (CP949로 실행 문자열이 만들어짐)
			return false;
		}
		mRooms.push_back(room);
	}

	rows.clear();
	const std::string lootPath = dir + "/loot.csv";
	if (!readCsv(lootPath, 2, &rows, outError))
	{
		return false;
	}
	int32_t chanceSum = 0;
	bool bGradeSeen[static_cast<int32_t>(eItemGrade::Count)] = {};
	for (size_t i = 0; i < rows.size(); ++i)
	{
		eItemGrade grade;
		int32_t chance = 0;
		if (!toGrade(rows[i][0], &grade) || !toInt(rows[i][1], &chance) || !inRange(chance, 0, 100) || bGradeSeen[static_cast<int32_t>(grade)])
		{
			*outError = badValue(lootPath, i) + " (등급이 겹치거나 확률이 0~100이 아님)"; // 같은 등급 두 줄이면 합은 맞아도 확률이 틀어진다
			return false;
		}
		bGradeSeen[static_cast<int32_t>(grade)] = true;
		mGradeChance[static_cast<int32_t>(grade)] = chance;
		chanceSum += chance;
	}
	if (chanceSum != 100)
	{
		*outError = lootPath + ": 확률의 합이 100이 아님(" + std::to_string(chanceSum) + ")";
		return false;
	}
	if (mRooms.empty() || mRooms.back().Type != eRoomType::Boss)
	{
		*outError = roomsPath + ": 마지막 방이 보스 방이어야 함";
		return false;
	}
	for (const ItemData& item : mItems)
	{
		if (item.Heal > 0)
		{
			mPotionKind = item.Kind; // 첫 회복약 - 시작 아이템·조사 보상·회복약 사용이 모두 이 종류
			break;
		}
	}
	if (mPotionKind == 0)
	{
		*outError = itemsPath + ": 회복약(Heal 1) 아이템이 하나는 있어야 함";
		return false;
	}
	return true;
}

const ItemData* GameData::FindItem(const uint16_t kind) const
{
	for (const ItemData& item : mItems)
	{
		if (item.Kind == kind)
		{
			return &item;
		}
	}
	return nullptr;
}

const MonsterData* GameData::FindMonster(const uint16_t kind) const
{
	for (const MonsterData& monster : mMonsters)
	{
		if (monster.Kind == kind)
		{
			return &monster;
		}
	}
	return nullptr;
}

const std::vector<ItemData>& GameData::GetItems() const
{
	return mItems;
}

const std::vector<RoomData>& GameData::GetRooms() const
{
	return mRooms;
}

uint16_t GameData::GetPotionKind() const
{
	return mPotionKind;
}

int32_t GameData::GetGradeChance(const eItemGrade grade) const
{
	return mGradeChance[static_cast<int32_t>(grade)];
}
