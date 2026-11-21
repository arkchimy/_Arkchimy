#include <ctime>
#include "AccountTable.h"
#include "Assert.h"

bool AccountTable::Has(const std::string& id)
{
	std::lock_guard<std::mutex> lock(mLock);
	return mAccounts.find(id) != mAccounts.end();
}

eLoginResult AccountTable::Login(const std::string& id, const uint64_t sessionID, UniquePtr<Account> loaded, Account** outAccount,
	uint64_t* outReplacedSessionID)
{
	Assert(outAccount != nullptr && outReplacedSessionID != nullptr, "Login: out 매개변수가 null");
	*outReplacedSessionID = 0;
	std::lock_guard<std::mutex> lock(mLock);
	auto it = mAccounts.find(id);
	if (it == mAccounts.end())
	{
		Assert(loaded != nullptr, "Login: 표에 없는 계정을 DB에서 읽지 않고 로그인 - Has가 true일 때만 loaded 없이 부른다");
		it = mAccounts.emplace(id, std::move(loaded)).first;
	}
	Account* account = it->second.get();
	{
		std::lock_guard<std::mutex> accountLock(account->GetLock()); // 계정 표 → 계정(가장 안쪽) - 차단은 운영 명령이 바꾼다
		if (account->GetBanUntil() > static_cast<int64_t>(time(nullptr)))
		{
			return eLoginResult::Banned;
		}
	}
	// 같은 계정이 이미 입장해 있으면 접속 표시를 새 세션으로 바꾸고, 기존 세션은 부르는 쪽이 끊는다(설계 2판 4장, 사용자 결정)
	// 확인과 바꾸기를 같은 락 안에서 해 동시에 두 입장이 와도 마지막 하나가 접속 표시를 갖는다. 기존 세션의 Logout은 자기 표시가 아니면 지우지 않는다
	const auto onlineIt = mOnline.find(id);
	if (onlineIt != mOnline.end())
	{
		*outReplacedSessionID = onlineIt->second;
		onlineIt->second = sessionID;
	}
	else
	{
		mOnline.emplace(id, sessionID);
	}
	*outAccount = account;
	return eLoginResult::Ok;
}

void AccountTable::Logout(const std::string& id, const uint64_t sessionID)
{
	std::lock_guard<std::mutex> lock(mLock);
	const auto it = mOnline.find(id);
	if (it != mOnline.end() && it->second == sessionID)
	{
		mOnline.erase(it); // 같은 계정의 새 입장이 이미 표시를 가져갔으면 그대로 둔다
	}
}

Account* AccountTable::Find(const std::string& id)
{
	std::lock_guard<std::mutex> lock(mLock);
	const auto it = mAccounts.find(id);
	return (it != mAccounts.end()) ? it->second.get() : nullptr;
}

int64_t AccountTable::GetOnlineCnt() const
{
	std::lock_guard<std::mutex> lock(mLock);
	return static_cast<int64_t>(mOnline.size());
}

int64_t AccountTable::GetAccountCnt() const
{
	std::lock_guard<std::mutex> lock(mLock);
	return static_cast<int64_t>(mAccounts.size());
}

uint64_t AccountTable::GetOnlineSessionID(const std::string& id)
{
	std::lock_guard<std::mutex> lock(mLock);
	const auto it = mOnline.find(id);
	return (it != mOnline.end()) ? it->second : 0;
}
