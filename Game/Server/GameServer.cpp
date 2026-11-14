#include "GameServer.h"
#include "ItemUID.h"

GameServer::GameServer(const int32_t maxSession, const GameData* gameData, const GameRule& rule)
	: mPlayers(maxSession)
	, mGameData(gameData)
	, mRule(rule)
	, mAuctionHouse([this](const uint64_t sessionID, const char* data, const int32_t len)
		{
			SendPacket(sessionID, data, len); // 마감은 타이머 스레드에서도 - 라이브러리가 워커로 넘긴다
		}, &mAuctionTimer, rule.AuctionDurationSec)
{
	Assert(gameData != nullptr, "GameServer: gameData가 null");
	mTurnTimer.Start([this](const uint64_t partyID, const uint32_t turnNo)
		{
			onTurnExpired(partyID, turnNo);
		});
	mAuctionTimer.Start([this](const uint64_t auctionID, const uint32_t)
		{
			mAuctionHouse.Close(auctionID); // 대기열 락을 놓은 뒤 불린다 - 락 순서 경매장 → 대기열
		});
}

GameServer::~GameServer()
{
	Shutdown(); // 파생 소멸자에서 멈춘다 - 기반 소멸자 시점엔 파생 부분이 없어 OnRelease가 순수 가상 호출이 된다
}

void GameServer::Shutdown()
{
	mTurnTimer.Stop(); // 워커 밖 스레드는 라이브러리 Stop 전에 멈춘다 - Stop 중·뒤에 SendPacket·파티를 만지지 않게
	mAuctionTimer.Stop();
	Stop();            // 두 번 불러도 된다(라이브러리 Stop은 이미 멈췄으면 그냥 돌아온다)
}

void GameServer::OnAccept(const uint64_t id, const SOCKADDR_IN& remoteAddr)
{
	Player& player = getPlayer(id);
	Assert(player.GetSessionID() == 0, "OnAccept: 이전 세션의 Player가 정리되지 않음");
	player.Init(id, remoteAddr);
}

void GameServer::OnRecv(const uint64_t id, const char* payload, const int32_t len)
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
	if (len != static_cast<int32_t>(sizeof(LoginReq)) || player.GetAccount() != nullptr)
	{
		disconnectBadPacket(id); // 길이가 틀리거나, 이미 로그인한 세션이 다시 로그인
		return;
	}
	LoginReq req;
	MemCpy(&req, payload, sizeof(req));

	LoginRes res;
	res.Type = eGameType::LoginRes;
	const size_t idLen = strnlen(req.AccountID, ACCOUNT_ID_LEN);
	if (idLen == 0 || idLen == ACCOUNT_ID_LEN)
	{
		res.Result = eLoginResult::BadID; // 비었거나 널로 끝나지 않음 - 밖에서 온 값이라 끊지 않고 거절만
	}
	else
	{
		Account* account = nullptr;
		uint64_t onlineSessionID = 0;
		res.Result = mAccountTable.Login(std::string(req.AccountID, idLen), id, mGameData->GetPotionKind(), &account, &onlineSessionID);
		if (res.Result == eLoginResult::Ok)
		{
			player.SetAccount(account); // 같은 세션의 콜백은 동시에 불리지 않으므로 Player는 락 없이
		}
		else if (res.Result == eLoginResult::AlreadyLoggedIn)
		{
			// 이전 접속을 끊는다 - 클라 전원·랜선이 끊겨 FIN·RST가 오지 않으면 서버는 그 세션이 살아 있다고 보고, 그 아이디로 다시는 못 들어온다
			// 이전 세션의 OnRelease가 접속 표시를 지운 뒤 새 접속이 다시 로그인하면 성공한다. 끊긴 ID면 Disconnect는 아무 일도 안 한다
			Disconnect(onlineSessionID);
		}
	}
	SendPacket(id, reinterpret_cast<const char*>(&res), sizeof(res));
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
	SendPacket(id, reinterpret_cast<const char*>(&res), sizeof(res));
}

void GameServer::onEquipReq(const uint64_t id, const char* payload, const int32_t len, const bool bEquip)
{
	if (!isLoggedIn(id))
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
		if (bEquip)
		{
			EquipReq req;
			MemCpy(&req, payload, sizeof(req));
			res.Result = account->Equip(req.ItemUID, *mGameData);
		}
		else
		{
			UnequipReq req;
			MemCpy(&req, payload, sizeof(req));
			res.Result = account->Unequip(req.Slot);
		}
	}
	if (partyLock.owns_lock())
	{
		partyLock.unlock();
	}
	SendPacket(id, reinterpret_cast<const char*>(&res), sizeof(res));
	if (res.Result == eEquipResult::Ok)
	{
		sendInventory(id, *account);
	}
}

void GameServer::onAuctionRegisterReq(const uint64_t id, const char* payload, const int32_t len)
{
	if (!isLoggedIn(id))
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
		SendPacket(id, reinterpret_cast<const char*>(&res), sizeof(res));
		return;
	}
	Account* account = player.GetAccount();
	mAuctionHouse.Register(id, account, account->GetID(), req.ItemUID, req.StartPrice);
}

void GameServer::onAuctionListReq(const uint64_t id, const char* payload, const int32_t len)
{
	if (!isLoggedIn(id))
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
	SendPacket(id, reinterpret_cast<const char*>(&res), sizeof(res));
}

void GameServer::onAuctionBidReq(const uint64_t id, const char* payload, const int32_t len)
{
	if (!isLoggedIn(id))
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
	if (!isLoggedIn(id))
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
	if (!isLoggedIn(id))
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
		SendPacket(id, reinterpret_cast<const char*>(&res), sizeof(res));
		return;
	}

	// 방장(나)을 넣은 파티가 표에 올라간다 - 아이디는 만든 뒤 바뀌지 않아 플레이어 락 없이 읽는다
	const std::shared_ptr<Party> party = mPartyTable.Create(id, player.GetAccount()->GetID(), player.GetAccount());
	player.SetPartyID(party->GetID());
	std::lock_guard<std::mutex> lock(party->GetLock()); // 그사이 다른 사람이 들어왔을 수 있다 - 지금 상태를 파티 락 안에서 보낸다
	res.Result = eCreatePartyResult::Ok;
	res.PartyID = party->GetID();
	SendPacket(id, reinterpret_cast<const char*>(&res), sizeof(res));
	broadcastPartyInfo(*party);
}

void GameServer::onPartyListReq(const uint64_t id, const int32_t len)
{
	if (!isLoggedIn(id))
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
	SendPacket(id, reinterpret_cast<const char*>(&res), sizeof(res));
}

void GameServer::onPartyJoinReq(const uint64_t id, const char* payload, const int32_t len)
{
	if (!isLoggedIn(id))
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
		SendPacket(id, reinterpret_cast<const char*>(&res), sizeof(res));
		return;
	}

	const std::shared_ptr<Party> party = mPartyTable.Find(req.PartyID);
	if (party == nullptr)
	{
		res.Result = eJoinPartyResult::NotFound;
		SendPacket(id, reinterpret_cast<const char*>(&res), sizeof(res));
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
	SendPacket(id, reinterpret_cast<const char*>(&res), sizeof(res));
	if (res.Result == eJoinPartyResult::Ok)
	{
		broadcastPartyInfo(*party); // 결과를 먼저, 그다음 상태 - 같은 파티 락 안이라 다른 참가자와 섞이지 않는다
	}
}

void GameServer::onPartyLeaveReq(const uint64_t id, const int32_t len)
{
	if (!isLoggedIn(id))
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
	if (!isLoggedIn(id))
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
		SendPacket(id, reinterpret_cast<const char*>(&res), sizeof(res));
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
	SendPacket(id, reinterpret_cast<const char*>(&res), sizeof(res));
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
	if (!isLoggedIn(id))
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
			SendPacket(id, reinterpret_cast<const char*>(&event), sizeof(event)); // 할 수 없는 행동 - 나에게만, 차례는 그대로
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
	return MakeUnique<Dungeon>(mGameData, std::move(members), party.GetLastTurnNo());
}

void GameServer::broadcastDungeon(Party& party, const std::vector<DungeonEventNotify>& events)
{
	for (const DungeonEventNotify& event : events)
	{
		for (const PartyMember& member : party.GetMembers())
		{
			SendPacket(member.SessionID, reinterpret_cast<const char*>(&event), sizeof(event));
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
	turn.TimeLimitMs = static_cast<uint16_t>(mRule.TurnTimeMs);
	for (const PartyMember& member : party.GetMembers())
	{
		SendPacket(member.SessionID, reinterpret_cast<const char*>(&turn), sizeof(turn));
	}
	// 새 차례의 마감 - 파티 락 → 대기열 락. 차례가 그대로인 알림(차례가 아닌 파티원이 빠짐)이면 원래 마감을 그대로 둔다
	if (party.MarkTurnScheduled(turn.TurnNo))
	{
		mTurnTimer.Add(TurnTimer::Clock::now() + std::chrono::milliseconds(mRule.TurnTimeMs), party.GetID(), turn.TurnNo);
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
			for (const uint16_t kind : loot[i])
			{
				if (account->IsInventoryFull())
				{
					++end.DiscardedCnt; // 인벤토리가 가득 차 버린다(설계 3-9-6)
					continue;
				}
				const ItemInfo item{ NewItemUID(), kind };
				account->AddItem(item);
				if (end.ItemCnt < DUNGEON_REWARD_MAX)
				{
					end.Items[end.ItemCnt++] = item;
				}
			}
		}
		SendPacket(members[i].SessionID, reinterpret_cast<const char*>(&end), sizeof(end));
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
			SendPacket(id, reinterpret_cast<const char*>(&notify), sizeof(notify));
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
		SendPacket(member.SessionID, reinterpret_cast<const char*>(&notify), sizeof(notify)); // 끊긴 세션이면 실패하고 넘어간다
	}
}

bool GameServer::isLoggedIn(const uint64_t id)
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
		<< " 잘못된패킷=" << server.GetBadPacketCnt(); // 컨텐츠가 종류·길이로 끊은 수
	return out;
}
