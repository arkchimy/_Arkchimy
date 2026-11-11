#include "EchoServer.h"

EchoServer::EchoServer(const int32_t maxSession)
	: mPlayers(maxSession)
{
}

EchoServer::~EchoServer()
{
	Stop(); // 파생 소멸자에서 멈춘다 - 기반 소멸자 시점엔 파생 부분이 없어 OnRelease가 순수 가상 호출이 된다
}

void EchoServer::OnAccept(const uint64_t id, const SOCKADDR_IN& remoteAddr)
{
	Player& player = getPlayer(id);
	Assert(player.GetSessionID() == 0, "OnAccept: 이전 세션의 Player가 정리되지 않음");
	player.Init(id, remoteAddr);
}

void EchoServer::OnRecv(const uint64_t id, const char* payload, const int32_t len)
{
	if (len < static_cast<int32_t>(sizeof(eEchoType)))
	{
		disconnectBadPacket(id); // 종류도 못 읽는 패킷
		return;
	}
	eEchoType type;
	MemCpy(&type, payload, sizeof(type)); // payload는 라이브러리가 넘긴 바이트 배열 안의 주소라 정렬이 맞는다는 보장이 없다

	switch (type)
	{
	case eEchoType::EchoReq:
		onEchoReq(id, payload, len);
		break;
	default:
		disconnectBadPacket(id); // 모르는 종류
		break;
	}
}

void EchoServer::OnRelease(const uint64_t id)
{
	Player& player = getPlayer(id);
	Assert(player.GetSessionID() == id, "OnRelease: 다른 세션의 Player");
	player.Clear();
}

Player& EchoServer::getPlayer(const uint64_t id)
{
	const uint32_t index = static_cast<uint32_t>(id); // 라이브러리의 세션 ID = (시퀀스 번호 << 32) | 인덱스
	Assert(index < mPlayers.size(), "getPlayer: 인덱스 범위 밖 - 생성자의 maxSession과 Start의 MaxSession이 다름");
	return mPlayers[index];
}

void EchoServer::onEchoReq(const uint64_t id, const char* payload, const int32_t len)
{
	if (len != static_cast<int32_t>(sizeof(EchoReq)))
	{
		disconnectBadPacket(id); // 종류에 맞지 않는 길이
		return;
	}
	EchoReq req;
	MemCpy(&req, payload, sizeof(req));

	EchoRes res;
	res.Type = eEchoType::EchoRes;
	res.Value = req.Value;
	SendPacket(id, reinterpret_cast<const char*>(&res), sizeof(res));
}

void EchoServer::disconnectBadPacket(const uint64_t id)
{
	++mBadPacketCnt;
	Disconnect(id);
}

std::ostream& operator<<(std::ostream& out, const EchoServer& server)
{
	out << "[stat] 세션=" << server.GetSessionCnt()
		<< " 수락=" << server.GetAcceptCnt()
		<< " 해제=" << server.GetReleaseCnt()
		<< " 비정상헤더=" << server.GetStat().BadPacketCnt // 라이브러리가 헤더 길이로 끊은 수
		<< " 잘못된패킷=" << server.GetBadPacketCnt();     // 컨텐츠가 종류·길이로 끊은 수
	return out;
}
