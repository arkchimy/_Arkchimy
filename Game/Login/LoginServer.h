#pragma once
#include <atomic>
#include <mutex>
#include <ostream>
#include <string>
#include <vector>
#include "NetworkLib.h" // 소켓 헤더를 다른 프로젝트 헤더보다 먼저(README_ERRORS 1-26)
#include "DbThread.h"
#include "IntervalThread.h"
#include "LoginProtocol.h"
#include "PasswordHash.h"
#include "RedisClient.h"
#include "SecureChannel.h"

// 로그인 서버 - 가입·로그인을 받아 비밀번호를 확인하고, 세션 키를 Redis에 넣어 클라에게 준다(설계 login_server.docx)
// 워커는 패킷을 확인해 인증 스레드(DbThread 재사용 - 스레드 하나 + 요청 대기열)에 넘기기만 한다. 해시·MySQL·Redis는 모두 인증 스레드에서
// Redis 연결은 인증 스레드만 쓴다
// 접속하면 먼저 ClientHello → ServerHello로 키를 나누고, 그 뒤는 모두 암호문(설계 5-1, SecureChannel)
class LoginServer final : public NetworkLib
{
public:
	// idleLimitSec: 접속하거나 마지막으로 응답한 뒤 이 시간 안에 요청이 없으면 끊는다(0이면 끔) - 아무것도 보내지 않는 연결이 세션 칸을 채우지 못하게
	LoginServer(const int32_t maxSession, DbThread* authThread, RedisClient* redis, const PasswordHasher* hasher, const uint8_t (&signSecretKey)[SIGN_SECRET_KEY_LEN],
		const int32_t idleLimitSec);
	~LoginServer() override;

	// 새 패킷을 막고 → 인증 스레드가 남은 요청을 끝내게 한 뒤 → 라이브러리를 멈춘다(인증 스레드가 응답·UnholdSession을 하므로 라이브러리보다 먼저)
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
	int64_t GetAuthPendingCnt() const;
	int64_t GetIdleTimeoutCnt() const
	{
		return mIdleTimeoutCnt;
	}
	int64_t GetSkippedCnt() const
	{
		return mSkippedCnt;
	}

protected:
	void OnAccept(const uint64_t id, const SOCKADDR_IN& remoteAddr) override;
	void OnRecv(const uint64_t id, const char* payload, const int32_t len) override;
	void OnRelease(const uint64_t id) override;

private:
	// 세션마다의 상태 - 세션과 같은 인덱스. 요청 중(bPending)에는 인증 스레드만 만진다
	struct LoginSession
	{
		uint64_t SessionID = 0;
		std::atomic<bool> bPending = false; // 요청을 인증 스레드에 넘겼고 응답 전 - 그동안 온 요청은 잘못된 패킷
		int32_t FailCnt = 0;                // 이 세션에서 틀린 로그인 수
		ChannelCipher Cipher;               // 키를 나눈 뒤 준비됨. 받기(Open)는 워커만, 보내기(Seal)는 SendLock 안에서. 키 설정(ClientHello)과 지우기(OnAccept·OnRelease)는
		                                    // 락 없이 워커가 한다 - 그때는 요청이 없어(첫 패킷, 또는 HoldSession으로 인증 스레드가 끝난 뒤) 보내는 스레드가 없다
		std::mutex SendLock;                // 순번 매기기 + 암호화 + SendPacket을 한 번에 - 워커와 인증 스레드가 보내도 순번 순서대로 송신 링에 들어가게
		// 감시 스레드가 읽는 값 - 접속하거나 마지막으로 응답한 뒤 기한 안에 요청이 없으면 끊는다(요청 중에는 끊지 않는다)
		std::atomic<uint64_t> WatchSessionID = 0;  // SessionID와 같은 값(감시 스레드는 SessionID를 읽지 않는다). 0이면 빈 칸
		std::atomic<ULONGLONG> LastActiveTick = 0; // 접속, 요청을 받음, 응답을 보냄
	};

	LoginSession& getSession(const uint64_t id);
	void onClientHello(const uint64_t id, const char* payload, const int32_t len);
	void onRecvPacket(const uint64_t id, const char* payload, const int32_t len); // 푼 평문
	void sendSecure(const uint64_t id, const void* data, const int32_t len);      // 암호화해 보낸다 - 아무 스레드에서나
	void onRegisterReq(const uint64_t id, const char* payload, const int32_t len);
	void onAuthReq(const uint64_t id, const char* payload, const int32_t len);
	eRegisterResult registerOnAuthThread(DbConn& conn, const std::string& accountID, const std::string& password);
	AuthRes authOnAuthThread(DbConn& conn, const std::string& accountID, const std::string& password);
	void disconnectBadPacket(const uint64_t id);
	void checkIdleDeadline(); // 감시 스레드(1초마다)
	bool skipIfDisconnected(const uint64_t id); // 인증 스레드: 클라가 이미 끊었으면 표시를 내리고 Unhold한 뒤 true(요청을 처리하지 않는다)

	std::vector<LoginSession> mSessions;
	DbThread* mAuthThread;
	RedisClient* mRedis;          // 인증 스레드만
	const PasswordHasher* mHasher;
	uint8_t mSignSecretKey[SIGN_SECRET_KEY_LEN]; // 서버 고정 키 - ServerHello 서명. 출력하지 않는다
	std::atomic<bool> mbClosing = false;
	std::atomic<int32_t> mRecvInProgressCnt = 0;
	std::atomic<int64_t> mBadPacketCnt = 0;
	std::atomic<int64_t> mRegisterCnt = 0;
	std::atomic<int64_t> mAuthOkCnt = 0;
	std::atomic<int64_t> mAuthFailCnt = 0;
	std::atomic<int64_t> mIdleTimeoutCnt = 0;
	std::atomic<int64_t> mSkippedCnt = 0;
	ULONGLONG mIdleLimitMs = 0;
	IntervalThread mIdleWatch; // 워커 밖 스레드 - 라이브러리 Stop()보다 먼저 멈춘다
};

std::ostream& operator<<(std::ostream& out, const LoginServer& server);
