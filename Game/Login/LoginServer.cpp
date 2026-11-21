#include <cstring>
#include <sodium.h>
#include "LoginServer.h"
#include "Memory.h"

// 클라가 보낸 바이트를 X'16진수'로 - 이스케이프 없이 어떤 바이트든 안전하다(게임 서버 GameDB::ToHexLiteral과 같은 방식)
static std::string toHexLiteral(const std::string& bytes)
{
	static const char HEX[] = "0123456789ABCDEF";
	std::string out = "X'";
	for (const char c : bytes)
	{
		const unsigned char byte = static_cast<unsigned char>(c);
		out += HEX[byte >> 4];
		out += HEX[byte & 0x0F];
	}
	out += "'";
	return out;
}

// 고정 칸의 문자열 - 널로 끝나지 않으면(칸을 꽉 채움) false
template <size_t N>
static bool readFixedString(const char (&field)[N], std::string* outValue)
{
	Assert(outValue != nullptr, "readFixedString: outValue가 null");
	const size_t len = strnlen(field, N);
	if (len == N)
	{
		return false;
	}
	outValue->assign(field, len);
	return true;
}

static bool isValidID(const std::string& accountID)
{
	return !accountID.empty() && accountID.size() < ACCOUNT_ID_LEN;
}

static bool isValidPassword(const std::string& password)
{
	return password.size() >= PASSWORD_MIN_LEN && password.size() <= PASSWORD_MAX_LEN;
}

static void wipe(std::string* outValue) // 비밀번호 사본을 지운다(짧은 문자열은 객체 안 버퍼에 있어 소멸만으로는 남는다)
{
	Assert(outValue != nullptr, "wipe: outValue가 null");
	sodium_memzero(outValue->data(), outValue->size());
}

// 가입·로그인을 처리하는 동안 처리 중 워커 수를 센다(통계)
class WorkingScope final
{
public:
	explicit WorkingScope(std::atomic<int32_t>* counter)
		: mCounter(counter)
	{
		Assert(counter != nullptr, "WorkingScope: counter가 null");
		++(*mCounter);
	}
	~WorkingScope()
	{
		--(*mCounter);
	}
	WorkingScope(const WorkingScope&) = delete;
	WorkingScope& operator=(const WorkingScope&) = delete;

private:
	std::atomic<int32_t>* mCounter;
};

LoginServer::LoginServer(const int32_t maxSession, LoginStorePool* stores, const PasswordHasher* hasher, const RsaKey* signKey, RsaKeyPool* keyPool,
	const int32_t idleLimitSec, const int32_t ackLimitSec)
	: mSessions(maxSession)
	, mStores(stores)
	, mHasher(hasher)
	, mSignKey(signKey)
	, mKeyPool(keyPool)
	, mIdleLimitMs(static_cast<ULONGLONG>(idleLimitSec) * 1000)
	, mAckLimitMs(static_cast<ULONGLONG>(ackLimitSec) * 1000)
{
	Assert(stores != nullptr && hasher != nullptr && signKey != nullptr && signKey->IsReady() && keyPool != nullptr,
		"LoginServer: DB·Redis 연결·해시·서버 고정 키·키 쌍 풀이 없음");
	Assert(ackLimitSec > 0, "LoginServer: ackLimitSec가 0 이하 - 확인 기한은 끌 수 없다");
	// 확인 기한은 항상 보므로 감시 스레드는 요청 기한을 꺼도 돈다
	mIdleWatch.Start(1000, std::bind(&LoginServer::checkIdleDeadline, this));
}

LoginServer::~LoginServer()
{
	Shutdown(); // 파생 소멸자에서 멈춘다 - 기반 소멸자 시점엔 OnRelease가 순수 가상 호출이 된다
}

void LoginServer::Shutdown()
{
	mIdleWatch.Stop();       // 워커 밖 스레드는 라이브러리 Stop 전에 멈춘다
	mKeyPool->StopRefill();  // Stop이 끊는 연결들의 OnRelease가 아무도 쓰지 않을 새 키를 만들지 않게(종료가 늦어지지 않게)
	Stop();                  // 처리 중이던 요청은 워커가 끝낸 뒤 멈춘다
}

LoginServer::LoginSession& LoginServer::getSession(const uint64_t id)
{
	const uint32_t index = static_cast<uint32_t>(id); // 라이브러리의 세션 ID = (시퀀스 번호 << 32) | 인덱스
	Assert(index < mSessions.size(), "getSession: 인덱스 범위 밖 - 생성자의 maxSession과 Start의 MaxSession이 다름");
	return mSessions[index];
}

void LoginServer::OnAccept(const uint64_t id, const SOCKADDR_IN&)
{
	LoginSession& session = getSession(id);
	Assert(session.SessionID == 0, "OnAccept: 이전 세션이 정리되지 않음");
	session.SessionID = id;
	session.bRegistered = false;
	session.Channel.Clear(); // 임시 키는 이 연결의 HelloServer에서 새로 받는다
	session.LastActiveTick = GetTickCount64(); // 시각을 먼저, ID를 나중에 - 감시가 새 ID를 보면 새 시각도 보인다
	session.State = eLoginState::WaitHello;
	session.WatchSessionID = id;
}

void LoginServer::OnRelease(const uint64_t id)
{
	LoginSession& session = getSession(id);
	Assert(session.SessionID == id, "OnRelease: 다른 세션");
	session.WatchSessionID = 0;
	session.SessionID = 0; // 요청 처리는 OnRecv 안에서 끝나므로 처리 중에 불리지 않는다(같은 세션의 콜백은 동시에 불리지 않는다)
	// 마지막 콜백 - 이 뒤로 이 세션에 보내는 스레드가 없다. 서버 임시 키는 풀에 돌려주고(maxUseCnt번 썼으면 지우고 이 워커가 새 키를 만들어 채운다, 약 55ms) 나머지 통로 상태는 지운다
	if (session.KeyUseCnt > 0)
	{
		mKeyPool->Return(session.Channel.ReleaseMine(), session.KeyUseCnt);
		session.KeyUseCnt = 0;
	}
	session.Channel.Clear();
}

void LoginServer::OnRecv(const uint64_t id, const char* payload, const int32_t len)
{
	LoginSession& session = getSession(id);
	if (session.State == eLoginState::WaitHello)
	{
		onHelloServer(id, payload, len); // 첫 패킷은 임시 공개 키 주고받기
	}
	else
	{
		// 그 뒤는 잠그고 서명한 패킷 - 서명·번호가 틀리거나 풀리지 않으면 끊는다. 같은 세션의 OnRecv는 동시에 불리지 않아 받는 번호는 락이 없다
		std::string plain;
		if (session.Channel.Open(payload, static_cast<size_t>(len), &plain))
		{
			onRecvPacket(id, plain.data(), static_cast<int32_t>(plain.size()));
		}
		else
		{
			disconnectBadPacket(id);
		}
		wipe(&plain); // 비밀번호가 들어 있었다
	}
}

void LoginServer::onHelloServer(const uint64_t id, const char* payload, const int32_t len)
{
	HelloServer hello;
	if (len != static_cast<int32_t>(sizeof(HelloServer)))
	{
		disconnectBadPacket(id); // 키를 나누기 전에 다른 패킷 - 정상 클라는 보내지 않는다
		return;
	}
	MemCpy(&hello, payload, sizeof(hello));
	if (hello.Type != eLoginType::HelloServer)
	{
		disconnectBadPacket(id);
		return;
	}
	LoginSession& session = getSession(id);
	if (session.KeyUseCnt > 0)
	{
		// 키를 이미 빌린 세션에 HelloServer가 또 왔다 - 밖에서 온 입력이라 끊는다(멈추면 연결 하나로 서버 전체가 멈춘다)
		// 그냥 받으면 Take가 KeyUseCnt를 덮어쓰고 AcceptClient가 쥔 키를 반납 없이 지운다. 끊으면 빌린 키는 OnRelease가 풀에 돌려준다
		// (지금은 첫 HelloServer가 실패하면 끊고, 라이브러리가 OnRecv 안에서 끊긴 세션의 남은 패킷을 버려서 여기에 오지 않는다)
		disconnectBadPacket(id);
		return;
	}
	UniquePtr<RsaKey> serverKey = mKeyPool->Take(&session.KeyUseCnt);
	if (serverKey == nullptr)
	{
		++mKeyEmptyCnt; // 풀의 키를 모두 다른 연결이 빌려 갔다(HelloServer 뒤 끊기 전인 연결이 loginKeyPool개) - 아직 잠가 보낼 통로가 없어 응답 없이 끊는다. 이미 접속한 세션의 요청은 계속 처리된다
		Disconnect(id);
		return;
	}
	HelloClient reply{};
	if (!session.Channel.AcceptClient(hello, mSignKey, std::move(serverKey), &reply)) // 서명 한 번(약 0.5ms) - 워커에서
	{
		disconnectBadPacket(id); // 잘못된 클라 공개 키
		return;
	}
	session.LastActiveTick = GetTickCount64(); // 상태를 바꾸기 전에 시각부터(LoginSession::State 규칙) - 요청 기한은 HelloClient를 보낸 때부터 센다
	session.State = eLoginState::Ready;        // 보내기 전에 - 같은 세션의 OnRecv는 차례로 불리지만, 상태를 먼저 바꾸는 순서를 다른 응답과 맞춘다
	SendPacket(id, reinterpret_cast<const char*>(&reply), sizeof(reply)); // HelloClient만 평문 - 아직 요청을 받기 전이라 다른 송신과 겹치지 않는다
}

void LoginServer::sendSecure(const uint64_t id, const void* data, const int32_t len)
{
	LoginSession& session = getSession(id);
	std::string packet;
	// 보내는 스레드는 이 세션의 워커뿐(OnRecv 안)이라 번호 매기기 + 잠그기·서명 + SendPacket에 락이 없어도 번호 순서대로 송신 링에 들어간다
	if (!session.Channel.Seal(data, static_cast<size_t>(len), &packet))
	{
		Disconnect(id); // CNG 실패 - 보낼 수 없는 연결
		return;
	}
	SendPacket(id, packet.data(), static_cast<int32_t>(packet.size()));
	session.LastActiveTick = GetTickCount64(); // 응답한 뒤부터 다시 센다
}

void LoginServer::onRecvPacket(const uint64_t id, const char* payload, const int32_t len)
{
	if (len < static_cast<int32_t>(sizeof(eLoginType)))
	{
		disconnectBadPacket(id); // 종류도 없다
		return;
	}
	eLoginType type;
	MemCpy(&type, payload, sizeof(type));
	const eLoginState state = getSession(id).State;
	if (state == eLoginState::Ready && type == eLoginType::RegisterReq)
	{
		onRegisterReq(id, payload, len);
	}
	else if (state == eLoginState::Ready && type == eLoginType::AuthReq)
	{
		onAuthReq(id, payload, len);
	}
	else if (state == eLoginState::KeySent && type == eLoginType::AuthAck)
	{
		onAuthAck(id, len);
	}
	else
	{
		// 상태에 맞지 않는 메시지 - 로그인 성공 뒤 다시 요청(KeySent), 로그인 실패 뒤 다시 요청(FailSent - 한 연결에 한 번), 끊기로 한 뒤(Closing), 모르는 종류, 서버 → 클라 종류
		// (요청은 OnRecv 안에서 끝나므로 다음 요청은 응답 뒤의 상태에서 받는다 - 미리 보낸 요청도 하나씩 차례로 처리된다)
		disconnectBadPacket(id);
	}
}

void LoginServer::onAuthAck(const uint64_t id, const int32_t len)
{
	if (len != static_cast<int32_t>(sizeof(AuthAck)))
	{
		disconnectBadPacket(id);
		return;
	}
	// 클라가 세션 키를 받았다 - 이 연결은 할 일이 끝났다. 클라는 이미 응답을 읽었으므로 RST로 닫아도 잃을 것이 없다
	getSession(id).State = eLoginState::Closing;
	++mAckCnt;
	Disconnect(id);
}

void LoginServer::onRegisterReq(const uint64_t id, const char* payload, const int32_t len)
{
	if (len != static_cast<int32_t>(sizeof(RegisterReq)))
	{
		disconnectBadPacket(id);
		return;
	}
	RegisterReq req;
	MemCpy(&req, payload, sizeof(req));
	std::string accountID;
	std::string password;
	RegisterRes res{ eLoginType::RegisterRes, eRegisterResult::Ok };
	if (!readFixedString(req.AccountID, &accountID) || !isValidID(accountID))
	{
		res.Result = eRegisterResult::BadID; // 밖에서 온 값 - 끊지 않고 거절만
	}
	else if (!readFixedString(req.Password, &password) || !isValidPassword(password))
	{
		res.Result = eRegisterResult::BadPassword;
	}
	sodium_memzero(req.Password, sizeof(req.Password));
	LoginSession& session = getSession(id);
	if (session.bRegistered)
	{
		wipe(&password);
		disconnectBadPacket(id); // 이 연결에서 이미 가입했다 - 한 연결에서 가입은 한 번(해시·INSERT를 계속 시키지 못하게)
		return;
	}
	if (res.Result == eRegisterResult::Ok)
	{
		// 워커가 그 자리에서 DB 확인·해시·INSERT(수십 ms) - 그동안 이 워커는 다른 연결을 처리하지 못한다
		session.LastActiveTick = GetTickCount64(); // 상태를 바꾸기 전에 시각부터(LoginSession::State 규칙) - 감시가 Ready를 읽은 직후 Pending이 돼도 옛 시각으로 끊지 않게
		session.State = eLoginState::Pending;      // 처리하는 동안 기한 감시가 끊지 않게
		const WorkingScope working(&mWorkingCnt);
		const StoreLease lease(mStores);
		res.Result = (lease.Get() != nullptr) ? registerAccount(*lease.Get(), accountID, password) : eRegisterResult::ServerError;
		session.bRegistered = (res.Result == eRegisterResult::Ok);
	}
	wipe(&password);
	session.LastActiveTick = GetTickCount64(); // 상태를 바꾸기 전에 - 감시가 바뀐 상태를 보면 새 시각도 보이게
	session.State = eLoginState::Ready;
	sendSecure(id, &res, sizeof(res));
}

void LoginServer::onAuthReq(const uint64_t id, const char* payload, const int32_t len)
{
	if (len != static_cast<int32_t>(sizeof(AuthReq)))
	{
		disconnectBadPacket(id);
		return;
	}
	AuthReq req;
	MemCpy(&req, payload, sizeof(req));
	std::string accountID;
	std::string password;
	const bool bWellFormed = readFixedString(req.AccountID, &accountID) && readFixedString(req.Password, &password);
	sodium_memzero(req.Password, sizeof(req.Password));
	if (!bWellFormed)
	{
		wipe(&password);
		disconnectBadPacket(id); // 널로 끝나지 않는 칸 - 정상 클라는 보내지 않는다
		return;
	}
	// 워커가 그 자리에서 DB 읽기·해시(약 63ms)·Redis - 그동안 이 워커는 다른 연결을 처리하지 못한다
	LoginSession& session = getSession(id);
	session.LastActiveTick = GetTickCount64(); // 상태를 바꾸기 전에 시각부터(LoginSession::State 규칙) - 감시가 Ready를 읽은 직후 Pending이 돼도 옛 시각으로 끊지 않게
	session.State = eLoginState::Pending;      // 처리하는 동안 기한 감시가 끊지 않게
	const WorkingScope working(&mWorkingCnt);  // 응답을 보낼 때까지 - 처리 중 워커 수(통계)
	const StoreLease lease(mStores);           // 워커만 빌리므로 정상이면 모자라지 않는다
	AuthRes result = authenticate(lease.Get(), accountID, password); // 대입이 아니라 초기화 - 세션 키가 든 임시 사본을 남기지 않는다
	wipe(&password);
	session.LastActiveTick = GetTickCount64(); // 상태를 바꾸기 전에 - KeySent·FailSent면 이 시각부터 확인 기한을 센다
	// 로그인 성공이면 KeySent(AuthAck를 기다림), 실패면 FailSent(클라가 끊기를 기다림), 서버 오류(DB·Redis)면 Ready(같은 연결에서 다시 해도 된다)
	// 실패한 연결은 다음 요청을 받지 않는다 - 한 연결에서 비밀번호는 한 번만 맞혀 볼 수 있다(2026-11-21 사용자 결정, 전에는 5번)
	// 실패 응답을 보낸 뒤 서버가 바로 끊지 않는 것은 라이브러리가 RST로 닫기 때문이다 - 클라가 읽기 전에 RST가 닿으면 이 응답이 사라져
	// 클라는 "비밀번호 틀림" 대신 "연결 끊김"을 본다(세션 키를 보낸 뒤 AuthAck를 기다리는 것과 같은 이유). 클라가 끊지 않으면 확인 기한(loginAckSec)으로 끊는다
	eLoginState next = eLoginState::Ready;
	if (result.Result == eAuthResult::Ok)
	{
		next = eLoginState::KeySent;
	}
	else if (result.Result == eAuthResult::Fail)
	{
		next = eLoginState::FailSent;
	}
	session.State = next;
	sendSecure(id, &result, sizeof(result));
	sodium_memzero(result.SessionKey, sizeof(result.SessionKey));
}

eRegisterResult LoginServer::registerAccount(LoginStore& store, const std::string& accountID, const std::string& password)
{
	DbConn& conn = store.Db;
	const std::string idLiteral = toHexLiteral(accountID);
	std::vector<std::vector<std::string>> rows;
	if (!conn.Query("SELECT 1 FROM credentials WHERE account_id = " + idLiteral, &rows))
	{
		return eRegisterResult::ServerError;
	}
	if (!rows.empty())
	{
		return eRegisterResult::Exists; // 해시(느림)를 하기 전에 거른다
	}
	std::string hash;
	if (!mHasher->Hash(password, &hash))
	{
		return eRegisterResult::ServerError;
	}
	// 해시 문자열은 $·,·=·+·/와 영숫자뿐이라 작은따옴표 안에 그대로 넣어도 된다
	const bool bInserted = conn.Exec("INSERT INTO credentials (account_id, password_hash) VALUES (" + idLiteral + ", '" + hash + "')", "credentials INSERT"); // 실패 로그에 해시를 남기지 않게 설명만
	if (!bInserted)
	{
		// 같은 아이디가 동시에 가입하면 UNIQUE에 걸린다 - 다시 읽어 있으면 Exists
		const bool bQueried = conn.Query("SELECT 1 FROM credentials WHERE account_id = " + idLiteral, &rows);
		return (bQueried && !rows.empty()) ? eRegisterResult::Exists : eRegisterResult::ServerError;
	}
	++mRegisterCnt;
	return eRegisterResult::Ok;
}

AuthRes LoginServer::authenticate(LoginStore* store, const std::string& accountID, const std::string& password)
{
	AuthRes res{};
	res.Type = eLoginType::AuthRes;
	res.Result = eAuthResult::Fail;
	std::vector<std::vector<std::string>> rows;
	std::string hash;
	uint64_t accountNo = 0;
	if (store == nullptr)
	{
		res.Result = eAuthResult::ServerError; // 빌릴 연결 쌍이 없다 - 워커만 빌리므로 정상이면 오지 않는다
	}
	// 아이디가 없거나 형식이 틀려도 해시 한 번만큼 걸리게 가짜 확인을 한다 - 로그인 응답의 시간으로 아이디가 있는지 가리기 어렵게
	// (가입 응답은 "이미 있음"을 알려 주므로 아이디가 있는지 자체는 숨기지 못한다 - 비밀번호를 맞혀 보는 쪽만 막는 목적)
	else if (!isValidID(accountID) || !isValidPassword(password))
	{
		mHasher->VerifyDummy(password);
	}
	else if (!store->Db.Query("SELECT account_no, password_hash FROM credentials WHERE account_id = " + toHexLiteral(accountID), &rows))
	{
		res.Result = eAuthResult::ServerError;
	}
	else if (rows.empty() || rows[0].size() < 2)
	{
		mHasher->VerifyDummy(password);
	}
	else if (mHasher->Verify(password, rows[0][1]))
	{
		accountNo = _strtoui64(rows[0][0].c_str(), nullptr, 10);
		hash = rows[0][1];
		res.Result = eAuthResult::Ok;
	}

	if (res.Result == eAuthResult::Ok && mHasher->NeedsRehash(hash))
	{
		std::string newHash; // 설정을 올린 뒤의 첫 로그인 - 비밀번호를 바꾸지 않아도 새 설정으로
		if (mHasher->Hash(password, &newHash))
		{
			store->Db.Exec("UPDATE credentials SET password_hash = '" + newHash + "' WHERE account_no = " + std::to_string(accountNo), "credentials 해시 바꾸기"); // 실패 로그에 해시를 남기지 않게 설명만
		}
	}
	if (res.Result == eAuthResult::Ok)
	{
		// 세션 키 - 게임 서버는 Redis에서, 클라는 이 응답에서 받는다. 30초 안에 한 번만 쓰인다(게임 서버가 확인 뒤 지움)
		randombytes_buf(res.SessionKey, sizeof(res.SessionKey));
		std::string value;
		value.reserve(sizeof(res.SessionKey) + accountID.size()); // 처음부터 다 들어갈 크기로 - 커지며 옮겨 간 옛 버퍼에 세션 키가 남지 않게
		value.append(reinterpret_cast<const char*>(res.SessionKey), sizeof(res.SessionKey));
		value += accountID;
		const bool bStored = store->Redis.SetEx("session:" + std::to_string(accountNo), value, SESSION_KEY_TTL_SEC);
		wipe(&value);
		if (bStored)
		{
			res.AccountNo = accountNo;
		}
		else
		{
			sodium_memzero(res.SessionKey, sizeof(res.SessionKey));
			res.Result = eAuthResult::ServerError;
		}
	}
	if (res.Result == eAuthResult::Ok)
	{
		++mAuthOkCnt;
	}
	else if (res.Result == eAuthResult::Fail)
	{
		++mAuthFailCnt;
	}
	return res;
}

void LoginServer::disconnectBadPacket(const uint64_t id)
{
	++mBadPacketCnt;
	Disconnect(id);
}

void LoginServer::checkIdleDeadline()
{
	// 접속만 하고 요청하지 않는 연결이 세션 칸(maxSession)을 채워 새 사용자를 막지 못하게. 워커가 요청을 처리하는 중(Pending)은 끊지 않는다
	// 끊기는 이미 끊겼거나 칸이 다른 세션으로 바뀐 ID면 아무 일도 안 한다
	// 세션 키를 보낸 연결(KeySent)·로그인 실패 응답을 보낸 연결(FailSent)은 확인 기한 - 클라가 끊지 않고 붙잡고 있지 못하게
	std::vector<uint64_t> idleExpired;
	std::vector<uint64_t> ackExpired;
	std::vector<uint64_t> failWaitExpired;
	for (LoginSession& session : mSessions)
	{
		const uint64_t sessionID = session.WatchSessionID;
		if (sessionID == 0)
		{
			continue;
		}
		const eLoginState state = session.State; // 상태를 먼저, 시각을 나중에 - 상태를 바꾸는 쪽은 시각을 먼저 쓴다
		const bool bAckWait = (state == eLoginState::KeySent || state == eLoginState::FailSent);
		const bool bIdleWait = (mIdleLimitMs > 0) && (state == eLoginState::WaitHello || state == eLoginState::Ready);
		if (!bAckWait && !bIdleWait)
		{
			continue; // 요청 처리 중(Pending)이거나 이미 끊는 중(Closing)
		}
		const ULONGLONG lastActive = session.LastActiveTick;
		const ULONGLONG now = GetTickCount64(); // 시각을 읽은 뒤에 - now가 앞서면 부호 없는 뺄셈이 넘친다(README_ERRORS 2-8)
		if (bAckWait && now - lastActive > mAckLimitMs)
		{
			if (state == eLoginState::KeySent)
			{
				ackExpired.push_back(sessionID);
			}
			else
			{
				failWaitExpired.push_back(sessionID);
			}
		}
		else if (bIdleWait && now - lastActive > mIdleLimitMs)
		{
			idleExpired.push_back(sessionID);
		}
	}
	for (const uint64_t sessionID : idleExpired)
	{
		Disconnect(sessionID);
		++mIdleTimeoutCnt; // 그사이 스스로 끊긴 세션도 셀 수 있다 - 통계용
	}
	for (const uint64_t sessionID : ackExpired)
	{
		Disconnect(sessionID);
		++mAckTimeoutCnt;
	}
	for (const uint64_t sessionID : failWaitExpired)
	{
		Disconnect(sessionID);
		++mFailWaitTimeoutCnt;
	}
}

std::ostream& operator<<(std::ostream& out, const LoginServer& server)
{
	const NetStat stat = server.GetStat();
	out << "[stat] 세션=" << server.GetSessionCnt()
		<< " 수락=" << server.GetAcceptCnt()
		<< " 해제=" << server.GetReleaseCnt()
		<< " 비정상헤더=" << stat.BadPacketCnt
		<< " 잘못된패킷=" << server.GetBadPacketCnt()
		<< " 가입=" << server.GetRegisterCnt()        // 누적
		<< " 로그인성공=" << server.GetAuthOkCnt()     // 누적
		<< " 로그인실패=" << server.GetAuthFailCnt()   // 누적(아이디 없음·비밀번호 틀림)
		<< " 처리중워커=" << server.GetWorkingCnt()     // 지금 가입·로그인(해시·DB·Redis)을 처리하는 워커 수 - 워커 수와 같으면 다른 연결이 기다린다
		<< " 기한끊김=" << server.GetIdleTimeoutCnt()  // 접속·응답 뒤 기한 안에 요청이 없어 끊은 수(누적)
		<< " 키쌍=" << server.GetKeyReadyCnt()          // 미리 만들어 둔 서버 임시 키 쌍
		<< " 키쌍부족끊김=" << server.GetKeyEmptyCnt() // 키 쌍이 떨어져 HelloServer를 받지 않고 끊은 수(누적)
		<< " 키생성=" << server.GetKeyCreatedCnt()     // 만든 서버 임시 키 쌍(누적, 시작 때 채운 것 포함) - 돌려 쓰면 접속 수보다 적다
		<< " 키생성실패=" << server.GetKeyFailCnt()    // 다시 채우지 않는다 - 0이 아니면 키 수가 loginKeyPool보다 그만큼 적다
		<< " 확인받음=" << server.GetAckCnt()           // 세션 키를 보낸 뒤 AuthAck를 받고 끊은 수(누적)
		<< " 확인시간초과=" << server.GetAckTimeoutCnt() // 세션 키를 보낸 뒤 기한 안에 AuthAck가 없어 끊은 수(누적). 이름에 "기한끊김"을 넣지 않는다 - 시험이 이름 일부로 찾는다
		<< " 실패뒤시간초과=" << server.GetFailWaitTimeoutCnt(); // 로그인 실패 응답 뒤 기한 안에 클라가 끊지 않아 끊은 수(누적)
	return out;
}
