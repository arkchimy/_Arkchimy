#include "GameServer.h"

GameServer::GameServer(const int32_t maxSession)
	: mPlayers(maxSession)
{
}

GameServer::~GameServer()
{
	Stop(); // 파생 소멸자에서 멈춘다 - 기반 소멸자 시점엔 파생 부분이 없어 OnRelease가 순수 가상 호출이 된다
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
	default:
		disconnectBadPacket(id); // 모르는 종류, 또는 서버 → 클라 종류
		break;
	}
}

void GameServer::OnRelease(const uint64_t id)
{
	Player& player = getPlayer(id);
	Assert(player.GetSessionID() == id, "OnRelease: 다른 세션의 Player");
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
		res.Result = mAccountTable.Login(std::string(req.AccountID, idLen), id, &account, &onlineSessionID);
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
	InventoryRes res{};
	res.Type = eGameType::InventoryRes;
	{
		std::lock_guard<std::mutex> lock(account->GetLock()); // 낙찰·회수가 다른 스레드에서 같은 계정을 바꿀 수 있다
		account->FillInventory(&res);
	}
	SendPacket(id, reinterpret_cast<const char*>(&res), sizeof(res));
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
