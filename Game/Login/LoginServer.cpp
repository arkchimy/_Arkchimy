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

LoginServer::LoginServer(const int32_t maxSession, DbThread* authThread, RedisClient* redis, const PasswordHasher* hasher, const RsaKey* signKey, RsaKeyPool* keyPool,
	const int32_t idleLimitSec, const int32_t ackLimitSec)
	: mSessions(maxSession)
	, mAuthThread(authThread)
	, mRedis(redis)
	, mHasher(hasher)
	, mSignKey(signKey)
	, mKeyPool(keyPool)
	, mIdleLimitMs(static_cast<ULONGLONG>(idleLimitSec) * 1000)
	, mAckLimitMs(static_cast<ULONGLONG>(ackLimitSec) * 1000)
{
	Assert(authThread != nullptr && redis != nullptr && hasher != nullptr && signKey != nullptr && signKey->IsReady() && keyPool != nullptr,
		"LoginServer: 인증 스레드·Redis·해시·서버 고정 키·키 쌍 풀이 없음");
	Assert(ackLimitSec > 0, "LoginServer: ackLimitSec가 0 이하 - 확인 기한은 끌 수 없다");
	// 확인 기한은 항상 보므로 감시 스레드는 요청 기한을 꺼도 돈다
	mIdleWatch.Start(1000, [this]()
		{
			checkIdleDeadline();
		});
}

LoginServer::~LoginServer()
{
	Shutdown(); // 파생 소멸자에서 멈춘다 - 기반 소멸자 시점엔 OnRelease가 순수 가상 호출이 된다
}

void LoginServer::Shutdown()
{
	mIdleWatch.Stop(); // 워커 밖 스레드는 라이브러리 Stop 전에 멈춘다
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
	session.bRegistered = false;
	session.Channel.Clear(); // 임시 키는 이 연결의 ClientHello에서 새로 받는다
	session.LastActiveTick = GetTickCount64(); // 시각을 먼저, ID를 나중에 - 감시가 새 ID를 보면 새 시각도 보인다
	session.State = eLoginState::WaitHello;
	session.WatchSessionID = id;
}

void LoginServer::OnRelease(const uint64_t id)
{
	LoginSession& session = getSession(id);
	Assert(session.SessionID == id, "OnRelease: 다른 세션");
	session.WatchSessionID = 0;
	session.SessionID = 0; // 요청 중이었다면 HoldSession 때문에 인증 스레드의 UnholdSession 뒤에 불린다
	// 마지막 콜백 - 이 뒤로 이 세션에 보내는 스레드가 없다. 서버 임시 키는 풀에 돌려주고(maxUseCnt번 썼으면 풀이 지운다) 나머지 통로 상태는 지운다
	if (session.KeyUseCnt > 0)
	{
		mKeyPool->Return(session.Channel.ReleaseMine(), session.KeyUseCnt);
		session.KeyUseCnt = 0;
	}
	session.Channel.Clear();
}

void LoginServer::OnRecv(const uint64_t id, const char* payload, const int32_t len)
{
	++mRecvInProgressCnt; // 먼저 올리고 종료 표시를 본다 - GameServer::OnRecv와 같은 이유
	if (!mbClosing)
	{
		LoginSession& session = getSession(id);
		if (session.State == eLoginState::WaitHello)
		{
			onClientHello(id, payload, len); // 첫 패킷은 임시 공개 키 주고받기
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
	if (hello.Type != eLoginType::ClientHello)
	{
		disconnectBadPacket(id);
		return;
	}
	LoginSession& session = getSession(id);
	UniquePtr<RsaKey> serverKey = mKeyPool->Take(&session.KeyUseCnt);
	if (serverKey == nullptr)
	{
		++mKeyEmptyCnt; // 미리 만든 키 쌍이 떨어졌다(접속 폭주) - 아직 잠가 보낼 통로가 없어 응답 없이 끊는다. 이미 접속한 세션의 요청은 계속 처리된다
		Disconnect(id);
		return;
	}
	ServerHello reply{};
	if (!session.Channel.AcceptClient(hello, mSignKey, std::move(serverKey), &reply)) // 서명 한 번(약 0.5ms) - 워커에서
	{
		disconnectBadPacket(id); // 잘못된 클라 공개 키
		return;
	}
	session.State = eLoginState::Ready; // 보내기 전에 - 같은 세션의 OnRecv는 차례로 불리지만, 상태를 먼저 바꾸는 순서를 다른 응답과 맞춘다
	SendPacket(id, reinterpret_cast<const char*>(&reply), sizeof(reply)); // ServerHello만 평문 - 아직 요청을 받기 전이라 다른 송신과 겹치지 않는다
}

void LoginServer::sendSecure(const uint64_t id, const void* data, const int32_t len)
{
	LoginSession& session = getSession(id);
	std::string packet;
	std::lock_guard<std::mutex> lock(session.SendLock); // 번호를 매긴 순서대로 송신 링에 들어가야 받는 쪽의 서명 확인이 맞는다
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
		// 상태에 맞지 않는 메시지 - 응답 전에 다음 요청(Pending), 로그인 성공 뒤 다시 요청(KeySent), 끊기로 한 뒤(Closing), 모르는 종류, 서버 → 클라 종류
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
	if (res.Result == eRegisterResult::Ok && mAuthThread->GetPendingCnt() >= AUTH_QUEUE_MAX)
	{
		res.Result = eRegisterResult::ServerError; // 밀려 있다 - 잠시 뒤 다시
	}
	if (getSession(id).bRegistered)
	{
		wipe(&password);
		disconnectBadPacket(id); // 이 연결에서 이미 가입했다 - 한 연결에서 가입은 한 번(해시·INSERT를 계속 시키지 못하게)
		return;
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
	getSession(id).State = eLoginState::Pending;
	mAuthThread->Post([this, id, accountID, password](DbConn& conn) mutable
		{
			if (skipIfDisconnected(id))
			{
				wipe(&password);
				return;
			}
			const RegisterRes result{ eLoginType::RegisterRes, registerOnAuthThread(conn, accountID, password) };
			wipe(&password);
			LoginSession& session = getSession(id); // 붙잡고 있어 이 칸은 아직 이 세션 것이다
			session.bRegistered = (result.Result == eRegisterResult::Ok);
			session.LastActiveTick = GetTickCount64(); // 상태를 바꾸기 전에 - 대기열에서 오래 기다린 요청이 응답 직전에 기한으로 끊기지 않게
			session.State = eLoginState::Ready; // 상태를 바꾼 뒤 응답 - 응답을 받자마자 보낸 다음 요청이 Pending에 걸리지 않게
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
	getSession(id).State = eLoginState::Pending;
	mAuthThread->Post([this, id, accountID, password](DbConn& conn) mutable
		{
			if (skipIfDisconnected(id))
			{
				wipe(&password);
				return;
			}
			AuthRes result = authOnAuthThread(conn, accountID, password);
			wipe(&password);
			LoginSession& session = getSession(id); // 붙잡고 있어 이 칸은 아직 이 세션 것이다
			session.LastActiveTick = GetTickCount64(); // 상태를 바꾸기 전에 - KeySent면 이 시각부터 확인 기한을 센다
			bool bTooManyFails = false;
			if (result.Result == eAuthResult::Fail)
			{
				++session.FailCnt;
				bTooManyFails = (session.FailCnt >= LOGIN_FAIL_MAX);
			}
			if (bTooManyFails)
			{
				// 상태를 Closing으로 바꾼 뒤 응답 → 끊기 - 응답을 받자마자 보낸 다음 요청은 Closing에 걸려 받아들여지지 않는다
				// 응답 직후 끊으므로 클라가 읽기 전에 RST가 닿으면 이 응답은 사라질 수 있다(실패 알림이라 클라는 끊김으로 처리한다)
				session.State = eLoginState::Closing;
				sendSecure(id, &result, sizeof(result));
				Disconnect(id); // 한 세션에서 계속 틀림 - 다시 접속하는 비용을 더한다(설계 6장, 계정·주소별 제한은 없다)
			}
			else
			{
				// 상태를 바꾼 뒤 응답 - 응답을 받자마자 보낸 다음 메시지(로그인 성공이면 AuthAck)가 Pending에 걸리지 않게
				// 로그인 성공이면 KeySent - 서버는 먼저 끊지 않고 AuthAck를 기다린다(먼저 끊으면 클라가 읽기 전에 RST가 닿아 세션 키가 사라질 수 있다)
				session.State = (result.Result == eAuthResult::Ok) ? eLoginState::KeySent : eLoginState::Ready;
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

bool LoginServer::skipIfDisconnected(const uint64_t id)
{
	// 대기열에서 기다리는 동안 클라가 끊었으면(시간 제한 등) 응답할 곳이 없다 - 해시(64MiB, 수십 ms)·가입 INSERT를 하지 않고 넘어간다
	// 끊긴 연결의 요청이 대기열에 쌓여 있어도 인증 스레드가 그만큼 오래 붙잡히지 않게. 확인 뒤 바로 끊기는 경우는 그대로 처리한다(결과는 같다)
	if (!IsDisconnecting(id))
	{
		return false;
	}
	++mSkippedCnt;
	getSession(id).State = eLoginState::Closing; // 이미 끊기는 연결 - 아무것도 받지 않는다
	UnholdSession(id);
	return true;
}

void LoginServer::checkIdleDeadline()
{
	// 접속만 하고 요청하지 않는 연결이 세션 칸(maxSession)을 채워 새 사용자를 막지 못하게. 요청 중(인증 대기열에서 기다리는 중 포함)은 끊지 않는다
	// 끊기는 이미 끊겼거나 칸이 다른 세션으로 바뀐 ID면 아무 일도 안 한다
	// 세션 키를 보낸 연결(KeySent)은 확인 기한 - 클라가 AuthAck도 끊기도 하지 않고 붙잡고 있지 못하게
	std::vector<uint64_t> idleExpired;
	std::vector<uint64_t> ackExpired;
	for (LoginSession& session : mSessions)
	{
		const uint64_t sessionID = session.WatchSessionID;
		if (sessionID == 0)
		{
			continue;
		}
		const eLoginState state = session.State; // 상태를 먼저, 시각을 나중에 - 상태를 바꾸는 쪽은 시각을 먼저 쓴다
		const bool bAckWait = (state == eLoginState::KeySent);
		const bool bIdleWait = (mIdleLimitMs > 0) && (state == eLoginState::WaitHello || state == eLoginState::Ready);
		if (!bAckWait && !bIdleWait)
		{
			continue; // 요청 처리 중(Pending)이거나 이미 끊는 중(Closing)
		}
		const ULONGLONG lastActive = session.LastActiveTick;
		const ULONGLONG now = GetTickCount64(); // 시각을 읽은 뒤에 - now가 앞서면 부호 없는 뺄셈이 넘친다(README_ERRORS 2-8)
		if (bAckWait && now - lastActive > mAckLimitMs)
		{
			ackExpired.push_back(sessionID);
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
		<< " 인증대기=" << server.GetAuthPendingCnt()  // 인증 스레드가 아직 처리하지 않은 요청
		<< " 기한끊김=" << server.GetIdleTimeoutCnt()  // 접속·응답 뒤 기한 안에 요청이 없어 끊은 수(누적)
		<< " 건너뜀=" << server.GetSkippedCnt()         // 클라가 이미 끊어 해시 없이 넘어간 요청(누적)
		<< " 키쌍=" << server.GetKeyReadyCnt()          // 미리 만들어 둔 서버 임시 키 쌍
		<< " 키쌍부족끊김=" << server.GetKeyEmptyCnt() // 키 쌍이 떨어져 ClientHello를 받지 않고 끊은 수(누적)
		<< " 키생성=" << server.GetKeyCreatedCnt()     // 만든 서버 임시 키 쌍(누적, 시작 때 채운 것 포함) - 돌려 쓰면 접속 수보다 적다
		<< " 확인받음=" << server.GetAckCnt()           // 세션 키를 보낸 뒤 AuthAck를 받고 끊은 수(누적)
		<< " 확인시간초과=" << server.GetAckTimeoutCnt(); // 세션 키를 보낸 뒤 기한 안에 AuthAck가 없어 끊은 수(누적). 이름에 "기한끊김"을 넣지 않는다 - 시험이 이름 일부로 찾는다
	return out;
}
