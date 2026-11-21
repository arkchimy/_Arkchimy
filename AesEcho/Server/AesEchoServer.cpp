#include "AesEchoServer.h" // 맨 앞 - NetworkLib.h(WinSock2.h)가 Windows.h보다 먼저 와야 한다(README_ERRORS 1-26)
#include <bcrypt.h>        // BCryptGenRandom(세션 키)
#include <cstring>
#include <memory>
#include "ParserLib.h"
#include "Profiler.h" // NetworkLib.h(WinSock2.h) 뒤에 - Windows.h가 먼저 들어오면 재정의 오류(README_ERRORS 1-26)

AesEchoServer::AesEchoServer(const int32_t maxSession)
	: mSessions(maxSession)
	, mKeyPool(RSA_ECHO_KEY_MAX_USE) // RSA 에코와 같게 - 빈 채로 시작해 워커가 필요할 때 만든다
{
}

AesEchoServer::~AesEchoServer()
{
	Stop(); // 파생 소멸자에서 멈춘다 - 기반 소멸자 시점엔 파생 부분이 없어 OnRelease가 순수 가상 호출이 된다
	mKeyPool.Clear();
}

bool AesEchoServer::LoadSignKey(const char* secretCfgPath)
{
	// RSA 에코 서버의 LoadSignKey와 같다 - 같은 rsa_echo.cfg를 읽는다
	utility::stDebug debug;
	const std::unique_ptr<utility::Parser> parser = utility::Parser::CreateParser(secretCfgPath, debug);
	std::string hex;
	std::string blob;
	uint8_t modulus[RSA_MODULUS_LEN]; // 형식 검사로만 쓴다(2048비트·e = 65537)
	const bool bOk = parser != nullptr && parser->Get("signSecretKey", hex) && HexToBytes(hex, &blob)
		&& mSignKey.ImportPrivateBlob(blob) && mSignKey.ExportPublic(modulus);
	SecureZeroMemory(hex.data(), hex.size());
	SecureZeroMemory(blob.data(), blob.size());
	return bOk;
}

void AesEchoServer::OnAccept(const uint64_t id, const SOCKADDR_IN& remoteAddr)
{
	EchoSession& session = getSession(id);
	Assert(session.SessionID == 0, "OnAccept: 이전 세션이 정리되지 않음");
	session.SessionID = id;
	session.Channel.Clear();
	session.Aes.Clear();
}

void AesEchoServer::OnRecv(const uint64_t id, const char* payload, const int32_t len)
{
	EchoSession& session = getSession(id);
	if (!session.Channel.IsReady())
	{
		onHelloServer(id, session, payload, len); // 첫 패킷은 임시 공개 키 주고받기
	}
	else if (!session.LoggedIn)
	{
		onRsaMsg(id, session, payload, len); // 로그인 전 - 가입·로그인(RSA)
	}
	else
	{
		onAesEcho(id, session, payload, len); // 로그인 뒤 - 에코(AES)
	}
}

void AesEchoServer::OnRelease(const uint64_t id)
{
	EchoSession& session = getSession(id);
	Assert(session.SessionID == id, "OnRelease: 다른 세션");
	UniquePtr<RsaKey> serverKey = session.Channel.ReleaseMine(); // Hello 전에 끊겼으면 빌린 키가 없다
	if (serverKey != nullptr)
	{
		mKeyPool.Return(std::move(serverKey), session.KeyUseCnt);
	}
	session.KeyUseCnt = 0;
	session.LoggedIn = false;
	session.AccountNo = 0;
	session.Channel.Clear();
	session.Aes.Clear(); // 세션 키를 지운다
	session.SessionID = 0;
}

EchoSession& AesEchoServer::getSession(const uint64_t id)
{
	const uint32_t index = static_cast<uint32_t>(id); // 라이브러리의 세션 ID = (시퀀스 번호 << 32) | 인덱스
	Assert(index < mSessions.size(), "getSession: 인덱스 범위 밖 - 생성자의 maxSession과 Start의 MaxSession이 다름");
	return mSessions[index];
}

void AesEchoServer::onHelloServer(const uint64_t id, EchoSession& session, const char* payload, const int32_t len)
{
	if (len != static_cast<int32_t>(sizeof(HelloServer)))
	{
		disconnectBadPacket(id);
		return;
	}
	ProfileScope total("Handshake total");
	HelloServer hello;
	MemCpy(&hello, payload, sizeof(hello));
	UniquePtr<RsaKey> serverKey;
	bool bTaken = false;
	{
		ProfileScope scope("Handshake take key"); // 풀에 있으면 꺼내기만, 비었으면 키 쌍 만들기
		bTaken = mKeyPool.Take(&serverKey, &session.KeyUseCnt);
	}
	if (!bTaken)
	{
		++mKeyEmptyCnt;
		Disconnect(id);
		return;
	}
	HelloClient reply{};
	bool bOk = false;
	{
		ProfileScope scope("Handshake sign");
		bOk = session.Channel.AcceptClient(hello, &mSignKey, std::move(serverKey), &reply);
	}
	if (!bOk)
	{
		disconnectBadPacket(id);
		return;
	}
	++mHelloCnt;
	SendPacket(id, reinterpret_cast<const char*>(&reply), static_cast<int32_t>(sizeof(reply)));
}

void AesEchoServer::onRsaMsg(const uint64_t id, EchoSession& session, const char* payload, const int32_t len)
{
	ProfileScope total("Request total");
	std::string plain;
	bool bOpened = false;
	{
		ProfileScope scope("Request decrypt");
		bOpened = session.Channel.Open(payload, static_cast<size_t>(len), &plain);
	}
	std::string reply;
	if (!bOpened || !handleRsaPlain(session, plain, &reply))
	{
		disconnectBadPacket(id);
		return;
	}
	std::string sealed;
	bool bSealed = false;
	{
		ProfileScope scope("Response encrypt");
		bSealed = session.Channel.Seal(reply.data(), reply.size(), &sealed);
	}
	SecureZeroMemory(reply.data(), reply.size()); // 로그인 응답이면 세션 키 사본
	if (!bSealed)
	{
		disconnectBadPacket(id);
		return;
	}
	ProfileScope scope("Response send");
	SendPacket(id, sealed.data(), static_cast<int32_t>(sealed.size()));
}

void AesEchoServer::onAesEcho(const uint64_t id, EchoSession& session, const char* payload, const int32_t len)
{
	ProfileScope total("Echo total");
	std::string plain;
	bool bOpened = false;
	{
		ProfileScope scope("Echo decrypt: AES-GCM"); // 세션 키로 풀고 태그 확인
		bOpened = session.Aes.Open(payload, static_cast<size_t>(len), &plain);
	}
	eRsaMsgType type = eRsaMsgType::EchoReq;
	if (bOpened && plain.size() == sizeof(RsaEchoMsg))
	{
		MemCpy(&type, plain.data(), sizeof(type));
	}
	if (!bOpened || plain.size() != sizeof(RsaEchoMsg) || type != eRsaMsgType::EchoReq)
	{
		disconnectBadPacket(id); // 태그·번호가 틀림, 또는 로그인 뒤에 에코가 아닌 메시지
		return;
	}
	{
		ProfileScope scope("Echo logic");
		const eRsaMsgType resType = eRsaMsgType::EchoRes;
		MemCpy(plain.data(), &resType, sizeof(resType)); // 종류만 바꿔 그대로 돌려준다
		++mEchoCnt;
	}
	std::string sealed;
	bool bSealed = false;
	{
		ProfileScope scope("Echo encrypt: AES-GCM"); // 세션 키로 잠그고 태그 붙이기
		bSealed = session.Aes.Seal(plain.data(), plain.size(), &sealed);
	}
	if (!bSealed)
	{
		disconnectBadPacket(id);
		return;
	}
	ProfileScope scope("Echo send");
	SendPacket(id, sealed.data(), static_cast<int32_t>(sealed.size()));
}

bool AesEchoServer::handleRsaPlain(EchoSession& session, const std::string& plain, std::string* outReply)
{
	Assert(outReply != nullptr, "handleRsaPlain: outReply가 null");
	if (plain.size() < sizeof(eRsaMsgType))
	{
		return false;
	}
	eRsaMsgType type;
	MemCpy(&type, plain.data(), sizeof(type));
	bool bOk = false;
	switch (type)
	{
	case eRsaMsgType::RegisterReq:
		bOk = onRegister(session, plain, outReply);
		break;
	case eRsaMsgType::LoginReq:
		bOk = onLogin(session, plain, outReply);
		break;
	default:
		bOk = false; // 로그인 전에는 가입·로그인만 - 에코를 RSA로 보내면 끊는다
		break;
	}
	return bOk;
}

// 고정 길이 칸에서 널로 끝나는 문자열을 읽는다. 널이 없거나 비었으면 false
static bool readField(const char* field, const size_t capacity, std::string* outText)
{
	const size_t len = strnlen(field, capacity);
	if (len == 0 || len == capacity)
	{
		return false;
	}
	outText->assign(field, len);
	return true;
}

bool AesEchoServer::onRegister(EchoSession& session, const std::string& plain, std::string* outReply)
{
	ProfileScope scope("Register logic");
	if (plain.size() != sizeof(RsaRegisterReq) || session.LoggedIn)
	{
		return false;
	}
	RsaRegisterReq req;
	MemCpy(&req, plain.data(), sizeof(req));
	std::string accountID;
	std::string password;
	RsaRegisterRes res{};
	res.Type = eRsaMsgType::RegisterRes;
	if (!readField(req.AccountID, sizeof(req.AccountID), &accountID))
	{
		res.Result = eRegisterResult::BadID;
	}
	else if (!readField(req.Password, sizeof(req.Password), &password) || password.size() < PASSWORD_MIN_LEN)
	{
		res.Result = eRegisterResult::BadPassword;
	}
	else
	{
		res.Result = addAccount(accountID, password) ? eRegisterResult::Ok : eRegisterResult::Exists;
	}
	SecureZeroMemory(req.Password, sizeof(req.Password));
	SecureZeroMemory(password.data(), password.size());
	++mRegisterCnt;
	outReply->assign(reinterpret_cast<const char*>(&res), sizeof(res));
	return true;
}

bool AesEchoServer::onLogin(EchoSession& session, const std::string& plain, std::string* outReply)
{
	ProfileScope scope("Login logic"); // 계정 확인 + 세션 키 만들기
	if (plain.size() != sizeof(RsaLoginReq) || session.LoggedIn)
	{
		return false;
	}
	RsaLoginReq req;
	MemCpy(&req, plain.data(), sizeof(req));
	std::string accountID;
	std::string password;
	AesLoginRes res{};
	res.Type = eRsaMsgType::LoginRes;
	res.Result = eAuthResult::Fail;
	uint64_t accountNo = 0;
	const bool bChecked = readField(req.AccountID, sizeof(req.AccountID), &accountID) && readField(req.Password, sizeof(req.Password), &password) &&
		checkAccount(accountID, password, &accountNo);
	// 세션 키 32바이트 - 운영체제 난수(CNG). 응답이 RSA로 잠겨 나가므로 네트워크에 평문으로 나가지 않는다
	if (bChecked && BCRYPT_SUCCESS(BCryptGenRandom(nullptr, res.SessionKey, sizeof(res.SessionKey), BCRYPT_USE_SYSTEM_PREFERRED_RNG)) &&
		session.Aes.Start(res.SessionKey, true))
	{
		res.Result = eAuthResult::Ok;
		res.AccountNo = accountNo;
		session.LoggedIn = true; // 이 응답 뒤로 이 연결의 패킷은 모두 AES
		session.AccountNo = accountNo;
		++mLoginOkCnt;
	}
	else
	{
		SecureZeroMemory(res.SessionKey, sizeof(res.SessionKey));
		++mLoginFailCnt;
	}
	SecureZeroMemory(req.Password, sizeof(req.Password));
	SecureZeroMemory(password.data(), password.size());
	outReply->assign(reinterpret_cast<const char*>(&res), sizeof(res));
	SecureZeroMemory(res.SessionKey, sizeof(res.SessionKey));
	return true;
}

bool AesEchoServer::addAccount(const std::string& accountID, const std::string& password)
{
	std::lock_guard<std::mutex> lock(mAccountLock);
	const std::pair<std::unordered_map<std::string, EchoAccount>::iterator, bool> result = mAccounts.try_emplace(accountID);
	if (!result.second)
	{
		return false;
	}
	result.first->second.Password = password;
	result.first->second.AccountNo = mNextAccountNo;
	++mNextAccountNo;
	return true;
}

bool AesEchoServer::checkAccount(const std::string& accountID, const std::string& password, uint64_t* outAccountNo)
{
	Assert(outAccountNo != nullptr, "checkAccount: outAccountNo가 null");
	std::lock_guard<std::mutex> lock(mAccountLock);
	const std::unordered_map<std::string, EchoAccount>::const_iterator it = mAccounts.find(accountID);
	if (it == mAccounts.end() || it->second.Password != password)
	{
		return false;
	}
	*outAccountNo = it->second.AccountNo;
	return true;
}

void AesEchoServer::disconnectBadPacket(const uint64_t id)
{
	++mBadPacketCnt;
	Disconnect(id);
}

std::ostream& operator<<(std::ostream& out, const AesEchoServer& server)
{
	const NetStat stat = server.GetStat();
	out << "[stat] 세션=" << server.GetSessionCnt()
		<< " 수락=" << server.GetAcceptCnt()
		<< " 해제=" << server.GetReleaseCnt()
		<< " Hello=" << server.GetHelloCnt()
		<< " 가입=" << server.GetRegisterCnt()
		<< " 로그인=" << server.GetLoginOkCnt()
		<< " 로그인실패=" << server.GetLoginFailCnt()
		<< " 에코=" << server.GetEchoCnt()
		<< " 키쌍=" << server.GetReadyKeyCnt()
		<< " 빌려줌=" << server.GetLentKeyCnt()
		<< " 키생성=" << server.GetCreatedKeyCnt()
		<< " 키교체=" << server.GetReplacedKeyCnt()
		<< " 키생성실패끊김=" << server.GetKeyEmptyCnt()
		<< " 비정상헤더=" << stat.BadPacketCnt
		<< " 잘못된패킷=" << server.GetBadPacketCnt();
	return out;
}
