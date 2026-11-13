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
