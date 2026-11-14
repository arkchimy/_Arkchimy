#include "Account.h"
#include "Assert.h"
#include "Memory.h"

Account::Account(const std::string& id)
	: mID(id)
{
	mItems.reserve(INVENTORY_MAX);
}

const std::string& Account::GetID() const
{
	return mID;
}

std::mutex& Account::GetLock()
{
	return mLock;
}

void Account::FillInventory(InventoryRes* outRes) const
{
	Assert(outRes != nullptr, "FillInventory: outRes가 null");
	outRes->Gold = mGold;
	MemCpy(outRes->Equipped, mEquipped, sizeof(mEquipped));
	outRes->ItemCnt = static_cast<uint8_t>(mItems.size());
	for (size_t i = 0; i < mItems.size(); ++i)
	{
		outRes->Items[i] = mItems[i];
	}
}

void Account::AddItem(const ItemInfo& item)
{
	Assert(mItems.size() < INVENTORY_MAX, "AddItem: 인벤토리가 가득 참 - 부르는 쪽이 먼저 확인해야 한다");
	mItems.push_back(item);
}

void Account::AddGold(const int64_t gold)
{
	mGold += gold;
}

bool Account::IsInventoryFull() const
{
	return mItems.size() >= INVENTORY_MAX;
}

bool Account::RemoveOneItemOfKind(const uint16_t kind)
{
	for (auto it = mItems.begin(); it != mItems.end(); ++it)
	{
		if (it->ItemKind != kind)
		{
			continue;
		}
		bool bEquipped = false;
		for (const uint64_t equippedUID : mEquipped)
		{
			bEquipped = bEquipped || (equippedUID == it->ItemUID);
		}
		if (!bEquipped)
		{
			mItems.erase(it);
			return true;
		}
	}
	return false;
}

eEquipResult Account::Equip(const uint64_t itemUID, const GameData& gameData)
{
	for (const ItemInfo& item : mItems)
	{
		if (item.ItemUID != itemUID)
		{
			continue;
		}
		const ItemData* data = gameData.FindItem(item.ItemKind);
		if (data == nullptr || !data->bEquippable)
		{
			return eEquipResult::NotEquippable;
		}
		mEquipped[static_cast<int32_t>(data->Slot)] = itemUID;
		return eEquipResult::Ok;
	}
	return eEquipResult::NotFound;
}

eEquipResult Account::Unequip(const eEquipSlot slot)
{
	const int32_t slotIdx = static_cast<int32_t>(slot);
	if (slotIdx < 0 || slotIdx >= static_cast<int32_t>(eEquipSlot::Count))
	{
		return eEquipResult::BadSlot; // 밖에서 온 칸 번호
	}
	mEquipped[slotIdx] = 0;
	return eEquipResult::Ok;
}

EquipBonus Account::GetEquipBonus(const GameData& gameData) const
{
	EquipBonus bonus{};
	for (const uint64_t equippedUID : mEquipped)
	{
		if (equippedUID == 0)
		{
			continue;
		}
		for (const ItemInfo& item : mItems)
		{
			const ItemData* data = (item.ItemUID == equippedUID) ? gameData.FindItem(item.ItemKind) : nullptr;
			if (data != nullptr)
			{
				bonus.Attack += data->Attack;
				bonus.Defense += data->Defense;
				bonus.Investigate += data->Investigate;
				bonus.WeaponDamage += data->WeaponDamage;
			}
		}
	}
	return bonus;
}
