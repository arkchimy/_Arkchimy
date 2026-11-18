#pragma once
#include <atomic>
#include <mutex>
#include <ostream>
#include <string>
#include <unordered_map>
#include <vector>
#include "NetworkLib.h"
#include "RsaEchoProtocol.h"
#include "LoginCrypto.h"
#include "ReusableKeyPool.h"

// 연결 하나 - 같은 세션의 콜백(OnAccept·OnRecv·OnRelease)은 동시에 불리지 않아 락이 없다
struct EchoSession
{
	uint64_t SessionID = 0;
	LoginChannel Channel; // ClientHello 뒤 준비됨. Open·Seal 모두 그 세션의 OnRecv(워커)에서만
	int32_t KeyUseCnt = 0; // 빌려 온 서버 임시 키를 이 세션까지 쓴 횟수 - 반납할 때 풀이 바꿀지 정한다
	bool LoggedIn = false; // 로그인한 뒤에만 에코를 받는다
	uint64_t AccountNo = 0;
};

// 계정 하나 - 서버 메모리에만(측정용, 비밀번호를 해시하지 않는다)
struct EchoAccount
{
	std::string Password;
	uint64_t AccountNo = 0;
};

// RSA 에코 서버 - 로그인 서버와 같은 RSA 흐름으로 가입·로그인·에코를 받아 응답을 잠가 돌려준다. 서버 쪽 RSA 비용 측정용(ProfilerLib)
// 워커에서 바로 RSA를 계산한다(로그인 서버는 요청을 인증 스레드로 넘기지만, 여기서는 연산 시간만 본다)
// 측정 태그(작업 폴더의 RsaEchoServer_<시각>_Profiler.txt):
//   Handshake total = ClientHello 처리 전체 ⊃ Handshake take key(서버 임시 키 꺼내기, 비었으면 만들기) + Handshake sign(ServerHello 서명)
//   Request total   = 잠근 요청(가입·로그인·에코) 하나 처리 전체 ⊃ Request decrypt(클라 서명 확인 + 풀기)
//                     + Register/Login/Echo logic + Response encrypt(잠그기 + 서버 고정 키 서명) + Response send
//   RSA 비용 = Request decrypt + Response encrypt, 나머지가 RSA 없이도 드는 일
class RsaEchoServer final : public NetworkLib
{
public:
	explicit RsaEchoServer(const int32_t maxSession); // EchoSession 배열을 maxSession개 - Start에 넘길 MaxSession과 같은 값
	~RsaEchoServer() override;

	// 서버 고정 키를 secretCfgPath(rsa_echo.cfg)의 signSecretKey에서 읽는다. Start 전에
	bool LoadSignKey(const char* secretCfgPath);

	int64_t GetEchoCnt() const
	{
		return mEchoCnt;
	}
	int64_t GetHelloCnt() const
	{
		return mHelloCnt;
	}
	int64_t GetRegisterCnt() const // 가입 요청(결과와 상관없이)
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
	int64_t GetKeyEmptyCnt() const // 키를 만들지 못해(CNG 실패) 끊은 수
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
	void onClientHello(const uint64_t id, EchoSession& session, const char* payload, const int32_t len);
	void onSealedMsg(const uint64_t id, EchoSession& session, const char* payload, const int32_t len); // 풀기 → 종류별 처리 → 잠가 보내기
	bool handlePlain(EchoSession& session, const std::string& plain, std::string* outReply);          // false = 잘못된 메시지(끊는다)
	bool onRegister(EchoSession& session, const std::string& plain, std::string* outReply);
	bool onLogin(EchoSession& session, const std::string& plain, std::string* outReply);
	bool onEcho(EchoSession& session, const std::string& plain, std::string* outReply);
	bool addAccount(const std::string& accountID, const std::string& password);                            // 이미 있으면 false
	bool checkAccount(const std::string& accountID, const std::string& password, uint64_t* outAccountNo);  // 없거나 틀리면 false
	void disconnectBadPacket(const uint64_t id);

	std::vector<EchoSession> mSessions;
	std::unordered_map<std::string, EchoAccount> mAccounts; // 아이디 → 계정
	std::mutex mAccountLock;                                // mAccounts·mNextAccountNo - 여러 워커가 가입·로그인
	uint64_t mNextAccountNo = 1;
	RsaKey mSignKey;          // 서버 고정 키 - 모든 세션이 ServerHello·응답 서명에 함께 쓴다(키 안의 락으로 한 번에 하나씩)
	ReusableKeyPool mKeyPool; // 서버 임시 키 쌍 - 세션이 빌려 쓰고 반납, 5번 쓴 키는 버린다. 비었으면 워커가 새로 만든다
	std::atomic<int64_t> mEchoCnt = 0;
	std::atomic<int64_t> mHelloCnt = 0;
	std::atomic<int64_t> mRegisterCnt = 0;
	std::atomic<int64_t> mLoginOkCnt = 0;
	std::atomic<int64_t> mLoginFailCnt = 0;
	std::atomic<int64_t> mBadPacketCnt = 0;
	std::atomic<int64_t> mKeyEmptyCnt = 0;
};

// 모니터링 한 줄(줄바꿈은 부르는 쪽이)
std::ostream& operator<<(std::ostream& out, const RsaEchoServer& server);
