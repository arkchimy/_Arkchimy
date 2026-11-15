#include <cstdlib>
#include <ctime>
#include "GameServer.h"
#include "DbThread.h"
#include "ItemUID.h"
#include "LoginProtocol.h"
#include "SessionAuth.h"

enum : int64_t
{
	LOGIN_DB_QUEUE_MAX = 500, // DB 대기열이 이만큼 밀렸으면 입장을 바로 거절(장시간 시험의 평소 최대는 0–5)
	ENTRY_FAIL_MAX = 3,       // 한 연결에서 입장 확인이 이만큼 틀리면 끊는다 - 틀린 입장마다 저장과 같은 DB 스레드가 Redis를 읽는다.
	                          // 정상 클라도 키가 30초 지나 한두 번 틀릴 수 있어 1이 아니라 3(로그인 서버는 비밀번호라 5)
};

GameServer::GameServer(const int32_t maxSession, const GameData* gameData
	, const GameRule& rule, GameDB* gameDB, RedisClient* redis, const uint64_t lastAuctionID)
	: mPlayers(maxSession)
	, mGameData(gameData)
	, mRule(rule)
	, mGameDB(gameDB)
	, mRedis(redis)
	, mAuctionHouse([this](const uint64_t sessionID, const char* data, const int32_t len)
		{
			sendGame(sessionID, data, len); // 마감은 타이머 스레드에서도 - 라이브러리가 워커로 넘긴다
		}, &mAuctionTimer, gameDB, rule.AuctionDurationSec, lastAuctionID)
{
	Assert(gameData != nullptr && gameDB != nullptr && redis != nullptr, "GameServer: gameData·gameDb·redis가 null");
	mTurnTimer.Start([this](const uint64_t partyID, const uint32_t turnNo)
		{
			onTurnExpired(partyID, turnNo);
		});
	mAuctionTimer.Start([this](const uint64_t auctionID, const uint32_t)
		{
			mAuctionHouse.Close(auctionID); // 대기열 락을 놓은 뒤 불린다 - 락 순서 경매장 → 대기열
		});
	if (rule.EntryLimitSec > 0)
	{
		mEntryWatch.Start(1000, [this]()
			{
				checkEntryDeadline();
			});
	}
}

void GameServer::checkEntryDeadline()
{
	// 접속만 하고 입장하지 않는 연결이 세션 칸(maxSession)을 채워 새 사용자를 막지 못하게(운영자 서버의 인증 기한과 같은 방식)
	// 칸마다 송신 락으로 잠깐 보고, 끊기는 락 밖에서 - 이미 끊겼거나 칸이 다른 세션으로 바뀐 ID면 Disconnect가 아무 일도 안 한다
	const ULONGLONG limitMs = static_cast<ULONGLONG>(mRule.EntryLimitSec) * 1000;
	std::vector<uint64_t> expired;
	for (Player& player : mPlayers)
	{
		const uint64_t sessionID = player.GetEntryExpiredSessionID(limitMs);
		if (sessionID != 0)
		{
			expired.push_back(sessionID);
		}
	}
	for (const uint64_t sessionID : expired)
	{
		Disconnect(sessionID);
		++mEntryTimeoutCnt; // 그사이 스스로 끊긴 세션도 셀 수 있다 - 통계용
	}
}

GameServer::~GameServer()
{
	Shutdown(); // 파생 소멸자에서 멈춘다 - 기반 소멸자 시점엔 파생 부분이 없어 OnRelease가 순수 가상 호출이 된다
}

void GameServer::Shutdown()
{
	mEntryWatch.Stop(); // 워커 밖 스레드는 라이브러리 Stop 전에 멈춘다
	mTurnTimer.Stop(); // 워커 밖 스레드는 라이브러리 Stop 전에 멈춘다 - Stop 중·뒤에 SendPacket·파티를 만지지 않게
	mAuctionTimer.Stop();
	mbClosing = true; // 이 뒤로 OnRecv는 패킷을 버린다
	while (mRecvInProgressCnt != 0)
	{
		Sleep(1); // 이미 처리 중이던 패킷이 끝나기를 - 그 패킷이 넣는 저장 요청까지 DB 스레드가 받은 뒤에 멈춘다
	}
	mGameDB->GetThread().Stop(); // 남은 저장을 모두 끝낸다. 열린 경매는 다음 시작 때 취소·환불된다(GameDB::Prepare)
	Stop();            // 두 번 불러도 된다(라이브러리 Stop은 이미 멈췄으면 그냥 돌아온다)
}

void GameServer::OnAccept(const uint64_t id, const SOCKADDR_IN& remoteAddr)
{
	Player& player = getPlayer(id);
	Assert(player.GetSessionID() == 0, "OnAccept: 이전 세션의 Player가 정리되지 않음");
	player.Init(id, remoteAddr);
	EnterChallenge challenge{}; // 이 연결의 무작위 값 - 클라가 입장 증명에 넣는다(가로챈 LoginReq를 다른 연결에서 다시 쓰지 못하게)
	challenge.Type = eGameType::EnterChallenge;
	player.GetChallenge(challenge.Nonce);
	sendGame(id, reinterpret_cast<const char*>(&challenge), sizeof(challenge));
}

void GameServer::sendGame(const uint64_t id, const char* data, const int32_t len)
{
	// 게임 서버의 모든 송신이 여기를 지난다 - 입장 뒤에는 세션 키 통로로 암호화(설계 login_server.docx 5-2)
	// 한 세션에 워커(방송)·타이머·DB 스레드가 동시에 보낼 수 있어, 순번 매기기 + 암호화 + 송신 링에 넣기를 칸의 송신 락 안에서 한 번에 한다
	// (락 순서: 게임 락 → 송신 락 → 라이브러리 세션 락. 라이브러리는 자기 락을 쥔 채 게임 코드를 부르지 않는다)
	Player& player = getPlayer(id);
	std::string payload;
	std::lock_guard<std::mutex> lock(player.GetSendLock());
	if (player.PrepareSendLocked(id, data, len, &payload))
	{
		SendPacket(id, payload.data(), static_cast<int32_t>(payload.size()));
	}
}

void GameServer::OnRecv(const uint64_t id, const char* payload, const int32_t len)
{
	// 처리 중 수를 먼저 올리고 종료 표시를 본다 - Shutdown은 표시를 세운 뒤 이 수가 0이 되기를 기다리므로, 둘 중 하나는 반드시 상대를 본다
	++mRecvInProgressCnt;
	if (!mbClosing)
	{
		Player& player = getPlayer(id);
		if (player.IsSecure())
		{
			// 입장 뒤에는 암호문만 - 풀리지 않으면(변조·순번 어긋남·평문) 끊는다. 같은 세션의 OnRecv는 동시에 불리지 않아 받는 순번은 락이 없다
			std::string plain;
			if (player.OpenReceived(payload, len, &plain))
			{
				onRecvPacket(id, plain.data(), static_cast<int32_t>(plain.size()));
			}
			else
			{
				disconnectBadPacket(id);
			}
		}
		else
		{
			eGameType type = eGameType::LoginRes; // 못 읽으면 입장 요청이 아닌 것으로
			if (len >= static_cast<int32_t>(sizeof(type)))
			{
				MemCpy(&type, payload, sizeof(type));
			}
			if (type == eGameType::LoginReq)
			{
				onRecvPacket(id, payload, len); // 입장 전에 평문으로 받는 것은 입장 요청뿐
			}
			else
			{
				disconnectBadPacket(id); // 입장 전의 다른 패킷 - 평문 명령으로 암호 통로를 건너뛰지 못하게
			}
		}
	}
	--mRecvInProgressCnt;
}

void GameServer::onRecvPacket(const uint64_t id, const char* payload, const int32_t len)
{
	if (len < static_cast<int32_t>(sizeof(eGameType)))
	{
		disconnectBadPacket(id); // 종류도 못 읽는 패킷
		return;
	}
	eGameType type;
	MemCpy(&type, payload, sizeof(type)); // payload는 라이브러리가 넘긴 바이트 배열 안의 주소라 정렬이 맞는다는 보장이 없다

	switch (type)
	{
	case eGameType::LoginReq:
		onLoginReq(id, payload, len);
		break;
	case eGameType::InventoryReq:
		onInventoryReq(id, len);
		break;
	case eGameType::PartyCreateReq:
		onPartyCreateReq(id, len);
		break;
	case eGameType::PartyListReq:
		onPartyListReq(id, len);
		break;
	case eGameType::PartyJoinReq:
		onPartyJoinReq(id, payload, len);
		break;
	case eGameType::PartyLeaveReq:
		onPartyLeaveReq(id, len);
		break;
	case eGameType::PartyStartReq:
		onPartyStartReq(id, len);
		break;
	case eGameType::DungeonActionReq:
		onDungeonActionReq(id, payload, len);
		break;
	case eGameType::EquipReq:
		onEquipReq(id, payload, len, true);
		break;
	case eGameType::UnequipReq:
		onEquipReq(id, payload, len, false);
		break;
	case eGameType::AuctionRegisterReq:
		onAuctionRegisterReq(id, payload, len);
		break;
	case eGameType::AuctionListReq:
		onAuctionListReq(id, payload, len);
		break;
	case eGameType::AuctionBidReq:
		onAuctionBidReq(id, payload, len);
		break;
	case eGameType::AuctionWatchReq:
		onAuctionWatchReq(id, payload, len);
		break;
	default:
		disconnectBadPacket(id); // 모르는 종류, 또는 서버 → 클라 종류
		break;
	}
}

void GameServer::OnRelease(const uint64_t id)
{
	Player& player = getPlayer(id);
	Assert(player.GetSessionID() == id, "OnRelease: 다른 세션의 Player");
	leaveParty(player, false); // 떠나는 사람에게는 보내지 않는다 - 남은 파티원에게만 알린다
	mAuctionHouse.RemoveSession(id); // 경매는 남는다 - 계정은 지워지지 않아 마감 때 그 계정에 넣는다
	const Account* account = player.GetAccount();
	if (account != nullptr)
	{
		mAccountTable.Logout(account->GetID(), id); // 접속 표시만 지운다 - 계정 데이터는 남아 다시 로그인하면 이어진다
	}
	player.Clear();
}

void GameServer::onLoginReq(const uint64_t id, const char* payload, const int32_t len)
{
	Player& player = getPlayer(id);
	// 로그인 중 표시를 계정보다 먼저 읽는다 - DB 스레드는 계정을 넣은 뒤 표시를 내리므로, 표시가 꺼진 것을 봤다면 계정도 보인다
	// (거꾸로 읽으면 "계정 없음"을 본 뒤 DB 스레드가 로그인을 끝내 "로그인 중 아님"까지 보여, 한 세션이 두 계정에 로그인된다)
	if (len != static_cast<int32_t>(sizeof(LoginReq)) || player.IsLoginPending() || player.GetAccount() != nullptr)
	{
		disconnectBadPacket(id); // 길이가 틀리거나, 이미 로그인했거나 로그인 중인 세션이 다시 로그인
		return;
	}
	LoginReq req;
	MemCpy(&req, payload, sizeof(req));
	// 입장은 인증 전에 받는 패킷이라 접속을 많이 열어 보내면 DB 스레드 대기열(저장도 같이 쓴다)이 한없이 길어질 수 있다 - 많이 밀렸으면 바로 거절
	if (mGameDB->GetThread().GetPendingCnt() >= LOGIN_DB_QUEUE_MAX)
	{
		const LoginRes busy{ eGameType::LoginRes, eLoginResult::DbError };
		sendGame(id, reinterpret_cast<const char*>(&busy), sizeof(busy));
		return;
	}

	// 세션 키 확인(Redis)과 처음 보는 계정 읽기(MySQL)는 블록되므로 DB 스레드에 넘긴다(설계 3-2, login_server.docx 2장 ④)
	// DB 스레드가 결과를 보내고 Player에 계정을 넣을 때까지 이 세션이 해제되지 않게 붙잡는다(그사이 끊겨도 OnRelease는 Unhold 뒤)
	if (!HoldSession(id))
	{
		return; // 내 콜백 안이라 실패하지 않는다 - 실패하면 이미 해제 중
	}
	player.SetLoginPending(true);
	mGameDB->GetThread().Post([this, id, req](DbConn& conn)
		{
			std::string accountID;
			uint8_t sessionKey[SESSION_KEY_LEN] = {};
			LoginRes res{ eGameType::LoginRes, verifyEntryOnDbThread(id, req, &accountID, sessionKey) };
			if (res.Result == eLoginResult::Ok)
			{
				// 계정은 DB 스레드만 메모리에 올린다(로그인·운영 명령) - 이미 있으면 메모리가 기준이라 DB를 다시 읽지 않는다
				UniquePtr<Account> loaded;
				if (!mAccountTable.Has(accountID))
				{
					loaded = GameDB::LoadOrCreate(conn, accountID, mGameData->GetPotionKind());
				}
				res = (loaded != nullptr || mAccountTable.Has(accountID)) ? finishLogin(id, accountID, std::move(loaded))
					: LoginRes{ eGameType::LoginRes, eLoginResult::DbError };
			}
			// 순서: 계정 넣기(finishLogin) → 로그인 중 표시 내리기 → 응답. 응답을 먼저 보내면 실패를 받고 바로 다시 로그인한 클라가
			// 아직 켜진 표시에 걸려 잘못된 패킷으로 끊길 수 있다. 붙잡고 있어 이 칸은 아직 이 세션 것이다
			Player& player = getPlayer(id);
			const bool bTooManyFails = (res.Result == eLoginResult::AuthFail) && (player.AddEntryFail() >= ENTRY_FAIL_MAX);
			if (!bTooManyFails)
			{
				player.SetLoginPending(false); // 평소에는 표시를 내린 뒤 응답(위 순서 설명)
			}
			{
				// LoginRes는 평문으로 만들어 두고 → 통로를 먼저 연 뒤 → 보낸다. 같은 송신 락 안이라 다른 스레드의 송신이 끼지 않는다
				// (보낸 뒤에 열면, 클라가 LoginRes를 받자마자 보낸 암호문을 워커가 통로를 열기 전에 받아 평문 명령으로 보고 끊는다 - 장시간 시험에서 봇 2개)
				std::lock_guard<std::mutex> lock(player.GetSendLock());
				std::string payload;
				if (player.PrepareSendLocked(id, reinterpret_cast<const char*>(&res), sizeof(res), &payload))
				{
					if (res.Result == eLoginResult::Ok)
					{
						player.OpenChannelLocked(sessionKey);
					}
					SendPacket(id, payload.data(), static_cast<int32_t>(payload.size()));
				}
			}
			if (bTooManyFails)
			{
				// 끊을 때는 응답 → 끊기 → 표시 내리기 - 먼저 내리면 응답을 받자마자 보낸 다음 입장 요청이 끊기 전에 받아들여진다(로그인 서버와 같은 순서)
				Disconnect(id);
				++mEntryFailCutCnt;
				player.SetLoginPending(false);
			}
			SecureZeroMemory(sessionKey, sizeof(sessionKey));
			UnholdSession(id);
		});
}

eLoginResult GameServer::verifyEntryOnDbThread(const uint64_t id, const LoginReq& req, std::string* outAccountID, uint8_t (&outSessionKey)[SESSION_KEY_LEN])
{
	Assert(outAccountID != nullptr, "verifyEntryOnDbThread: outAccountID가 null");
	uint8_t challenge[ENTER_CHALLENGE_LEN];
	getPlayer(id).GetChallenge(challenge); // 붙잡고 있어 이 칸은 아직 이 세션 것이다(값은 OnAccept 뒤 바뀌지 않는다)
	const std::string key = "session:" + std::to_string(req.AccountNo);
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
	const bool bProofOk = VerifyEnterProof(sessionKey, req.AccountNo, challenge, req.Proof);
	if (bProofOk) // 틀리면 지우지 않는다 - 가짜 증명으로 남의 키를 지워 진짜 사용자를 막지 못하게
	{
		// 확인한 뒤에, 읽은 값과 같을 때만 지운다. 지운 결과가 1인 연결만 통과 - 같은 증명을 두 연결이 동시에 보내도 하나만 들어오고,
		// GET 뒤에 같은 계정이 다시 로그인해 새 키로 덮어썼으면 새 키는 남는다(옛 키의 입장만 실패)
		int64_t removedCnt = 0;
		if (!mRedis->DelIfEquals(key, value, &removedCnt))
		{
			result = eLoginResult::DbError; // 끊긴 지우기는 다시 하지 않는다(RedisClient) - 클라가 로그인 서버에서 다시
		}
		else if (removedCnt == 1)
		{
			result = eLoginResult::Ok;
			*outAccountID = value.substr(SESSION_KEY_LEN);
			MemCpy(outSessionKey, sessionKey, sizeof(outSessionKey)); // 입장 뒤 통로 키를 이것에서 파생한다
		}
	}
	SecureZeroMemory(sessionKey, sizeof(sessionKey));
	SecureZeroMemory(value.data(), value.size());
	return result;
}

LoginRes GameServer::finishLogin(const uint64_t id, const std::string& accountID, UniquePtr<Account> loaded)
{
	Account* account = nullptr;
	uint64_t onlineSessionID = 0;
	const LoginRes res{ eGameType::LoginRes, mAccountTable.Login(accountID, id, std::move(loaded), &account, &onlineSessionID) };
	if (res.Result == eLoginResult::Ok)
	{
		getPlayer(id).SetAccount(account); // 다 만든 계정만 넣는다 - 이 뒤로 그 세션의 워커가 로그인된 것으로 본다
	}
	else if (res.Result == eLoginResult::AlreadyLoggedIn)
	{
		// 이전 접속을 끊는다 - 클라 전원·랜선이 끊겨 FIN·RST가 오지 않으면 서버는 그 세션이 살아 있다고 보고, 그 아이디로 다시는 못 들어온다
		// 이전 세션의 OnRelease가 접속 표시를 지운 뒤 새 접속이 다시 로그인하면 성공한다. 끊긴 ID면 Disconnect는 아무 일도 안 한다
		Disconnect(onlineSessionID);
	}
	return res; // 보내는 것은 부르는 쪽 - DB 스레드는 로그인 중 표시를 내린 뒤에 보낸다
}

void GameServer::onInventoryReq(const uint64_t id, const int32_t len)
{
	Account* account = getPlayer(id).GetAccount();
	if (len != static_cast<int32_t>(sizeof(InventoryReq)) || account == nullptr)
	{
		disconnectBadPacket(id); // 길이가 틀리거나 로그인 전
		return;
	}
	sendInventory(id, *account);
}

void GameServer::sendInventory(const uint64_t id, Account& account)
{
	InventoryRes res{};
	res.Type = eGameType::InventoryRes;
	{
		std::lock_guard<std::mutex> lock(account.GetLock()); // 낙찰·회수가 다른 스레드에서 같은 계정을 바꿀 수 있다
		account.FillInventory(&res);
	}
	sendGame(id, reinterpret_cast<const char*>(&res), sizeof(res));
}

void GameServer::onEquipReq(const uint64_t id, const char* payload, const int32_t len, const bool bEquip)
{
	if (!requireLogin(id))
	{
		return;
	}
	if (len != static_cast<int32_t>(bEquip ? sizeof(EquipReq) : sizeof(UnequipReq)))
	{
		disconnectBadPacket(id);
		return;
	}
	Player& player = getPlayer(id);
	Account* account = player.GetAccount();
	EquipRes res{};
	res.Type = eGameType::EquipRes;

	// 던전 확인과 착용을 같은 파티 락 안에서 - 확인한 뒤 락을 놓으면 그사이 방장이 던전을 시작해, 옛 장비로 복사된 뒤 착용이 성공할 수 있다
	// 파티에 들어가고 나오는 일은 내 콜백이라 지금 동시에 일어나지 않는다. 파티가 없으면 플레이어 락만
	const std::shared_ptr<Party> party = findMyParty(player);
	std::unique_lock<std::mutex> partyLock;
	if (party != nullptr)
	{
		partyLock = std::unique_lock<std::mutex>(party->GetLock());
	}
	if (party != nullptr && !party->IsClosed() && party->GetState() == ePartyState::InDungeon)
	{
		res.Result = eEquipResult::InDungeon; // 던전 상태에 복사한 능력치와 어긋나지 않게(설계 3-9-1)
	}
	else
	{
		std::lock_guard<std::mutex> playerLock(account->GetLock()); // 파티 → 플레이어(가장 안쪽)
		// DB 저장은 플레이어 락 안에서 넣는다 - 같은 계정의 다른 변경과 DB에 들어가는 순서가 메모리와 같게(설계 3-3)
		if (bEquip)
		{
			EquipReq req;
			MemCpy(&req, payload, sizeof(req));
			eEquipSlot slot = eEquipSlot::Count;
			res.Result = account->Equip(req.ItemUID, *mGameData, &slot);
			if (res.Result == eEquipResult::Ok)
			{
				mGameDB->SaveEquip(account->GetID(), slot, req.ItemUID);
			}
		}
		else
		{
			UnequipReq req;
			MemCpy(&req, payload, sizeof(req));
			res.Result = account->Unequip(req.Slot);
			if (res.Result == eEquipResult::Ok)
			{
				mGameDB->SaveEquip(account->GetID(), req.Slot, 0);
			}
		}
	}
	if (partyLock.owns_lock())
	{
		partyLock.unlock();
	}
	sendGame(id, reinterpret_cast<const char*>(&res), sizeof(res));
	if (res.Result == eEquipResult::Ok)
	{
		sendInventory(id, *account);
	}
}

void GameServer::onAuctionRegisterReq(const uint64_t id, const char* payload, const int32_t len)
{
	if (!requireLogin(id))
	{
		return;
	}
	if (len != static_cast<int32_t>(sizeof(AuctionRegisterReq)))
	{
		disconnectBadPacket(id);
		return;
	}
	AuctionRegisterReq req;
	MemCpy(&req, payload, sizeof(req));
	Player& player = getPlayer(id);
	// 던전 중에는 등록하지 않는다(설계 3-10-5). 파티 락과 경매장 락을 함께 잡지 않으려고 먼저 확인하고 놓는다
	// 놓은 뒤 던전이 시작돼도 꺼내는 것은 착용하지 않은 아이템이라 던전 능력치와 어긋나지 않는다
	if (isInDungeon(player))
	{
		AuctionRegisterRes res{};
		res.Type = eGameType::AuctionRegisterRes;
		res.Result = eAuctionRegisterResult::InDungeon;
		sendGame(id, reinterpret_cast<const char*>(&res), sizeof(res));
		return;
	}
	Account* account = player.GetAccount();
	mAuctionHouse.Register(id, account, account->GetID(), req.ItemUID, req.StartPrice);
}

void GameServer::onAuctionListReq(const uint64_t id, const char* payload, const int32_t len)
{
	if (!requireLogin(id))
	{
		return;
	}
	if (len != static_cast<int32_t>(sizeof(AuctionListReq)))
	{
		disconnectBadPacket(id);
		return;
	}
	AuctionListReq req;
	MemCpy(&req, payload, sizeof(req));
	AuctionListRes res{};
	res.Type = eGameType::AuctionListRes;
	mAuctionHouse.FillList(req.ItemKind, req.AfterAuctionID, &res);
	sendGame(id, reinterpret_cast<const char*>(&res), sizeof(res));
}

void GameServer::onAuctionBidReq(const uint64_t id, const char* payload, const int32_t len)
{
	if (!requireLogin(id))
	{
		return;
	}
	if (len != static_cast<int32_t>(sizeof(AuctionBidReq)))
	{
		disconnectBadPacket(id);
		return;
	}
	AuctionBidReq req;
	MemCpy(&req, payload, sizeof(req));
	Account* account = getPlayer(id).GetAccount();
	mAuctionHouse.Bid(id, account, account->GetID(), req.AuctionID, req.Price);
}

void GameServer::onAuctionWatchReq(const uint64_t id, const char* payload, const int32_t len)
{
	if (!requireLogin(id))
	{
		return;
	}
	if (len != static_cast<int32_t>(sizeof(AuctionWatchReq)))
	{
		disconnectBadPacket(id);
		return;
	}
	AuctionWatchReq req;
	MemCpy(&req, payload, sizeof(req));
	mAuctionHouse.SetWatch(id, req.bWatch != 0);
}

bool GameServer::isInDungeon(Player& player)
{
	const std::shared_ptr<Party> party = findMyParty(player);
	if (party == nullptr)
	{
		return false;
	}
	std::lock_guard<std::mutex> lock(party->GetLock());
	return !party->IsClosed() && party->GetState() == ePartyState::InDungeon;
}

void GameServer::onPartyCreateReq(const uint64_t id, const int32_t len)
{
	if (!requireLogin(id))
	{
		return;
	}
	if (len != static_cast<int32_t>(sizeof(PartyCreateReq)))
	{
		disconnectBadPacket(id);
		return;
	}
	Player& player = getPlayer(id);
	PartyCreateRes res{};
	res.Type = eGameType::PartyCreateRes;
	if (findMyParty(player) != nullptr)
	{
		res.Result = eCreatePartyResult::AlreadyInParty;
		sendGame(id, reinterpret_cast<const char*>(&res), sizeof(res));
		return;
	}

	// 방장(나)을 넣은 파티가 표에 올라간다 - 아이디는 만든 뒤 바뀌지 않아 플레이어 락 없이 읽는다
	const std::shared_ptr<Party> party = mPartyTable.Create(id, player.GetAccount()->GetID(), player.GetAccount());
	player.SetPartyID(party->GetID());
	std::lock_guard<std::mutex> lock(party->GetLock()); // 그사이 다른 사람이 들어왔을 수 있다 - 지금 상태를 파티 락 안에서 보낸다
	res.Result = eCreatePartyResult::Ok;
	res.PartyID = party->GetID();
	sendGame(id, reinterpret_cast<const char*>(&res), sizeof(res));
	broadcastPartyInfo(*party);
}

void GameServer::onPartyListReq(const uint64_t id, const int32_t len)
{
	if (!requireLogin(id))
	{
		return;
	}
	if (len != static_cast<int32_t>(sizeof(PartyListReq)))
	{
		disconnectBadPacket(id);
		return;
	}
	PartyListRes res{};
	res.Type = eGameType::PartyListRes;
	mPartyTable.FillList(&res);
	sendGame(id, reinterpret_cast<const char*>(&res), sizeof(res));
}

void GameServer::onPartyJoinReq(const uint64_t id, const char* payload, const int32_t len)
{
	if (!requireLogin(id))
	{
		return;
	}
	if (len != static_cast<int32_t>(sizeof(PartyJoinReq)))
	{
		disconnectBadPacket(id);
		return;
	}
	PartyJoinReq req;
	MemCpy(&req, payload, sizeof(req));
	Player& player = getPlayer(id);
	PartyJoinRes res{};
	res.Type = eGameType::PartyJoinRes;
	if (findMyParty(player) != nullptr)
	{
		res.Result = eJoinPartyResult::AlreadyInParty;
		sendGame(id, reinterpret_cast<const char*>(&res), sizeof(res));
		return;
	}

	const std::shared_ptr<Party> party = mPartyTable.Find(req.PartyID);
	if (party == nullptr)
	{
		res.Result = eJoinPartyResult::NotFound;
		sendGame(id, reinterpret_cast<const char*>(&res), sizeof(res));
		return;
	}
	std::lock_guard<std::mutex> lock(party->GetLock());
	if (party->IsClosed())
	{
		res.Result = eJoinPartyResult::NotFound; // 찾은 뒤 락을 잡기 전에 마지막 사람이 나가 지워짐
	}
	else if (party->GetState() != ePartyState::Recruiting)
	{
		res.Result = eJoinPartyResult::AlreadyStarted;
	}
	else if (party->IsFull())
	{
		res.Result = eJoinPartyResult::Full;
	}
	else
	{
		party->AddMember(id, player.GetAccount()->GetID(), player.GetAccount()); // 계정 주소는 내 워커가 내 Player에서 읽는다
		player.SetPartyID(party->GetID());
		res.Result = eJoinPartyResult::Ok;
	}
	sendGame(id, reinterpret_cast<const char*>(&res), sizeof(res));
	if (res.Result == eJoinPartyResult::Ok)
	{
		broadcastPartyInfo(*party); // 결과를 먼저, 그다음 상태 - 같은 파티 락 안이라 다른 참가자와 섞이지 않는다
	}
}

void GameServer::onPartyLeaveReq(const uint64_t id, const int32_t len)
{
	if (!requireLogin(id))
	{
		return;
	}
	if (len != static_cast<int32_t>(sizeof(PartyLeaveReq)))
	{
		disconnectBadPacket(id);
		return;
	}
	leaveParty(getPlayer(id), true); // 파티에 없으면 무시 - 끊지 않는다(클라가 상태를 늦게 알 수 있다)
}

void GameServer::onPartyStartReq(const uint64_t id, const int32_t len)
{
	if (!requireLogin(id))
	{
		return;
	}
	if (len != static_cast<int32_t>(sizeof(PartyStartReq)))
	{
		disconnectBadPacket(id);
		return;
	}
	Player& player = getPlayer(id);
	PartyStartRes res{};
	res.Type = eGameType::PartyStartRes;
	const std::shared_ptr<Party> party = findMyParty(player);
	if (party == nullptr)
	{
		res.Result = eStartPartyResult::NotInParty;
		sendGame(id, reinterpret_cast<const char*>(&res), sizeof(res));
		return;
	}

	std::lock_guard<std::mutex> lock(party->GetLock());
	if (party->IsClosed() || !party->HasMember(id))
	{
		res.Result = eStartPartyResult::NotInParty; // findMyParty 뒤 다시 락을 잡기 전에 바뀜
	}
	else if (!party->IsLeader(id))
	{
		res.Result = eStartPartyResult::NotLeader;
	}
	else if (party->GetState() != ePartyState::Recruiting)
	{
		res.Result = eStartPartyResult::AlreadyStarted;
	}
	else if (party->GetMemberCnt() < PARTY_START_MIN)
	{
		res.Result = eStartPartyResult::NotEnoughMembers;
	}
	else
	{
		party->StartDungeon(createDungeon(*party));
		res.Result = eStartPartyResult::Ok;
	}
	sendGame(id, reinterpret_cast<const char*>(&res), sizeof(res));
	if (res.Result == eStartPartyResult::Ok)
	{
		broadcastPartyInfo(*party); // 던전 진행 중
		std::vector<DungeonEventNotify> events;
		party->GetDungeon()->Begin(&events); // 첫 방에 들어간다
		broadcastDungeon(*party, events);
	}
}

void GameServer::onDungeonActionReq(const uint64_t id, const char* payload, const int32_t len)
{
	if (!requireLogin(id))
	{
		return;
	}
	if (len != static_cast<int32_t>(sizeof(DungeonActionReq)))
	{
		disconnectBadPacket(id);
		return;
	}
	DungeonActionReq req;
	MemCpy(&req, payload, sizeof(req));
	const std::shared_ptr<Party> party = findMyParty(getPlayer(id));
	if (party == nullptr)
	{
		return; // 파티가 없다 - 늦게 온 요청일 수 있어 끊지 않고 무시
	}

	std::lock_guard<std::mutex> lock(party->GetLock());
	Dungeon* dungeon = party->GetDungeon();
	const int32_t memberIdx = party->FindMemberIdx(id);
	if (party->IsClosed() || dungeon == nullptr || memberIdx < 0)
	{
		return; // 던전이 끝났거나 그사이 파티에서 빠졌다 - 무시
	}
	std::vector<DungeonEventNotify> events;
	const eActResult result = dungeon->Act(memberIdx, req.TurnNo, req.Action, req.Target, &events);
	if (result == eActResult::Ignored)
	{
		return; // 내 차례가 아니거나 이미 넘어간 턴
	}
	if (result == eActResult::Rejected)
	{
		for (const DungeonEventNotify& event : events)
		{
			sendGame(id, reinterpret_cast<const char*>(&event), sizeof(event)); // 할 수 없는 행동 - 나에게만, 차례는 그대로
		}
		return;
	}
	broadcastDungeon(*party, events);
}

UniquePtr<Dungeon> GameServer::createDungeon(const Party& party)
{
	std::vector<DungeonMember> members;
	for (const PartyMember& partyMember : party.GetMembers())
	{
		EquipBonus bonus;
		{
			std::lock_guard<std::mutex> playerLock(partyMember.MemberAccount->GetLock()); // 파티 락 → 플레이어 락(가장 안쪽, 설계 3-9-1)
			bonus = partyMember.MemberAccount->GetEquipBonus(*mGameData);
		}
		members.push_back(DungeonMember{ partyMember.MemberAccount, mRule.PlayerHp, mRule.PlayerHp, mRule.PlayerAttack + bonus.Attack,
			mRule.PlayerDefense + bonus.Defense, mRule.PlayerInvestigate + bonus.Investigate, bonus.WeaponDamage, false, false });
	}
	return MakeUnique<Dungeon>(mGameData, mGameDB, std::move(members), party.GetLastTurnNo());
}

void GameServer::broadcastDungeon(Party& party, const std::vector<DungeonEventNotify>& events)
{
	for (const DungeonEventNotify& event : events)
	{
		for (const PartyMember& member : party.GetMembers())
		{
			sendGame(member.SessionID, reinterpret_cast<const char*>(&event), sizeof(event));
		}
	}
	Dungeon* dungeon = party.GetDungeon();
	if (dungeon->IsOver())
	{
		finishDungeon(party);
		return;
	}
	DungeonTurnNotify turn;
	dungeon->FillTurn(&turn);
	// 새 차례의 마감 - 파티 락 → 대기열 락. 차례가 그대로인 알림(차례가 아닌 파티원이 빠짐)이면 원래 마감을 그대로 두고 남은 시간을 싣는다
	const TurnTimer::Clock::time_point now = TurnTimer::Clock::now();
	if (party.MarkTurnScheduled(turn.TurnNo, now + std::chrono::milliseconds(mRule.TurnTimeMs)))
	{
		mTurnTimer.Add(party.GetTurnDeadline(), party.GetID(), turn.TurnNo);
	}
	const long long remainMs = std::chrono::duration_cast<std::chrono::milliseconds>(party.GetTurnDeadline() - now).count();
	turn.TimeLimitMs = static_cast<uint16_t>((remainMs > 0) ? remainMs : 0);
	for (const PartyMember& member : party.GetMembers())
	{
		sendGame(member.SessionID, reinterpret_cast<const char*>(&turn), sizeof(turn));
	}
}

void GameServer::onTurnExpired(const uint64_t partyID, const uint32_t turnNo)
{
	const std::shared_ptr<Party> party = mPartyTable.Find(partyID); // 파티 표 락으로 찾기만 - 놓은 뒤 파티 락
	if (party == nullptr)
	{
		return; // 파티가 이미 없어졌다
	}
	std::lock_guard<std::mutex> lock(party->GetLock());
	Dungeon* dungeon = party->GetDungeon();
	if (party->IsClosed() || dungeon == nullptr)
	{
		return; // 던전 중 마지막 사람이 나가 닫혔거나 던전이 끝났다
	}
	std::vector<DungeonEventNotify> events;
	if (dungeon->AutoDefend(turnNo, &events))
	{
		broadcastDungeon(*party, events); // 타이머 스레드에서 SendPacket - 실제 송신은 라이브러리가 워커로 넘긴다
	}
}

void GameServer::finishDungeon(Party& party)
{
	Dungeon* dungeon = party.GetDungeon();
	const bool bSuccess = dungeon->IsSuccess();
	const std::vector<PartyMember>& members = party.GetMembers();
	const std::vector<std::vector<uint16_t>> loot = bSuccess ? dungeon->RollLoot() : std::vector<std::vector<uint16_t>>(members.size());
	Assert(loot.size() == members.size(), "finishDungeon: 던전 파티원 수와 파티원 수가 다름 - 빠질 때 같은 번호로 함께 빼야 한다");

	for (size_t i = 0; i < members.size(); ++i)
	{
		DungeonEndNotify end{};
		end.Type = eGameType::DungeonEndNotify;
		end.bSuccess = bSuccess ? 1 : 0;
		if (bSuccess)
		{
			Account* account = members[i].MemberAccount;
			std::lock_guard<std::mutex> playerLock(account->GetLock()); // 파티 락 → 플레이어 락(가장 안쪽, 설계 3-9-1)
			account->AddGold(mRule.DungeonGold);
			end.Gold = mRule.DungeonGold;
			std::vector<ItemInfo> added;
			for (const uint16_t kind : loot[i])
			{
				if (account->IsInventoryFull())
				{
					++end.DiscardedCnt; // 인벤토리가 가득 차 버린다(설계 3-9-6)
					continue;
				}
				const ItemInfo item{ NewItemUID(), kind };
				account->AddItem(item);
				added.push_back(item);
				if (end.ItemCnt < DUNGEON_REWARD_MAX)
				{
					end.Items[end.ItemCnt++] = item;
				}
			}
			mGameDB->SaveDungeonReward(account->GetID(), mRule.DungeonGold, added); // 골드와 전리품을 트랜잭션 하나로, 플레이어 락 안에서
		}
		sendGame(members[i].SessionID, reinterpret_cast<const char*>(&end), sizeof(end));
	}
	party.EndDungeon();
	broadcastPartyInfo(party); // 모집 중으로 돌아감 - 다시 시작할 수 있다
}

std::shared_ptr<Party> GameServer::findMyParty(Player& player)
{
	const uint64_t partyID = player.GetPartyID();
	if (partyID == 0)
	{
		return nullptr;
	}
	std::shared_ptr<Party> party = mPartyTable.Find(partyID);
	if (party != nullptr)
	{
		std::lock_guard<std::mutex> lock(party->GetLock());
		if (!party->IsClosed() && party->HasMember(player.GetSessionID()))
		{
			return party;
		}
	}
	player.SetPartyID(0); // 파티가 없어졌거나 내가 빠져 있다 - 들고 있던 번호가 틀린 값이었다
	return nullptr;
}

void GameServer::leaveParty(Player& player, const bool bNotifyMe)
{
	const uint64_t partyID = player.GetPartyID();
	if (partyID == 0)
	{
		return;
	}
	player.SetPartyID(0);
	const std::shared_ptr<Party> party = mPartyTable.Find(partyID);
	if (party == nullptr)
	{
		return; // 이미 없어진 파티
	}

	const uint64_t id = player.GetSessionID();
	bool bClosed = false;
	{
		std::lock_guard<std::mutex> lock(party->GetLock());
		if (party->IsClosed() || !party->HasMember(id))
		{
			return; // 들고 있던 번호가 틀린 값이었다 - 무시
		}
		const int32_t removedIdx = party->RemoveMember(id);
		std::vector<DungeonEventNotify> events;
		Dungeon* dungeon = party->GetDungeon();
		if (dungeon != nullptr)
		{
			dungeon->RemoveMember(removedIdx, &events); // 같은 번호로 - 차례였으면 다음 사람에게, 아무도 안 남으면 실패(설계 3-9-4)
		}
		if (bNotifyMe)
		{
			PartyInfoNotify notify{};
			notify.Type = eGameType::PartyInfoNotify;
			notify.State = ePartyState::None;
			sendGame(id, reinterpret_cast<const char*>(&notify), sizeof(notify));
		}
		if (party->GetMemberCnt() == 0)
		{
			party->Close();
			bClosed = true;
		}
		else
		{
			broadcastPartyInfo(*party); // 방장이 나갔으면 다음 사람이 방장인 상태로
			if (dungeon != nullptr)
			{
				broadcastDungeon(*party, events); // 남은 사람에게 바뀐 차례(또는 던전 끝)
			}
		}
	}
	if (bClosed)
	{
		mPartyTable.Remove(partyID); // 파티 락을 푼 뒤 파티 표 락 - 두 락을 함께 잡지 않는다
	}
}

void GameServer::broadcastPartyInfo(const Party& party)
{
	PartyInfoNotify notify;
	party.FillInfo(&notify);
	for (const PartyMember& member : party.GetMembers())
	{
		sendGame(member.SessionID, reinterpret_cast<const char*>(&notify), sizeof(notify)); // 끊긴 세션이면 실패하고 넘어간다
	}
}

bool GameServer::requireLogin(const uint64_t id)
{
	if (getPlayer(id).GetAccount() == nullptr)
	{
		disconnectBadPacket(id); // 로그인 전에는 로그인 말고 보낼 수 있는 패킷이 없다
		return false;
	}
	return true;
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

void GameServer::AdminRecall(const uint64_t itemUID, const std::string& note, const std::function<void(const AdminRecallRes&)>& reply)
{
	const bool bAccepted = postExternal([this, itemUID, note, reply](DbConn& conn)
		{
			AdminRecallRes res{};
			res.Type = eAdminType::RecallRes;
			recallOnDbThread(conn, itemUID, note, &res);
			reply(res);
		});
	if (!bAccepted)
	{
		AdminRecallRes res{};
		res.Type = eAdminType::RecallRes;
		res.Result = eRecallResult::DbError; // 서버 종료 중
		reply(res);
	}
}

void GameServer::recallOnDbThread(DbConn& conn, const uint64_t itemUID, const std::string& note, AdminRecallRes* outRes)
{
	Assert(outRes != nullptr, "recallOnDbThread: outRes가 null");
	ItemInfo item{};
	std::string ownerID;
	// ① 경매 중 - 경매를 취소하고 입찰자에게 환불(경매장 락, 저장 포함)
	if (mAuctionHouse.CancelByItem(itemUID, note, &item, &ownerID))
	{
		outRes->Result = eRecallResult::Ok;
		outRes->From = eRecallFrom::Auction;
	}
	// ② 메모리에 있는 계정의 인벤토리 - 플레이어 락 안에서 꺼내고 저장(설계 3-4: 회수와 플레이어의 행동이 같은 플레이어 락에서 한쪽씩)
	//   착용 중이면 벗긴다 - 던전 중이어도(운영 명령이라 예외). 그 던전은 시작할 때 복사한 능력치로 끝까지 간다(설계 3-9-1)
	//   응답 Ok는 메모리에 반영됐다는 뜻이다 - DB 저장은 대기열에 들어갔고, 실패하면 다른 저장과 똑같이 DB실패로 센다
	else if (mAccountTable.TakeItemFromAnyone(itemUID, [this, &note, &item, &ownerID](Account& owner, const ItemInfo& taken)
		{
			mGameDB->SaveRecall(owner.GetID(), taken, note);
			item = taken;
			ownerID = owner.GetID();
		}))
	{
		outRes->Result = eRecallResult::Ok;
		outRes->From = eRecallFrom::Inventory;
		const uint64_t onlineSessionID = mAccountTable.GetOnlineSessionID(ownerID);
		if (onlineSessionID != 0)
		{
			const RecallNotify notify{ eGameType::RecallNotify, item };
			sendGame(onlineSessionID, reinterpret_cast<const char*>(&notify), sizeof(notify)); // DB 스레드에서 - 라이브러리가 워커로 넘긴다
		}
	}
	// ③ DB에만 있는 계정 - 계정을 읽는 로그인도 이 스레드라 지금 이 계정이 메모리에 올라오지 않는다
	else
	{
		std::vector<std::vector<std::string>> rows;
		if (!conn.Query("SELECT account_id, item_kind FROM items WHERE item_uid = " + std::to_string(itemUID), &rows))
		{
			outRes->Result = eRecallResult::DbError;
		}
		else if (rows.empty())
		{
			outRes->Result = eRecallResult::NotFound;
		}
		else
		{
			ownerID = rows[0][0];
			item = ItemInfo{ itemUID, static_cast<uint16_t>(std::strtoul(rows[0][1].c_str(), nullptr, 10)) };
			if (mAccountTable.Has(ownerID))
			{
				// 메모리 계정인데 ①②에서 못 찾음 - 그사이 경매 등록·낙찰로 옮겨졌거나 회복약으로 쓰였고, 그 저장이 아직 대기열에 있다
				// (다시 시도하면 옮겨진 곳에서 찾거나, 쓰였으면 NotFound)
				outRes->Result = eRecallResult::Moved;
			}
			else
			{
				outRes->Result = DbThread::RunTransaction(conn, GameDB::MakeRecallSQL(ownerID, item, note), "회수(DB만)") ? eRecallResult::Ok : eRecallResult::DbError;
				outRes->From = eRecallFrom::OfflineAccount;
			}
		}
	}
	MemCpy(outRes->OwnerID, ownerID.data(), (ownerID.size() < ACCOUNT_ID_LEN) ? ownerID.size() : ACCOUNT_ID_LEN - 1);
	outRes->Item = item;
}

void GameServer::AdminBan(const std::string& accountID, const int32_t minutes, const std::string& note, const std::function<void(const AdminBanRes&)>& reply)
{
	const bool bAccepted = postExternal([this, accountID, minutes, note, reply](DbConn& conn)
		{
			AdminBanRes res{};
			res.Type = eAdminType::BanRes;
			banOnDbThread(conn, accountID, minutes, note, &res);
			reply(res);
		});
	if (!bAccepted)
	{
		AdminBanRes res{};
		res.Type = eAdminType::BanRes;
		res.Result = eBanResult::DbError; // 서버 종료 중
		reply(res);
	}
}

void GameServer::banOnDbThread(DbConn& conn, const std::string& accountID, const int32_t minutes, const std::string& note, AdminBanRes* outRes)
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
		// 메모리 계정 - 로그인은 메모리의 차단 값을 보므로 메모리를 먼저, 저장은 같은 플레이어 락 안에서
		std::lock_guard<std::mutex> playerLock(account->GetLock());
		account->SetBanUntil(banUntil);
		mGameDB->SaveBan(accountID, banUntil, note);
		outRes->Result = eBanResult::Ok;
	}
	else
	{
		// DB에만 있는 계정 - 이 스레드에서 바로 써야 뒤에 줄 선 그 계정의 로그인이 차단 값을 읽는다
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
			outRes->Result = DbThread::RunTransaction(conn, GameDB::MakeBanSQL(accountID, banUntil, note), "제재(DB만)") ? eBanResult::Ok : eBanResult::DbError;
		}
	}
	if (outRes->Result == eBanResult::Ok && banUntil > 0)
	{
		const uint64_t onlineSessionID = mAccountTable.GetOnlineSessionID(accountID);
		if (onlineSessionID != 0)
		{
			Disconnect(onlineSessionID); // 강제 종료 - 다시 로그인하면 Banned
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

int64_t GameServer::GetPartyCnt() const
{
	return mPartyTable.GetCnt();
}

int64_t GameServer::GetAuctionCnt() const
{
	return mAuctionHouse.GetCnt();
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
		<< " 로그인=" << server.GetOnlineCnt()        // 지금 로그인한 아이디
		<< " 계정=" << server.GetAccountCnt()         // 메모리에 올린 계정(내리지 않아 계속 는다 - 메모리 증가를 볼 때 함께 본다)
		<< " 파티=" << server.GetPartyCnt()
		<< " 경매=" << server.GetAuctionCnt()
		<< " DB대기=" << server.GetDbPendingCnt()     // DB 스레드가 아직 처리하지 않은 요청
		<< " DB실패=" << server.GetDbFailCnt()        // 되돌린 트랜잭션(누적)
		<< " 입장기한끊김=" << server.GetEntryTimeoutCnt() // 접속한 뒤 입장 기한 안에 입장하지 않아 끊은 수(누적)
		<< " 입장실패끊김=" << server.GetEntryFailCutCnt(); // 한 연결에서 입장 확인을 ENTRY_FAIL_MAX번 틀려 끊은 수(누적)
	return out;
}
