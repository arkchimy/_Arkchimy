#pragma once
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>
#include "GameData.h"
#include "GameProtocol.h"

// 착용한 장비가 더하는 능력치 - 던전을 시작할 때 읽어 던전 상태에 복사한다(설계 3-9-1)
struct EquipBonus
{
	int32_t Attack;
	int32_t Defense;
	int32_t Investigate;
	int32_t WeaponDamage;
};

// 계정 하나 - 접속 중에는 메모리가 기준(설계 3-3). DB를 붙이기 전까지는 서버를 끄면 사라진다
// 골드·인벤토리·장비는 여러 스레드가 바꿀 수 있어(자기 패킷, 낙찰, 회수) 플레이어 락으로 지킨다(설계 3-1-1)
class Account final
{
public:
	explicit Account(const std::string& id);

	const std::string& GetID() const; // 만든 뒤 바뀌지 않아 락 없이 읽는다
	std::mutex& GetLock();            // 플레이어 락

	// 아래는 모두 플레이어 락을 잡은 채로 부른다
	void FillInventory(InventoryRes* outRes) const;
	void AddItem(const ItemInfo& item); // 칸이 가득 차면 Assert - 부르는 쪽이 IsInventoryFull로 먼저 확인한다
	void AddGold(const int64_t gold);
	bool IsInventoryFull() const;
	bool RemoveOneItemOfKind(const uint16_t kind);                  // 그 종류 아이템 하나를 뺀다(착용 중인 것은 빼지 않는다). 없으면 false
	EquipBonus GetEquipBonus(const GameData& gameData) const;       // 착용한 장비 능력치의 합
	eEquipResult Equip(const uint64_t itemUID, const GameData& gameData); // 그 칸에 착용한 것이 있으면 바꾼다
	eEquipResult Unequip(const eEquipSlot slot);

private:
	std::mutex mLock;
	const std::string mID;
	int64_t mGold = 0;
	std::vector<ItemInfo> mItems;                                    // 최대 INVENTORY_MAX개, 착용한 아이템도 여기에 있다
	uint64_t mEquipped[static_cast<int32_t>(eEquipSlot::Count)]{}; // 칸마다 착용한 아이템의 ItemUID, 0이면 비어 있음
};
