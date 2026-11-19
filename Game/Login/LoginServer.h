#pragma once
#include <atomic>
#include <ostream>
#include <string>
#include <vector>
#include "NetworkLib.h" // 소켓 헤더를 다른 프로젝트 헤더보다 먼저(README_ERRORS 1-26)
#include "IntervalThread.h"
#include "LoginCrypto.h"
#include "LoginProtocol.h"
#include "LoginStorePool.h"
#include "PasswordHash.h"
#include "RsaKeyPool.h"

// 로그인 서버 - 가입·로그인을 받아 비밀번호를 확인하고, 세션 키를 Redis에 넣어 클라에게 준다(설계 login_server.docx)
// 모든 일을 워커가 한다(2026-11-19 사용자 결정 - RSA 에코 서버처럼): RSA 풀기·서명, 비밀번호 해시(Argon2id 수십 ms), MySQL, Redis
// 워커는 요청 하나를 처리하는 동안 DB·Redis 연결 한 쌍을 빌린다(LoginStorePool, 워커 수만큼)
// 대가: 해시나 DB·Redis를 기다리는 동안 그 워커는 다른 연결을 처리하지 못한다 - 워커가 모두 그러면 서버 전체가 멈춘다(DB가 잠기면 그동안)
// 접속하면 먼저 ClientHello → ServerHello로 임시 RSA 공개 키를 주고받고, 그 뒤는 모두 RSA로 잠그고 서명한 패킷(설계 login_rsa_change.docx, LoginCrypto)
// 서버 임시 키 쌍은 수십 ms라 RsaKeyPool이 미리 만들어 둔다(키 쌍 스레드) - 접속 폭주에서 워커가 막히지 않게
class LoginServer final : public NetworkLib
{
public:
	// stores: 워커가 빌려 쓰는 DB·Redis 연결(워커 수 이상), signKey: 서버 고정 키(서명만), keyPool: 서버 임시 키 쌍 - 모두 서버보다 오래 살아야 한다
	// idleLimitSec: 접속하거나 마지막으로 응답한 뒤 이 시간 안에 요청이 없으면 끊는다(0이면 끔) - 아무것도 보내지 않는 연결이 세션 칸을 채우지 못하게
	// ackLimitSec: 로그인 성공 응답(세션 키)을 보낸 뒤 이 시간 안에 AuthAck가 없으면 끊는다(1 이상) - 받으면 바로 끊는다
	LoginServer(const int32_t maxSession, LoginStorePool* stores, const PasswordHasher* hasher, const RsaKey* signKey, RsaKeyPool* keyPool,
		const int32_t idleLimitSec, const int32_t ackLimitSec);
	~LoginServer() override;

	// 기한 감시를 멈춘 뒤 라이브러리를 멈춘다(처리 중인 요청은 워커가 끝낸 뒤 멈춘다)
	void Shutdown();

	int64_t GetBadPacketCnt() const
	{
		return mBadPacketCnt;
	}
	int64_t GetRegisterCnt() const
	{
		return mRegisterCnt;
	}
	int64_t GetAuthOkCnt() const
	{
		return mAuthOkCnt;
	}
	int64_t GetAuthFailCnt() const
	{
		return mAuthFailCnt;
	}
	int32_t GetWorkingCnt() const
	{
		return mWorkingCnt;
	}
	int64_t GetIdleTimeoutCnt() const
	{
		return mIdleTimeoutCnt;
	}
	int64_t GetKeyEmptyCnt() const
	{
		return mKeyEmptyCnt;
	}
	int32_t GetKeyReadyCnt() const
	{
		return mKeyPool->GetReadyCnt();
	}
	int64_t GetKeyCreatedCnt() const
	{
		return mKeyPool->GetCreatedCnt();
	}
	int64_t GetAckCnt() const
	{
		return mAckCnt;
	}
	int64_t GetAckTimeoutCnt() const
	{
		return mAckTimeoutCnt;
	}

protected:
	void OnAccept(const uint64_t id, const SOCKADDR_IN& remoteAddr) override;
	void OnRecv(const uint64_t id, const char* payload, const int32_t len) override;
	void OnRelease(const uint64_t id) override;

private:
	// 연결 상태 - 상태마다 받는 메시지가 정해져 있고, 맞지 않는 메시지가 오면 잘못된 패킷으로 끊는다
	// WaitHello → (ClientHello) → Ready → (가입·로그인 요청) → Pending → 응답 → Ready(가입, 로그인 실패) / KeySent(로그인 성공) / Closing(5번 실패)
	// KeySent → (AuthAck) → Closing, 끊기. loginAckSec 안에 AuthAck가 없어도 끊는다
	enum class eLoginState : uint8_t
	{
		WaitHello = 0, // 접속함 - ClientHello만 받는다
		Ready = 1,     // 임시 키를 나눔 - 가입·로그인 요청만 받는다
		Pending = 2,   // 워커가 요청을 처리하는 중(해시·DB·Redis) - 기한 감시가 끊지 않는다
		KeySent = 3,   // 로그인 성공 응답(세션 키)을 보냄 - AuthAck만 받는다
		Closing = 4,   // 끊기로 함 - 아무것도 받지 않는다
	};

	// 세션마다의 상태 - 세션과 같은 인덱스. 같은 세션의 콜백은 동시에 불리지 않아 워커가 락 없이 쓴다(감시 스레드가 읽는 값만 atomic)
	struct LoginSession
	{
		uint64_t SessionID = 0;
		std::atomic<eLoginState> State = eLoginState::WaitHello; // 워커가 바꾸고 감시 스레드가 읽는다. 바꿀 때는 LastActiveTick을 먼저 쓴다
		bool bRegistered = false;           // 이 연결에서 가입에 성공함 - 한 연결에서 가입은 한 번(두 번째 가입 요청은 끊는다)
		int32_t FailCnt = 0;                // 이 세션에서 틀린 로그인 수
		int32_t KeyUseCnt = 0;              // 빌려 온 서버 임시 키를 이 연결까지 쓴 횟수(0이면 빌리지 않음)
		LoginChannel Channel;               // ClientHello 뒤 준비됨. 받기·보내기 모두 이 세션의 워커만 - 보내는 번호도 락이 없다
		// 감시 스레드가 읽는 값 - 접속하거나 마지막으로 응답한 뒤 기한 안에 요청이 없으면 끊는다(요청 처리 중에는 끊지 않는다)
		std::atomic<uint64_t> WatchSessionID = 0;  // SessionID와 같은 값(감시 스레드는 SessionID를 읽지 않는다). 0이면 빈 칸
		std::atomic<ULONGLONG> LastActiveTick = 0; // 접속, 응답을 보냄(KeySent면 세션 키를 보낸 시각)
	};

	LoginSession& getSession(const uint64_t id);
	void onClientHello(const uint64_t id, const char* payload, const int32_t len);
	void onRecvPacket(const uint64_t id, const char* payload, const int32_t len); // 푼 평문
	void sendSecure(const uint64_t id, const void* data, const int32_t len);      // 암호화해 보낸다 - 그 세션의 워커만
	void onRegisterReq(const uint64_t id, const char* payload, const int32_t len);
	void onAuthReq(const uint64_t id, const char* payload, const int32_t len);
	eRegisterResult registerAccount(LoginStore& store, const std::string& accountID, const std::string& password);
	AuthRes authenticate(LoginStore& store, const std::string& accountID, const std::string& password);
	void disconnectBadPacket(const uint64_t id);
	void onAuthAck(const uint64_t id, const int32_t len);
	void checkIdleDeadline(); // 감시 스레드(1초마다) - 요청 기한(WaitHello·Ready)과 확인 기한(KeySent)

	std::vector<LoginSession> mSessions;
	LoginStorePool* mStores;
	const PasswordHasher* mHasher;
	const RsaKey* mSignKey; // 서버 고정 키 - ServerHello와 응답에 서명
	RsaKeyPool* mKeyPool;   // 서버 임시 키 쌍
	std::atomic<int32_t> mWorkingCnt = 0; // 가입·로그인을 처리 중인 워커 수(통계 - 워커 수와 같으면 다른 연결을 처리할 워커가 없다)
	std::atomic<int64_t> mBadPacketCnt = 0;
	std::atomic<int64_t> mRegisterCnt = 0;
	std::atomic<int64_t> mAuthOkCnt = 0;
	std::atomic<int64_t> mAuthFailCnt = 0;
	std::atomic<int64_t> mIdleTimeoutCnt = 0;
	std::atomic<int64_t> mKeyEmptyCnt = 0;   // 미리 만든 키 쌍이 떨어져 ClientHello를 받지 않고 끊은 수
	std::atomic<int64_t> mAckCnt = 0;        // AuthAck를 받고 끊은 수
	std::atomic<int64_t> mAckTimeoutCnt = 0; // 세션 키를 보낸 뒤 확인 기한 안에 AuthAck가 없어 끊은 수
	ULONGLONG mIdleLimitMs = 0;              // 0이면 요청 기한을 보지 않는다
	ULONGLONG mAckLimitMs = 0;
	IntervalThread mIdleWatch; // 워커 밖 스레드 - 라이브러리 Stop()보다 먼저 멈춘다
};

std::ostream& operator<<(std::ostream& out, const LoginServer& server);
