#pragma once
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>
#include "GameProtocol.h"

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
	void AddItem(const ItemInfo& item); // 칸이 가득 차면 Assert - 가득 찼을 때의 규칙은 전리품 단계에서 정한다
	void AddGold(const int64_t gold);

private:
	std::mutex mLock;
	const std::string mID;
	int64_t mGold = 0;
	std::vector<ItemInfo> mItems;                                    // 최대 INVENTORY_MAX개, 착용한 아이템도 여기에 있다
	uint64_t mEquipped[static_cast<int32_t>(eEquipSlot::Count)]{}; // 칸마다 착용한 아이템의 ItemUID, 0이면 비어 있음
};
