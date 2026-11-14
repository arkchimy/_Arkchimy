#include "Player.h"

void Player::Init(const uint64_t sessionID, const SOCKADDR_IN& remoteAddr)
{
	mSessionID = sessionID;
	mRemoteAddr = remoteAddr;
}

void Player::Clear()
{
	mSessionID = 0;
	mRemoteAddr = SOCKADDR_IN{};
	mAccount = nullptr;
	mbLoginPending = false;
	mPartyID = 0;
}

void Player::SetLoginPending(const bool bPending)
{
	mbLoginPending = bPending;
}

bool Player::IsLoginPending() const
{
	return mbLoginPending;
}

void Player::SetPartyID(const uint64_t partyID)
{
	mPartyID = partyID;
}

uint64_t Player::GetPartyID() const
{
	return mPartyID;
}

void Player::SetAccount(Account* account)
{
	mAccount = account;
}

Account* Player::GetAccount() const
{
	return mAccount;
}

uint64_t Player::GetSessionID() const
{
	return mSessionID;
}
