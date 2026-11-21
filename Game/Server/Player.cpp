#include "Player.h"
#include "Memory.h"
#include "SessionAuth.h"

void Player::Init(const uint64_t sessionID, const SOCKADDR_IN& remoteAddr)
{
	std::lock_guard<std::mutex> lock(mSendLock); // 이전 세션 ID로 보내던 스레드가 이 칸의 새 세션 순번을 건드리지 않게
	mSessionID = sessionID;
	mRemoteAddr = remoteAddr;
	mAcceptTick = GetTickCount64();
	mCipher.Clear();
	mHelloAccountNo = 0;
	mStartSeq = 0;
	mState = eEntryState::WaitHello;
}

uint64_t Player::GetEntryExpiredSessionID(const ULONGLONG limitMs)
{
	std::lock_guard<std::mutex> lock(mSendLock); // 세션 ID·접속 시각·상태를 Init·Clear·OpenChannelLocked와 같은 락으로 본다
	const eEntryState state = mState;
	if (mSessionID == 0 || state == eEntryState::Entered || state == eEntryState::Pending)
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
	mHelloAccountNo = 0;
	mStartSeq = 0;
	mCipher.Clear();
	mState = eEntryState::WaitHello;
}

void Player::SetAccount(Account* account)
{
	mAccount = account;
}

Account* Player::GetAccount() const
{
	return mAccount;
}

eEntryState Player::GetState() const
{
	return mState;
}

void Player::SetState(const eEntryState state)
{
	mState = state;
}

uint64_t Player::GetHelloAccountNo() const
{
	return mHelloAccountNo;
}

uint64_t Player::GetStartSeq() const
{
	return mStartSeq;
}

uint64_t Player::GetSessionID() const
{
	return mSessionID;
}

std::mutex& Player::GetSendLock()
{
	return mSendLock;
}

void Player::AcceptHelloLocked(const uint64_t accountNo, const uint64_t startSeq)
{
	mHelloAccountNo = accountNo;
	mStartSeq = startSeq;
	mState = eEntryState::WaitEnter;
}

bool Player::PrepareSendLocked(const uint64_t sessionID, const eGameType type, const void* body, const size_t bodyLen, std::string* outPacket)
{
	Assert(outPacket != nullptr, "PrepareSendLocked: outPacket이 null");
	if (mSessionID != sessionID || mState != eEntryState::Entered)
	{
		// 끊겨 칸이 비었거나 다른 세션이 쓴다(새 세션의 순번을 건드리지 않게), 또는 입장 전 - 보내지 않는다
		// 입장 전 세션에 다른 스레드가 보내는 코드가 생겨도 평문으로 새지 않게 버린다(타이밍으로 생길 수 있어 멈추지 않는다)
		return false;
	}
	mCipher.SealPacket(type, body, bodyLen, outPacket);
	return true;
}

bool Player::BeginEntryLocked(const uint8_t (&sessionKey)[SESSION_KEY_LEN], const char* packet, const size_t packetLen)
{
	return OpenEnterRequest(sessionKey, mStartSeq, mHelloAccountNo, packet, packetLen, &mCipher); // 성공하면 받을 SeqNumber = 시작값 + 1
}

void Player::CancelEntryLocked()
{
	mCipher.Clear();
}

void Player::OpenChannelLocked()
{
	Assert(mCipher.IsReady(), "OpenChannelLocked: BeginEntryLocked가 성공하기 전에 통로를 염");
	mState = eEntryState::Entered;
}

bool Player::OpenReceived(const char* packet, const int32_t len, eGameType* outType, std::string* outBody)
{
	return mCipher.OpenPacket(packet, static_cast<size_t>(len), outType, outBody);
}
