#include "Account.h"

Account::Account(const std::string& id)
	: mID(id)
{
}

const std::string& Account::GetID() const
{
	return mID;
}

std::mutex& Account::GetLock()
{
	return mLock;
}

int64_t Account::GetBanUntil() const
{
	return mBanUntil;
}

void Account::SetBanUntil(const int64_t banUntil)
{
	mBanUntil = banUntil;
}
