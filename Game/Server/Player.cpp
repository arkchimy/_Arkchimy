#include <sodium.h>
#include "Player.h"
#include "Memory.h"
#include "SessionAuth.h"

void Player::Init(const uint64_t sessionID, const SOCKADDR_IN& remoteAddr)
{
	std::lock_guard<std::mutex> lock(mSendLock); // 이전 세션 ID로 보내던 스레드가 이 칸의 새 세션 순번을 건드리지 않게
	mSessionID = sessionID;
	mRemoteAddr = remoteAddr;
	randombytes_buf(mChallenge, sizeof(mChallenge));
	mCipher.Clear();
	mbSecure = false;
}

void Player::Clear()
{
	std::lock_guard<std::mutex> lock(mSendLock);
	mSessionID = 0;
	mRemoteAddr = SOCKADDR_IN{};
	mAccount = nullptr;
	mbLoginPending = false;
	mPartyID = 0;
	mCipher.Clear();
	mbSecure = false;
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

void Player::GetChallenge(uint8_t (&outChallenge)[ENTER_CHALLENGE_LEN]) const
{
	MemCpy(outChallenge, mChallenge, sizeof(outChallenge));
}

bool Player::IsSecure() const
{
	return mbSecure;
}

std::mutex& Player::GetSendLock()
{
	return mSendLock;
}

bool Player::PrepareSendLocked(const uint64_t sessionID, const char* data, const int32_t len, std::string* outPayload)
{
	Assert(outPayload != nullptr, "PrepareSendLocked: outPayload가 null");
	if (mSessionID != sessionID)
	{
		return false; // 끊겨 칸이 비었거나 다른 세션이 쓴다 - 보내지 않는다(새 세션의 순번을 건드리지 않게)
	}
	if (mbSecure)
	{
		mCipher.Seal(data, static_cast<size_t>(len), outPayload);
	}
	else
	{
		outPayload->assign(data, static_cast<size_t>(len)); // 입장 전(EnterChallenge, 실패한 LoginRes)
	}
	return true;
}

void Player::OpenChannelLocked(const uint8_t (&sessionKey)[SESSION_KEY_LEN])
{
	SetGameChannelKeys(sessionKey, true, &mCipher);
	mbSecure = true;
}

bool Player::OpenReceived(const char* cipher, const int32_t len, std::string* outPlain)
{
	return mCipher.Open(cipher, static_cast<size_t>(len), outPlain);
}
