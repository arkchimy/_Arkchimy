#include <ctime>
#include "GameServer.h"
#include "DbThread.h"
#include "LoginProtocol.h"
#include "SecureChannel.h"
#include "SessionAuth.h"

enum : int64_t
{
	LOGIN_DB_QUEUE_MAX = 500, // DB 대기열이 이만큼 밀렸으면 입장을 바로 거절(장시간 시험의 평소 최대는 0–5)
};
// 입장 실패는 어떤 이유든 응답 없이 바로 끊는다(2026-11-20 사용자 결정) - 키가 없거나 틀린 연결에 서버가 보낼 것이 없다. 클라는 로그인 서버부터 다시

GameServer::GameServer(const int32_t maxSession, const GameRule& rule, GameDB* gameDB, RedisClient* redis)
	: mPlayers(maxSession)
	, mRule(rule)
	, mGameDB(gameDB)
	, mRedis(redis)
{
	Assert(gameDB != nullptr && redis != nullptr, "GameServer: gameDB·redis가 null");
	mRoomMembers.reserve(static_cast<size_t>(maxSession)); // 한 세션은 방에 한 번만 - 재할당 없이
	if (rule.EntryLimitSec > 0 || rule.IdleLimitSec > 0)
	{
		mEntryWatch.Start(1000, std::bind(&GameServer::checkEntryDeadline, this));
	}
}

void GameServer::checkEntryDeadline()
{
	// 접속만 하고 입장하지 않는 연결이 세션 칸(maxSession)을 채워 새 사용자를 막지 못하게(운영자 서버의 인증 기한과 같은 방식)
	// 입장한 뒤에는 하트비트가 끊긴 연결(랜선이 뽑힌 클라 등)을 정리한다 - TCP는 보낼 것이 없으면 상대가 사라진 것을 모른다
	// 칸마다 송신 락으로 잠깐 보고, 끊기는 락 밖에서 - 이미 끊겼거나 칸이 다른 세션으로 바뀐 ID면 Disconnect가 아무 일도 안 한다
	const ULONGLONG entryLimitMs = static_cast<ULONGLONG>(mRule.EntryLimitSec) * 1000;
	const ULONGLONG idleLimitMs = static_cast<ULONGLONG>(mRule.IdleLimitSec) * 1000;
	std::vector<uint64_t> entryExpired;
	std::vector<uint64_t> idleExpired;
	for (Player& player : mPlayers)
	{
		const uint64_t entrySessionID = (entryLimitMs > 0) ? player.GetEntryExpiredSessionID(entryLimitMs) : 0;
		if (entrySessionID != 0)
		{
			entryExpired.push_back(entrySessionID);
			continue;
		}
		const uint64_t idleSessionID = (idleLimitMs > 0) ? player.GetIdleExpiredSessionID(idleLimitMs) : 0;
		if (idleSessionID != 0)
		{
			idleExpired.push_back(idleSessionID);
		}
	}
	for (const uint64_t sessionID : entryExpired)
	{
		Disconnect(sessionID);
		++mEntryTimeoutCnt; // 그사이 스스로 끊긴 세션도 셀 수 있다 - 통계용
	}
	for (const uint64_t sessionID : idleExpired)
	{
		Disconnect(sessionID);
		++mIdleTimeoutCnt;
	}
}

GameServer::~GameServer()
{
	Shutdown(); // 파생 소멸자에서 멈춘다 - 기반 소멸자 시점엔 파생 부분이 없어 OnRelease가 순수 가상 호출이 된다
}

void GameServer::Shutdown()
{
	mEntryWatch.Stop(); // 워커 밖 스레드는 라이브러리 Stop 전에 멈춘다
	mbClosing = true;   // 이 뒤로 OnRecv는 패킷을 버린다
	while (mRecvInProgressCnt != 0)
	{
		Sleep(1); // 이미 처리 중이던 패킷이 끝나기를 - 그 패킷이 넣는 DB 요청까지 DB 스레드가 받은 뒤에 멈춘다
	}
	mGameDB->GetThread().Stop(); // 남은 일(입장·저장)을 모두 끝낸다
	Stop();            // 두 번 불러도 된다(라이브러리 Stop은 이미 멈췄으면 그냥 돌아온다)
}

void GameServer::OnAccept(const uint64_t id, const SOCKADDR_IN& remoteAddr)
{
	Player& player = getPlayer(id);
	Assert(player.GetSessionID() == 0, "OnAccept: 이전 세션의 Player가 정리되지 않음");
	player.Init(id, remoteAddr); // 접속만 한 연결에는 아무것도 보내지 않는다 - 클라가 먼저 입장 요청을 보낸다(설계 게임서버_입장_변경.docx)
}

void GameServer::sendGame(const uint64_t id, const eGameType type, const void* body, const size_t bodyLen)
{
	// 입장 뒤 게임 서버의 모든 송신이 여기를 지난다 - 통일 모양 패킷(설계 게임서버_입장_변경.docx 2판 3-1)
	// 한 세션에 워커(다른 세션의 채팅 방송)·DB 스레드가 동시에 보낼 수 있어, SeqNumber + 암호화 + 송신 링에 넣기를 칸의 송신 락 안에서 한 번에 한다
	// (락 순서: 방 락 → 송신 락 → 라이브러리 세션 락. 라이브러리는 자기 락을 쥔 채 게임 코드를 부르지 않는다)
	Player& player = getPlayer(id);
	std::string packet;
	std::lock_guard<std::mutex> lock(player.GetSendLock());
	if (player.PrepareSendLocked(id, type, body, bodyLen, &packet))
	{
		SendPacket(id, packet.data(), static_cast<int32_t>(packet.size()));
	}
}

void GameServer::OnRecv(const uint64_t id, const char* payload, const int32_t len)
{
	// 처리 중 수를 먼저 올리고 종료 표시를 본다 - Shutdown은 표시를 세운 뒤 이 수가 0이 되기를 기다리므로, 둘 중 하나는 반드시 상대를 본다
	++mRecvInProgressCnt;
	if (!mbClosing)
	{
		// 연결 상태마다 받을 수 있는 것이 하나다(흐름 0-3) - 다른 것이 오면 끊는다
		Player& player = getPlayer(id);
		switch (player.GetState())
		{
		case eEntryState::WaitHello:
			onHello(id, payload, len);
			break;
		case eEntryState::WaitEnter:
			onLoginReq(id, payload, len);
			break;
		case eEntryState::Entered:
		{
			// 같은 세션의 OnRecv는 동시에 불리지 않아 받는 SeqNumber는 락이 없다
			eGameType type = eGameType::Hello;
			std::string body;
			if (!player.OpenReceived(payload, len, &type, &body))
			{
				disconnectBadPacket(id); // SeqNumber·태그가 틀림(변조·다시 보내기·건너뛰기·되돌리기·평문)
			}
			else if (type == eGameType::ChatReq)
			{
				player.TouchRecv();
				onChatReq(id, body);
			}
			else if (type == eGameType::Heartbeat && body.empty())
			{
				player.TouchRecv(); // 마지막 수신 시각만 - 유휴 기한(checkEntryDeadline)
			}
			else
			{
				disconnectBadPacket(id); // 입장 요청 다시, 서버 → 클라 종류, 모르는 종류
			}
			break;
		}
		case eEntryState::Pending:
		default:
			disconnectBadPacket(id); // 입장 처리 중에는 아무것도 받지 않는다
			break;
		}
	}
	--mRecvInProgressCnt;
}

void GameServer::OnRelease(const uint64_t id)
{
	Player& player = getPlayer(id);
	Assert(player.GetSessionID() == id, "OnRelease: 다른 세션의 Player");
	leaveRoom(id); // 방에 없으면(입장 전) 아무 일도 없다
	const Account* account = player.GetAccount();
	if (account != nullptr)
	{
		mAccountTable.Logout(account->GetID(), id); // 접속 표시가 이 세션일 때만 지운다 - 같은 계정의 새 입장이 가져갔으면 그대로
	}
	player.Clear();
}

void GameServer::onHello(const uint64_t id, const char* payload, const int32_t len)
{
	Hello hello{};
	if (len == static_cast<int32_t>(sizeof(Hello)))
	{
		MemCpy(&hello, payload, sizeof(hello));
	}
	if (hello.Type != eGameType::Hello)
	{
		disconnectBadPacket(id); // 길이가 틀리거나 Hello가 아님(입장 요청부터 보냄 포함)
		return;
	}
	// Redis는 보지 않는다 - Hello만으로는 DB 스레드 일이 생기지 않게(설계 2판 5-1 A). 같은 10바이트를 돌려줘 받은 것보다 많이 보내지 않는다
	const Challenge challenge{ eGameType::Challenge, MakeStartSeq() };
	Player& player = getPlayer(id);
	std::lock_guard<std::mutex> lock(player.GetSendLock()); // 상태와 Challenge 송신을 한 번에 - 다른 스레드는 입장 전 세션에 보내지 않는다
	player.AcceptHelloLocked(hello.AccountNo, challenge.StartSeq);
	SendPacket(id, reinterpret_cast<const char*>(&challenge), sizeof(challenge)); // 평문 - 아직 키를 쓸 수 없다
}

void GameServer::onLoginReq(const uint64_t id, const char* payload, const int32_t len)
{
	Player& player = getPlayer(id);
	PacketHead head{};
	if (len == LOGIN_REQ_PACKET_LEN)
	{
		MemCpy(&head, payload, sizeof(head));
	}
	if (head.Type != eGameType::LoginReq)
	{
		disconnectBadPacket(id); // 길이가 틀리거나 입장 요청이 아님(Hello 두 번 포함)
		return;
	}
	if (head.Seq != player.GetStartSeq())
	{
		++mEntryAuthFailCnt; // SeqNumber가 이 연결의 시작값이 아니다 - 다른 연결에서 가로챈 입장 요청. 풀지 않고, Redis 키도 건드리지 않는다
		Disconnect(id);
		return;
	}
	// 입장은 인증 전에 받는 패킷이라 접속을 많이 열어 보내면 DB 스레드 대기열이 한없이 길어질 수 있다 - 많이 밀렸으면 바로 거절
	if (mGameDB->GetThread().GetPendingCnt() >= LOGIN_DB_QUEUE_MAX)
	{
		++mEntryRejectCnt;
		Disconnect(id); // 밀려 있다 - Redis에 묻지 않고 바로 끊는다(응답 없음)
		return;
	}
	// 세션 키 확인(Redis)과 계정 읽기(MySQL)는 블록되므로 DB 스레드에 넘긴다. 결과를 보낼 때까지 이 세션이 해제되지 않게 붙잡는다
	if (!HoldSession(id))
	{
		return; // 내 콜백 안이라 실패하지 않는다 - 실패하면 이미 해제 중
	}
	player.SetState(eEntryState::Pending); // 이 뒤에 오는 패킷은 잘못된 패킷 - Hello의 AccountNo·시작값은 대기열에 넣기 전에 적혀 있어 DB 스레드가 본다
	mGameDB->GetThread().Post(std::bind(&GameServer::finishEntryOnDbThread, this, std::placeholders::_1, id, std::string(payload, static_cast<size_t>(len))));
}

void GameServer::finishEntryOnDbThread(DbConn& conn, const uint64_t id, const std::string& packet)
{
	std::string accountID;
	eLoginResult result = verifyEntryOnDbThread(id, packet, &accountID);
	if (result == eLoginResult::Ok)
	{
		result = loginAccountOnDbThread(conn, id, accountID);
	}
	Player& player = getPlayer(id); // 붙잡고 있어 이 칸은 아직 이 세션 것이다
	if (result == eLoginResult::Ok)
	{
		// 방 락 안에서 응답을 넣고 방에 들어간다(락 순서: 방 → 송신). 방송도 방 락 안이라
		// ① 응답보다 먼저 채팅 알림이 가지 않고 ② 클라가 응답을 받자마자 보낸 첫 채팅도 방에 들어간 뒤 방송돼 자기 채팅을 받는다
		// (응답을 보낸 뒤 방에 넣으면 그 사이에 처리된 첫 채팅은 보낸 사람에게 가지 않는다 - README_ERRORS 2-16)
		std::lock_guard<std::mutex> roomLock(mRoomLock);
		{
			// 상태를 "입장"으로 바꾼 뒤 → 입장 응답(통일 모양, 서버 방향 SeqNumber = 시작값)을 만들어 → 보낸다. 같은 송신 락 안이라 다른 송신이 끼지 않는다
			// (보낸 뒤에 바꾸면, 클라가 응답을 받자마자 보낸 첫 메시지를 워커가 아직 입장 처리 중으로 보고 끊는다 - README_ERRORS 2-14)
			std::lock_guard<std::mutex> lock(player.GetSendLock());
			player.OpenChannelLocked();
			const LoginResBody res{ eLoginResult::Ok };
			std::string out;
			if (player.PrepareSendLocked(id, eGameType::LoginRes, &res, sizeof(res), &out))
			{
				SendPacket(id, out.data(), static_cast<int32_t>(out.size()));
			}
		}
		mRoomMembers.push_back(id);
	}
	else
	{
		// 입장 실패 - 응답 없이 끊는다. 상태는 입장 처리 중 그대로(끊기 전에 온 다음 패킷은 잘못된 패킷으로). 통로 키는 지운다
		{
			std::lock_guard<std::mutex> lock(player.GetSendLock());
			player.CancelEntryLocked();
		}
		Disconnect(id);
		if (result == eLoginResult::AuthFail)
		{
			++mEntryAuthFailCnt; // 키가 없음(30초 지남·이미 씀)·태그 틀림·AccountNo가 Hello와 다름
		}
		else
		{
			++mEntryRejectCnt; // 차단·DB 오류
		}
	}
	UnholdSession(id);
}

eLoginResult GameServer::verifyEntryOnDbThread(const uint64_t id, const std::string& packet, std::string* outAccountID)
{
	Assert(outAccountID != nullptr, "verifyEntryOnDbThread: outAccountID가 null");
	Player& player = getPlayer(id); // 붙잡고 있어 이 칸은 아직 이 세션 것이다
	const std::string key = "session:" + std::to_string(player.GetHelloAccountNo());
	std::string value;
	bool bFound = false;
	if (!mRedis->Get(key, &value, &bFound))
	{
		return eLoginResult::DbError; // Redis 오류 - 잠시 뒤 다시
	}
	// 값 = 세션 키 32바이트 + 아이디(로그인 서버가 넣는다). 30초가 지났거나 이미 쓴 키면 없다
	if (!bFound || value.size() <= SESSION_KEY_LEN || value.size() >= SESSION_KEY_LEN + ACCOUNT_ID_LEN)
	{
		return eLoginResult::AuthFail;
	}
	eLoginResult result = eLoginResult::AuthFail;
	uint8_t sessionKey[SESSION_KEY_LEN];
	MemCpy(sessionKey, value.data(), sizeof(sessionKey));
	bool bOpened = false;
	{
		// 이 키로 입장 요청을 푼다 - 태그가 맞고 본문의 AccountNo가 Hello와 같으면 보낸 쪽이 같은 키를 가졌다. 통로 키는 이 칸의 것으로 둔다
		std::lock_guard<std::mutex> lock(player.GetSendLock());
		bOpened = player.BeginEntryLocked(sessionKey, packet.data(), packet.size());
	}
	if (bOpened) // 틀리면 지우지 않는다 - 키를 모르는 가짜 요청으로 남의 키를 지워 진짜 사용자를 막지 못하게
	{
		// 확인한 뒤에, 읽은 값과 같을 때만 지운다. 지운 결과가 1인 연결만 통과 - GET 뒤에 같은 계정이 다시 로그인해 새 키로 덮어썼으면 새 키는 남는다
		int64_t removedCnt = 0;
		if (!mRedis->DelIfEquals(key, value, &removedCnt))
		{
			result = eLoginResult::DbError; // 끊긴 지우기는 다시 하지 않는다(RedisClient) - 클라가 로그인 서버에서 다시
		}
		else if (removedCnt == 1)
		{
			result = eLoginResult::Ok;
			*outAccountID = value.substr(SESSION_KEY_LEN);
		}
	}
	SecureZeroMemory(sessionKey, sizeof(sessionKey));
	SecureZeroMemory(value.data(), value.size());
	return result;
}

eLoginResult GameServer::loginAccountOnDbThread(DbConn& conn, const uint64_t id, const std::string& accountID)
{
	// 계정은 DB 스레드만 메모리에 올린다(입장·운영 명령) - 이미 있으면 메모리가 기준이라 DB를 다시 읽지 않는다
	UniquePtr<Account> loaded;
	if (!mAccountTable.Has(accountID))
	{
		loaded = GameDB::LoadOrCreate(conn, accountID);
		if (loaded == nullptr)
		{
			return eLoginResult::DbError; // 세션 키는 이미 지웠다 - 클라는 로그인 서버부터 다시
		}
	}
	Account* account = nullptr;
	uint64_t replacedSessionID = 0;
	const eLoginResult result = mAccountTable.Login(accountID, id, std::move(loaded), &account, &replacedSessionID);
	if (result == eLoginResult::Ok)
	{
		getPlayer(id).SetAccount(account); // 다 만든 계정만 넣는다
		if (replacedSessionID != 0)
		{
			// 같은 계정이 이미 입장해 있었다 - 기존 연결을 끊고 새 연결이 입장한다(설계 2판 4장). 기존 세션의 OnRelease는 접속 표시를 지우지 않는다
			// 끊긴 줄 모르는 이전 접속(클라 전원·랜선)도 이렇게 정리된다. 이미 끊긴 ID면 Disconnect는 아무 일도 안 한다
			Disconnect(replacedSessionID);
		}
	}
	return result;
}

void GameServer::leaveRoom(const uint64_t id)
{
	std::lock_guard<std::mutex> lock(mRoomLock);
	for (size_t i = 0; i < mRoomMembers.size(); ++i)
	{
		if (mRoomMembers[i] == id)
		{
			mRoomMembers[i] = mRoomMembers.back();
			mRoomMembers.pop_back();
			return;
		}
	}
}

void GameServer::onChatReq(const uint64_t id, const std::string& text)
{
	if (text.empty() || text.size() > CHAT_TEXT_MAX)
	{
		disconnectBadPacket(id); // 빈 채팅이거나 너무 길다
		return;
	}
	ChatNotifyBody notify{};
	const std::string& senderID = getPlayer(id).GetAccount()->GetID(); // 입장한 세션이라 계정이 있다. 아이디는 만든 뒤 바뀌지 않아 락 없이 읽는다
	MemCpy(notify.SenderID, senderID.data(), senderID.size()); // 15바이트 이하(로그인 서버가 검사) - 나머지는 0
	MemCpy(notify.Text, text.data(), text.size());
	const size_t notifyLen = offsetof(ChatNotifyBody, Text) + text.size();

	// 방 락 안에서 구성원 모두에게 - 워커 둘이 동시에 방송해도 받는 사람마다 같은 순서. 끊긴 세션이면 sendGame이 버린다
	std::lock_guard<std::mutex> lock(mRoomLock);
	for (const uint64_t memberID : mRoomMembers)
	{
		sendGame(memberID, eGameType::ChatNotify, &notify, notifyLen);
	}
	++mChatCnt;
}
Player& GameServer::getPlayer(const uint64_t id)
{
	const uint32_t index = static_cast<uint32_t>(id); // 라이브러리의 세션 ID = (시퀀스 번호 << 32) | 인덱스
	Assert(index < mPlayers.size(), "getPlayer: 인덱스 범위 밖 - 생성자의 maxSession과 Start의 MaxSession이 다름");
	return mPlayers[index];
}

void GameServer::disconnectBadPacket(const uint64_t id)
{
	++mBadPacketCnt;
	Disconnect(id);
}

bool GameServer::postExternal(DbThread::Job job)
{
	++mRecvInProgressCnt; // OnRecv와 같은 이유 - Shutdown이 DB 스레드를 멈춘 뒤 요청이 들어가지 않게
	const bool bAccepted = !mbClosing;
	if (bAccepted)
	{
		mGameDB->GetThread().Post(std::move(job));
	}
	--mRecvInProgressCnt;
	return bAccepted;
}

void GameServer::AdminBan(const std::string& accountID, const int32_t minutes, const std::function<void(const AdminBanRes&)>& reply)
{
	if (!postExternal(std::bind(&GameServer::banJobOnDbThread, this, std::placeholders::_1, accountID, minutes, reply)))
	{
		AdminBanRes res{};
		res.Type = eAdminType::BanRes;
		res.Result = eBanResult::DbError; // 서버 종료 중
		reply(res);
	}
}

void GameServer::banJobOnDbThread(DbConn& conn, const std::string& accountID, const int32_t minutes, const std::function<void(const AdminBanRes&)>& reply)
{
	AdminBanRes res{};
	res.Type = eAdminType::BanRes;
	banOnDbThread(conn, accountID, minutes, &res);
	reply(res);
}

void GameServer::banOnDbThread(DbConn& conn, const std::string& accountID, const int32_t minutes, AdminBanRes* outRes)
{
	Assert(outRes != nullptr, "banOnDbThread: outRes가 null");
	if (minutes < 0 || minutes > ADMIN_BAN_MINUTES_MAX)
	{
		outRes->Result = eBanResult::BadValue; // 운영 툴이 보낸 값 - 끊지 않고 거절
		return;
	}
	const int64_t banUntil = (minutes > 0) ? static_cast<int64_t>(time(nullptr)) + static_cast<int64_t>(minutes) * 60 : 0;
	Account* const account = mAccountTable.Find(accountID);
	if (account != nullptr)
	{
		// 메모리 계정 - 입장은 메모리의 차단 값을 보므로 메모리를 먼저, 저장은 같은 계정 락 안에서(같은 계정의 저장 순서 = 바꾼 순서)
		std::lock_guard<std::mutex> accountLock(account->GetLock());
		account->SetBanUntil(banUntil);
		mGameDB->SaveBan(accountID, banUntil);
		outRes->Result = eBanResult::Ok;
	}
	else
	{
		// DB에만 있는 계정 - 이 스레드에서 바로 써야 뒤에 줄 선 그 계정의 입장이 차단 값을 읽는다
		std::vector<std::vector<std::string>> rows;
		if (!conn.Query("SELECT 1 FROM accounts WHERE account_id = " + GameDB::ToHexLiteral(accountID), &rows))
		{
			outRes->Result = eBanResult::DbError;
		}
		else if (rows.empty())
		{
			outRes->Result = eBanResult::NotFound;
		}
		else
		{
			outRes->Result = DbThread::RunTransaction(conn, { GameDB::MakeBanSQL(accountID, banUntil) }, "제재(DB만)") ? eBanResult::Ok : eBanResult::DbError;
		}
	}
	if (outRes->Result == eBanResult::Ok && banUntil > 0)
	{
		const uint64_t onlineSessionID = mAccountTable.GetOnlineSessionID(accountID);
		if (onlineSessionID != 0)
		{
			Disconnect(onlineSessionID); // 강제 종료 - 다시 입장하면 Banned
			outRes->bKicked = 1;
		}
	}
}

int64_t GameServer::GetOnlineCnt() const
{
	return mAccountTable.GetOnlineCnt();
}

int64_t GameServer::GetAccountCnt() const
{
	return mAccountTable.GetAccountCnt();
}

int64_t GameServer::GetRoomMemberCnt() const
{
	std::lock_guard<std::mutex> lock(mRoomLock);
	return static_cast<int64_t>(mRoomMembers.size());
}

int64_t GameServer::GetDbPendingCnt() const
{
	return mGameDB->GetThread().GetPendingCnt();
}

int64_t GameServer::GetDbFailCnt() const
{
	return mGameDB->GetThread().GetFailCnt();
}

std::ostream& operator<<(std::ostream& out, const GameServer& server)
{
	const NetStat stat = server.GetStat();
	out << "[stat] 세션=" << server.GetSessionCnt()
		<< " 수락=" << server.GetAcceptCnt()
		<< " 해제=" << server.GetReleaseCnt()
		<< " 비정상헤더=" << stat.BadPacketCnt      // 라이브러리가 헤더 길이로 끊은 수
		<< " 넘침끊김=" << stat.OverflowCnt         // 송신 링이 가득 차 끊은 수
		<< " 송신정체끊김=" << stat.SendStallCnt    // 걸린 WSASend가 기준 시간 넘게 안 끝나 끊은 수
		<< " 슬롯부족=" << stat.SessionFullCnt      // 빈 세션이 없어 받자마자 끊은 수
		<< " 잘못된패킷=" << server.GetBadPacketCnt() // 컨텐츠가 종류·길이로 끊은 수
		<< " 로그인=" << server.GetOnlineCnt()        // 지금 입장한 아이디
		<< " 계정=" << server.GetAccountCnt()         // 메모리에 올린 계정(내리지 않아 계속 는다 - 메모리 증가를 볼 때 함께 본다)
		<< " 방인원=" << server.GetRoomMemberCnt()    // 채팅방에 있는 세션
		<< " 채팅=" << server.GetChatCnt()            // 방송한 채팅(누적)
		<< " DB대기=" << server.GetDbPendingCnt()     // DB 스레드가 아직 처리하지 않은 요청
		<< " DB실패=" << server.GetDbFailCnt()        // 되돌린 트랜잭션(누적)
		<< " 입장기한끊김=" << server.GetEntryTimeoutCnt() // 접속한 뒤 입장 기한 안에 입장하지 않아 끊은 수(누적)
		<< " 무응답끊김=" << server.GetIdleTimeoutCnt()    // 입장한 뒤 유휴 기한 동안 아무것도(하트비트 포함) 받지 못해 끊은 수(누적)
		<< " 입장확인실패=" << server.GetEntryAuthFailCnt() // 입장 요청의 키가 없거나(30초 지남·이미 씀) 틀려 응답 없이 끊은 수(누적) - 정상이면 0
		<< " 입장거절=" << server.GetEntryRejectCnt();      // 키는 맞았지만 이미 접속 중·차단·DB 오류(대기열 초과 포함)로 응답 없이 끊은 수(누적)
	return out;
}
