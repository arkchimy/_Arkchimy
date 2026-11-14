#include <WinSock2.h>
#include <WS2tcpip.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#include "AdminServer.h"
#include "Assert.h"
#include "GameServer.h"

enum : int32_t
{
	ADMIN_AUTH_LIMIT_MS = 5000, // 접속 후 이 안에 운영자 키가 와야 한다
};

// 고정 길이 칸 → 문자열(널이 없어도 칸 밖을 읽지 않게)
template <size_t N>
static std::string toText(const char (&text)[N])
{
	return std::string(text, strnlen(text, N));
}

AdminServer::AdminServer(GameServer* game, const std::string& adminKey, const int32_t maxSession)
	: mGame(game)
	, mAdminKey(adminKey)
	, mSlots(static_cast<size_t>(maxSession), Slot{ 0, 0, false })
{
	Assert(game != nullptr && !adminKey.empty(), "AdminServer: game이 null이거나 운영자 키가 비었음");
}

AdminServer::~AdminServer()
{
	Shutdown(); // 파생 소멸자에서 - 기반 소멸자 시점엔 OnRelease가 순수 가상 호출이 된다
}

bool AdminServer::StartAdmin(const NetConfig& config)
{
	Assert(config.MaxSession <= static_cast<int>(mSlots.size()), "StartAdmin: MaxSession이 생성자의 maxSession보다 큼");
	if (!Start(config))
	{
		return false;
	}
	mWatchThread = std::thread(&AdminServer::runAuthWatch, this);
	return true;
}

void AdminServer::Shutdown()
{
	{
		std::lock_guard<std::mutex> lock(mLock);
		mbWatchStopping = true;
	}
	mWatchWake.notify_one();
	if (mWatchThread.joinable())
	{
		mWatchThread.join(); // 워커 밖 스레드는 라이브러리 Stop 전에 멈춘다
	}
	Stop();
}

void AdminServer::OnAccept(const uint64_t id, const SOCKADDR_IN& remoteAddr)
{
	char ip[32] = {};
	inet_ntop(AF_INET, &remoteAddr.sin_addr, ip, sizeof(ip));
	printf("[admin] 접속 %s:%u\n", ip, ntohs(remoteAddr.sin_port));
	std::lock_guard<std::mutex> lock(mLock);
	getSlot(id) = Slot{ id, GetTickCount64(), false };
}

void AdminServer::OnRelease(const uint64_t id)
{
	std::lock_guard<std::mutex> lock(mLock);
	getSlot(id) = Slot{ 0, 0, false };
}

void AdminServer::OnRecv(const uint64_t id, const char* payload, const int32_t len)
{
	if (len < static_cast<int32_t>(sizeof(eAdminType)))
	{
		Disconnect(id);
		return;
	}
	eAdminType type;
	MemCpy(&type, payload, sizeof(type));
	if (type != eAdminType::LoginReq && !isAuthed(id))
	{
		Disconnect(id); // 키 확인 전 명령
		return;
	}
	switch (type)
	{
	case eAdminType::LoginReq:
		onLoginReq(id, payload, len);
		break;
	case eAdminType::RecallReq:
		onRecallReq(id, payload, len);
		break;
	case eAdminType::BanReq:
		onBanReq(id, payload, len);
		break;
	default:
		Disconnect(id); // 모르는 종류
		break;
	}
}

void AdminServer::onLoginReq(const uint64_t id, const char* payload, const int32_t len)
{
	if (len != static_cast<int32_t>(sizeof(AdminLoginReq)) || isAuthed(id))
	{
		Disconnect(id);
		return;
	}
	AdminLoginReq req;
	MemCpy(&req, payload, sizeof(req));
	const bool bOk = (toText(req.Key) == mAdminKey);
	const AdminLoginRes res{ eAdminType::LoginRes, static_cast<uint8_t>(bOk ? 1 : 0) };
	SendPacket(id, reinterpret_cast<const char*>(&res), sizeof(res));
	if (!bOk)
	{
		printf("[admin] 운영자 키 틀림 - 끊음\n");
		Disconnect(id); // 결과가 나가지 않을 수 있다(SendPacket 직후 Disconnect) - 운영 툴은 끊김으로도 실패를 안다
		return;
	}
	std::lock_guard<std::mutex> lock(mLock);
	getSlot(id).bAuthed = true;
}

void AdminServer::onRecallReq(const uint64_t id, const char* payload, const int32_t len)
{
	if (len != static_cast<int32_t>(sizeof(AdminRecallReq)))
	{
		Disconnect(id);
		return;
	}
	AdminRecallReq req;
	MemCpy(&req, payload, sizeof(req));
	printf("[admin] 회수 요청 아이템 #%llu\n", static_cast<unsigned long long>(req.ItemUID));
	mGame->AdminRecall(req.ItemUID, toText(req.Note), [this, id](const AdminRecallRes& res)
		{
			SendPacket(id, reinterpret_cast<const char*>(&res), sizeof(res)); // DB 스레드에서 - 운영 툴이 그사이 끊겼으면 버려진다
		});
}

void AdminServer::onBanReq(const uint64_t id, const char* payload, const int32_t len)
{
	if (len != static_cast<int32_t>(sizeof(AdminBanReq)))
	{
		Disconnect(id);
		return;
	}
	AdminBanReq req;
	MemCpy(&req, payload, sizeof(req));
	const std::string accountID = toText(req.AccountID);
	printf("[admin] 제재 요청 %s %d분\n", accountID.c_str(), req.Minutes);
	mGame->AdminBan(accountID, req.Minutes, toText(req.Note), [this, id](const AdminBanRes& res)
		{
			SendPacket(id, reinterpret_cast<const char*>(&res), sizeof(res));
		});
}

void AdminServer::runAuthWatch()
{
	std::unique_lock<std::mutex> lock(mLock);
	while (!mWatchWake.wait_for(lock, std::chrono::seconds(1), [this]
		{
			return mbWatchStopping;
		}))
	{
		std::vector<uint64_t> expired;
		const ULONGLONG now = GetTickCount64();
		for (const Slot& slot : mSlots)
		{
			if (slot.SessionID != 0 && !slot.bAuthed && now - slot.AcceptTick > ADMIN_AUTH_LIMIT_MS)
			{
				expired.push_back(slot.SessionID);
			}
		}
		lock.unlock(); // Disconnect는 락 밖에서 - 끊긴 세션의 OnRelease가 이 락을 잡는다
		for (const uint64_t sessionID : expired)
		{
			printf("[admin] 인증 기한 초과 - 끊음\n");
			Disconnect(sessionID); // 이미 끊겼거나 칸이 다른 세션으로 바뀐 ID면 아무 일도 안 한다
		}
		lock.lock();
	}
}

bool AdminServer::isAuthed(const uint64_t id)
{
	std::lock_guard<std::mutex> lock(mLock);
	return getSlot(id).bAuthed;
}

AdminServer::Slot& AdminServer::getSlot(const uint64_t id)
{
	const uint32_t index = static_cast<uint32_t>(id); // 세션 ID의 하위 32비트가 인덱스
	Assert(index < mSlots.size(), "slotOf: 인덱스 범위 밖 - 생성자의 maxSession과 Start의 MaxSession이 다름");
	return mSlots[index];
}
