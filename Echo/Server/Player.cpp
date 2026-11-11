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
}

uint64_t Player::GetSessionID() const
{
	return mSessionID;
}
