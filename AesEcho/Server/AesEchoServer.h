#pragma once
#include <atomic>
#include <mutex>
#include <ostream>
#include <string>
#include <unordered_map>
#include <vector>
#include "NetworkLib.h"
#include "AesEchoProtocol.h"
#include "LoginCrypto.h"
#include "ReusableKeyPool.h"

// 연결 하나 - 같은 세션의 콜백(OnAccept·OnRecv·OnRelease)은 동시에 불리지 않아 락이 없다
struct EchoSession
{
	uint64_t SessionID = 0;
	LoginChannel Channel; // HelloServer 뒤 준비됨 - 가입·로그인을 RSA로 주고받는다
	AesChannel Aes;       // 로그인 성공 뒤 준비됨 - 에코는 이것으로만
	int32_t KeyUseCnt = 0; // 빌려 온 서버 임시 키를 이 세션까지 쓴 횟수 - 반납할 때 풀이 바꿀지 정한다
	bool LoggedIn = false;
	uint64_t AccountNo = 0;
};

// 계정 하나 - 서버 메모리에만(측정용, 비밀번호를 해시하지 않는다)
struct EchoAccount
{
	std::string Password;
	uint64_t AccountNo = 0;
};

// AES 에코 서버 - RSA 에코 서버의 비교군. 가입·로그인은 RSA(같은 코드), 로그인 응답에 세션 키를 실어 주고 에코는 AES-256-GCM
class AesEchoServer final : public NetworkLib
{
public:
	explicit AesEchoServer(const int32_t maxSession); // EchoSession 배열을 maxSession개 - Start에 넘길 MaxSession과 같은 값
	~AesEchoServer() override;

	// 서버 고정 키를 secretCfgPath의 signSecretKey에서 읽는다. Start 전에
	bool LoadSignKey(const char* secretCfgPath);

	int64_t GetEchoCnt() const
	{
		return mEchoCnt;
	}
	int64_t GetHelloCnt() const
	{
		return mHelloCnt;
	}
	int64_t GetRegisterCnt() const
	{
		return mRegisterCnt;
	}
	int64_t GetLoginOkCnt() const
	{
		return mLoginOkCnt;
	}
	int64_t GetLoginFailCnt() const
	{
		return mLoginFailCnt;
	}
	int64_t GetBadPacketCnt() const
	{
		return mBadPacketCnt;
	}
	int64_t GetKeyEmptyCnt() const
	{
		return mKeyEmptyCnt;
	}
	int32_t GetReadyKeyCnt() const
	{
		return mKeyPool.GetReadyCnt();
	}
	int32_t GetLentKeyCnt() const
	{
		return mKeyPool.GetLentCnt();
	}
	int64_t GetCreatedKeyCnt() const
	{
		return mKeyPool.GetCreatedCnt();
	}
	int64_t GetReplacedKeyCnt() const
	{
		return mKeyPool.GetReplacedCnt();
	}

protected:
	void OnAccept(const uint64_t id, const SOCKADDR_IN& remoteAddr) override;
	void OnRecv(const uint64_t id, const char* payload, const int32_t len) override;
	void OnRelease(const uint64_t id) override;

private:
	EchoSession& getSession(const uint64_t id);
	void onHelloServer(const uint64_t id, EchoSession& session, const char* payload, const int32_t len);
	void onRsaMsg(const uint64_t id, EchoSession& session, const char* payload, const int32_t len); // 가입·로그인 - RSA로 풀기 → 처리 → 잠가 보내기
	void onAesEcho(const uint64_t id, EchoSession& session, const char* payload, const int32_t len); // 에코 - AES로 풀기 → 종류만 바꿔 → 잠가 보내기
	bool handleRsaPlain(EchoSession& session, const std::string& plain, std::string* outReply);       // false = 잘못된 메시지(끊는다)
	bool onRegister(EchoSession& session, const std::string& plain, std::string* outReply);
	bool onLogin(EchoSession& session, const std::string& plain, std::string* outReply);
	bool addAccount(const std::string& accountID, const std::string& password);
	bool checkAccount(const std::string& accountID, const std::string& password, uint64_t* outAccountNo);
	void disconnectBadPacket(const uint64_t id);

	std::vector<EchoSession> mSessions;
	std::unordered_map<std::string, EchoAccount> mAccounts; // 아이디 → 계정
	std::mutex mAccountLock;                                // mAccounts·mNextAccountNo - 여러 워커가 가입·로그인
	uint64_t mNextAccountNo = 1;
	RsaKey mSignKey;          // 서버 고정 키 - 모든 세션이 HelloClient·응답 서명에 함께 쓴다(서명은 락 없이 동시에 - LoadSignKey 뒤 바꾸지 않는다)
	ReusableKeyPool mKeyPool; // 서버 임시 키 쌍 - RSA 에코와 같은 풀
	std::atomic<int64_t> mEchoCnt = 0;
	std::atomic<int64_t> mHelloCnt = 0;
	std::atomic<int64_t> mRegisterCnt = 0;
	std::atomic<int64_t> mLoginOkCnt = 0;
	std::atomic<int64_t> mLoginFailCnt = 0;
	std::atomic<int64_t> mBadPacketCnt = 0;
	std::atomic<int64_t> mKeyEmptyCnt = 0;
};

// 모니터링 한 줄(줄바꿈은 부르는 쪽이)
std::ostream& operator<<(std::ostream& out, const AesEchoServer& server);
