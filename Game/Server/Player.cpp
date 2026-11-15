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
	mAcceptTick = GetTickCount64();
	mCipher.Clear();
	mbSecure = false;
}

uint64_t Player::GetEntryExpiredSessionID(const ULONGLONG limitMs)
{
	std::lock_guard<std::mutex> lock(mSendLock); // 세션 ID·접속 시각·통로를 Init·Clear·OpenChannelLocked와 같은 락으로 본다
	if (mSessionID == 0 || mbSecure || mbLoginPending)
	{
		return 0; // 빈 칸, 이미 입장함, 입장을 DB 스레드가 처리하는 중(대기열이 밀려도 끊지 않는다)
	}
	const ULONGLONG now = GetTickCount64(); // 접속 시각을 읽은 뒤에 - now가 접속 시각보다 앞서면 부호 없는 뺄셈이 넘친다(README_ERRORS 2-8)
	return (now - mAcceptTick > limitMs) ? mSessionID : 0;
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
		return true;
	}
	// 통로가 열리기 전에 평문으로 보낼 수 있는 것은 EnterChallenge·LoginRes뿐 - 나중에 다른 스레드가 입장 직후의 세션에
	// 알림을 보내는 코드가 생겨도 평문으로 새지 않게 버린다(타이밍으로 생길 수 있어 멈추지 않는다)
	eGameType type = eGameType::LoginReq;
	if (len >= static_cast<int32_t>(sizeof(type)))
	{
		MemCpy(&type, data, sizeof(type));
	}
	if (type != eGameType::EnterChallenge && type != eGameType::LoginRes)
	{
		return false;
	}
	outPayload->assign(data, static_cast<size_t>(len));
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
