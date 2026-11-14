#include <ctime>
#include "AccountTable.h"
#include "Assert.h"

bool AccountTable::Has(const std::string& id)
{
	std::lock_guard<std::mutex> lock(mLock);
	return mAccounts.find(id) != mAccounts.end();
}

eLoginResult AccountTable::Login(const std::string& id, const uint64_t sessionID, UniquePtr<Account> loaded, Account** outAccount,
	uint64_t* outOnlineSessionID)
{
	Assert(outAccount != nullptr && outOnlineSessionID != nullptr, "Login: out 매개변수가 null");
	std::lock_guard<std::mutex> lock(mLock);
	auto it = mAccounts.find(id);
	if (it == mAccounts.end())
	{
		Assert(loaded != nullptr, "Login: 표에 없는 계정을 DB에서 읽지 않고 로그인 - Has가 true일 때만 loaded 없이 부른다");
		it = mAccounts.emplace(id, std::move(loaded)).first;
	}
	Account* account = it->second.get();
	{
		std::lock_guard<std::mutex> playerLock(account->GetLock()); // 계정 표 → 플레이어(가장 안쪽) - 차단은 운영 명령이 다른 스레드에서 바꾼다
		if (account->GetBanUntil() > static_cast<int64_t>(time(nullptr)))
		{
			return eLoginResult::Banned;
		}
	}
	const auto onlineIt = mOnline.find(id);
	if (onlineIt != mOnline.end())
	{
		*outOnlineSessionID = onlineIt->second; // 부르는 쪽이 이 세션을 끊는다 - 끊긴 줄 모르는 이전 접속이 아이디를 영영 잡지 않게
		return eLoginResult::AlreadyLoggedIn;   // 확인과 접속 표시를 같은 락 안에서 해 동시 로그인은 한쪽만 성공한다
	}
	mOnline.emplace(id, sessionID);
	*outAccount = account;
	return eLoginResult::Ok;
}

void AccountTable::Logout(const std::string& id, const uint64_t sessionID)
{
	std::lock_guard<std::mutex> lock(mLock);
	const auto it = mOnline.find(id);
	Assert(it != mOnline.end() && it->second == sessionID, "Logout: 이 세션으로 접속한 기록이 없음");
	mOnline.erase(it);
}

Account* AccountTable::Find(const std::string& id)
{
	std::lock_guard<std::mutex> lock(mLock);
	const auto it = mAccounts.find(id);
	return (it != mAccounts.end()) ? it->second.get() : nullptr;
}

uint64_t AccountTable::GetOnlineSessionID(const std::string& id)
{
	std::lock_guard<std::mutex> lock(mLock);
	const auto it = mOnline.find(id);
	return (it != mOnline.end()) ? it->second : 0;
}
