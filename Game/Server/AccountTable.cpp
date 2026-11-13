#include "AccountTable.h"
#include "Assert.h"

enum : int32_t
{
	START_GOLD = 100,
	START_POTION_KIND = 1, // 회복약 - 아이템 데이터 파일을 만들면 그쪽으로 옮긴다
};

eLoginResult AccountTable::Login(const std::string& id, const uint64_t sessionID, Account** outAccount)
{
	Assert(outAccount != nullptr, "Login: outAccount가 null");
	std::lock_guard<std::mutex> lock(mLock);
	if (mOnline.find(id) != mOnline.end())
	{
		return eLoginResult::AlreadyLoggedIn; // 확인과 접속 표시를 같은 락 안에서 해 동시 로그인은 한쪽만 성공한다
	}

	auto it = mAccounts.find(id);
	if (it == mAccounts.end())
	{
		UniquePtr<Account> account = MakeUnique<Account>(id);
		{
			std::lock_guard<std::mutex> accountLock(account->GetLock()); // 아직 아무도 모르는 계정이지만 Account의 약속(락을 잡고 고친다)을 지킨다
			account->AddGold(START_GOLD);
			account->AddItem(ItemInfo{ newItemUID(), static_cast<uint16_t>(START_POTION_KIND) });
		}
		it = mAccounts.emplace(id, std::move(account)).first;
	}
	mOnline.emplace(id, sessionID);
	*outAccount = it->second.get();
	return eLoginResult::Ok;
}

void AccountTable::Logout(const std::string& id, const uint64_t sessionID)
{
	std::lock_guard<std::mutex> lock(mLock);
	const auto it = mOnline.find(id);
	Assert(it != mOnline.end() && it->second == sessionID, "Logout: 이 세션으로 접속한 기록이 없음");
	mOnline.erase(it);
}

uint64_t AccountTable::newItemUID()
{
	return mNextItemUID.fetch_add(1);
}
