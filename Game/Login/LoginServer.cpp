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

enum : int64_t
{
	AUTH_QUEUE_MAX = 200, // 인증 대기열이 이만큼 밀렸으면 해시 없이 바로 거절 - 접속을 많이 열어 해시(64MiB)를 쌓는 것을 막는다
};

LoginServer::LoginServer(const int32_t maxSession, DbThread* authThread, RedisClient* redis, const PasswordHasher* hasher, const uint8_t (&signSecretKey)[SIGN_SECRET_KEY_LEN])
	: mSessions(maxSession)
	, mAuthThread(authThread)
	, mRedis(redis)
	, mHasher(hasher)
{
	Assert(authThread != nullptr && redis != nullptr && hasher != nullptr, "LoginServer: 인증 스레드·Redis·해시가 null");
	MemCpy(mSignSecretKey, signSecretKey, sizeof(mSignSecretKey));
}

LoginServer::~LoginServer()
{
	Shutdown(); // 파생 소멸자에서 멈춘다 - 기반 소멸자 시점엔 OnRelease가 순수 가상 호출이 된다
	sodium_memzero(mSignSecretKey, sizeof(mSignSecretKey));
}

void LoginServer::Shutdown()
{
	mbClosing = true; // 이 뒤로 OnRecv는 패킷을 버린다
	while (mRecvInProgressCnt != 0)
	{
		Sleep(1); // 처리 중이던 패킷이 인증 스레드에 넣을 요청까지 넣은 뒤에 멈춘다
	}
	mAuthThread->Stop(); // 남은 요청에 응답하고 UnholdSession까지 끝낸다
	Stop();
}

int64_t LoginServer::GetAuthPendingCnt() const
{
	return mAuthThread->GetPendingCnt();
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
	session.FailCnt = 0;
	session.bPending = false;
	session.Cipher.Clear(); // 키는 이 연결의 ClientHello에서 새로 나눈다
}

void LoginServer::OnRelease(const uint64_t id)
{
	LoginSession& session = getSession(id);
	Assert(session.SessionID == id, "OnRelease: 다른 세션");
	session.SessionID = 0; // 요청 중이었다면 HoldSession 때문에 인증 스레드의 UnholdSession 뒤에 불린다
	session.Cipher.Clear(); // 마지막 콜백 - 이 뒤로 이 세션에 보내는 스레드가 없다
}

void LoginServer::OnRecv(const uint64_t id, const char* payload, const int32_t len)
{
	++mRecvInProgressCnt; // 먼저 올리고 종료 표시를 본다 - GameServer::OnRecv와 같은 이유
	if (!mbClosing)
	{
		LoginSession& session = getSession(id);
		if (!session.Cipher.IsReady())
		{
			onClientHello(id, payload, len); // 첫 패킷은 키 나누기
		}
		else
		{
			// 그 뒤는 암호문 - 풀리지 않으면(변조·순번 어긋남·평문) 끊는다. 같은 세션의 OnRecv는 동시에 불리지 않아 받는 순번은 락이 없다
			std::string plain;
			if (session.Cipher.Open(payload, static_cast<size_t>(len), &plain))
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
	--mRecvInProgressCnt;
}

void LoginServer::onClientHello(const uint64_t id, const char* payload, const int32_t len)
{
	ClientHello hello;
	if (len != static_cast<int32_t>(sizeof(ClientHello)))
	{
		disconnectBadPacket(id); // 키를 나누기 전에 다른 패킷 - 정상 클라는 보내지 않는다
		return;
	}
	MemCpy(&hello, payload, sizeof(hello));
	ServerHello reply{};
	LoginSession& session = getSession(id);
	if (hello.Type != eLoginType::ClientHello || !AcceptClientHello(hello, mSignSecretKey, &reply, &session.Cipher))
	{
		disconnectBadPacket(id); // 종류가 틀리거나 잘못된 공개 키
		return;
	}
	SendPacket(id, reinterpret_cast<const char*>(&reply), sizeof(reply)); // ServerHello만 평문 - 아직 요청을 받기 전이라 다른 송신과 겹치지 않는다
}

void LoginServer::sendSecure(const uint64_t id, const void* data, const int32_t len)
{
	LoginSession& session = getSession(id);
	std::string cipher;
	std::lock_guard<std::mutex> lock(session.SendLock); // 순번을 매긴 순서대로 송신 링에 들어가야 받는 쪽이 푼다
	session.Cipher.Seal(data, static_cast<size_t>(len), &cipher);
	SendPacket(id, cipher.data(), static_cast<int32_t>(cipher.size()));
}

void LoginServer::onRecvPacket(const uint64_t id, const char* payload, const int32_t len)
{
	if (len < static_cast<int32_t>(sizeof(eLoginType)) || getSession(id).bPending)
	{
		disconnectBadPacket(id); // 종류도 없거나, 응답을 받기 전에 다음 요청(한 세션은 한 번에 하나)
		return;
	}
	eLoginType type;
	MemCpy(&type, payload, sizeof(type));
	switch (type)
	{
	case eLoginType::RegisterReq:
		onRegisterReq(id, payload, len);
		break;
	case eLoginType::AuthReq:
		onAuthReq(id, payload, len);
		break;
	default:
		disconnectBadPacket(id); // 모르는 종류, 또는 서버 → 클라 종류
		break;
	}
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
	if (res.Result == eRegisterResult::Ok && mAuthThread->GetPendingCnt() >= AUTH_QUEUE_MAX)
	{
		res.Result = eRegisterResult::ServerError; // 밀려 있다 - 잠시 뒤 다시
	}
	if (res.Result != eRegisterResult::Ok || !HoldSession(id)) // 붙잡기: 인증 스레드가 응답할 때까지 해제되지 않게
	{
		wipe(&password);
		if (res.Result != eRegisterResult::Ok)
		{
			sendSecure(id, &res, sizeof(res));
		}
		return;
	}
	getSession(id).bPending = true;
	mAuthThread->Post([this, id, accountID, password](DbConn& conn) mutable
		{
			const RegisterRes result{ eLoginType::RegisterRes, registerOnAuthThread(conn, accountID, password) };
			wipe(&password);
			getSession(id).bPending = false; // 표시를 내린 뒤 응답 - 응답을 받자마자 보낸 다음 요청이 표시에 걸리지 않게
			sendSecure(id, &result, sizeof(result));
			UnholdSession(id);
		});
	wipe(&password); // 람다가 사본을 가졌다 - 워커의 것은 여기서 지운다
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
	if (mAuthThread->GetPendingCnt() >= AUTH_QUEUE_MAX)
	{
		wipe(&password);
		AuthRes busy{};
		busy.Type = eLoginType::AuthRes;
		busy.Result = eAuthResult::ServerError; // 밀려 있다 - 해시 없이 바로 거절
		sendSecure(id, &busy, sizeof(busy));
		return;
	}
	if (!HoldSession(id))
	{
		wipe(&password);
		return;
	}
	getSession(id).bPending = true;
	mAuthThread->Post([this, id, accountID, password](DbConn& conn) mutable
		{
			AuthRes result = authOnAuthThread(conn, accountID, password);
			wipe(&password);
			LoginSession& session = getSession(id); // 붙잡고 있어 이 칸은 아직 이 세션 것이다
			bool bTooManyFails = false;
			if (result.Result == eAuthResult::Fail)
			{
				++session.FailCnt;
				bTooManyFails = (session.FailCnt >= LOGIN_FAIL_MAX);
			}
			if (bTooManyFails)
			{
				// 끊을 때는 응답 → 끊기 → 표시 내리기 - 먼저 내리면 응답을 받자마자 보낸 다음 요청이 끊기 전에 받아들여진다
				sendSecure(id, &result, sizeof(result));
				Disconnect(id); // 한 세션에서 계속 틀림 - 다시 접속하는 비용을 더한다(설계 6장, 계정·주소별 제한은 없다)
				session.bPending = false;
			}
			else
			{
				// 평소에는 표시를 내린 뒤 응답 - 응답을 받자마자 보낸 다음 요청이 아직 켜진 표시에 걸려 끊기지 않게
				session.bPending = false;
				sendSecure(id, &result, sizeof(result));
			}
			sodium_memzero(result.SessionKey, sizeof(result.SessionKey));
			UnholdSession(id);
		});
	wipe(&password);
}

eRegisterResult LoginServer::registerOnAuthThread(DbConn& conn, const std::string& accountID, const std::string& password)
{
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
	if (!conn.Exec("INSERT INTO credentials (account_id, password_hash) VALUES (" + idLiteral + ", '" + hash + "')"))
	{
		// 같은 아이디가 동시에 가입하면 UNIQUE에 걸린다 - 다시 읽어 있으면 Exists
		const bool bQueried = conn.Query("SELECT 1 FROM credentials WHERE account_id = " + idLiteral, &rows);
		return (bQueried && !rows.empty()) ? eRegisterResult::Exists : eRegisterResult::ServerError;
	}
	++mRegisterCnt;
	return eRegisterResult::Ok;
}

AuthRes LoginServer::authOnAuthThread(DbConn& conn, const std::string& accountID, const std::string& password)
{
	AuthRes res{};
	res.Type = eLoginType::AuthRes;
	res.Result = eAuthResult::Fail;
	std::vector<std::vector<std::string>> rows;
	std::string hash;
	uint64_t accountNo = 0;
	// 아이디가 없거나 형식이 틀려도 해시 한 번만큼 걸리게 가짜 확인을 한다 - 로그인 응답의 시간으로 아이디가 있는지 가리기 어렵게
	// (가입 응답은 "이미 있음"을 알려 주므로 아이디가 있는지 자체는 숨기지 못한다 - 비밀번호를 맞혀 보는 쪽만 막는 목적)
	if (!isValidID(accountID) || !isValidPassword(password))
	{
		mHasher->VerifyDummy(password);
	}
	else if (!conn.Query("SELECT account_no, password_hash FROM credentials WHERE account_id = " + toHexLiteral(accountID), &rows))
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
			conn.Exec("UPDATE credentials SET password_hash = '" + newHash + "' WHERE account_no = " + std::to_string(accountNo));
		}
	}
	if (res.Result == eAuthResult::Ok)
	{
		// 세션 키 - 게임 서버는 Redis에서, 클라는 이 응답에서 받는다. 30초 안에 한 번만 쓰인다(게임 서버가 확인 뒤 지움)
		randombytes_buf(res.SessionKey, sizeof(res.SessionKey));
		std::string value(reinterpret_cast<const char*>(res.SessionKey), sizeof(res.SessionKey));
		value += accountID;
		const bool bStored = mRedis->SetEx("session:" + std::to_string(accountNo), value, SESSION_KEY_TTL_SEC);
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
		<< " 인증대기=" << server.GetAuthPendingCnt();  // 인증 스레드가 아직 처리하지 않은 요청
	return out;
}
