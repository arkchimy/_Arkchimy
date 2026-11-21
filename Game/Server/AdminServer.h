#pragma once
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "NetworkLib.h"
#include "AdminProtocol.h"

class GameServer;

// 운영자 포트 - 같은 라이브러리로 띄운 두 번째 서버 객체(설계 3-5). 운영 툴이 접속해 차단·해제를 명령한다
// 127.0.0.1에만 묶고(NetConfig bLoopbackOnly), 첫 패킷은 운영자 키 - 틀리거나, 로그인 전에 명령이 오거나, 접속 후 5초 안에 키가 없으면 끊는다
// (키 없이 붙어만 있는 연결이 몇 안 되는 세션 칸을 차지해 운영자가 못 들어오는 일을 막는다)
// 명령은 GameServer가 DB 스레드에서 처리하고, 결과는 DB 스레드가 이 객체의 SendPacket으로 보낸다
class AdminServer final : public NetworkLib
{
public:
	AdminServer(GameServer* game, const std::string& adminKey, const int32_t maxSession);
	~AdminServer() override;

	bool StartAdmin(const NetConfig& config); // Start + 인증 기한 감시 스레드
	void Shutdown();                          // 감시 스레드(워커 밖 스레드)를 먼저 멈추고 라이브러리 Stop. 두 번 불러도 된다

protected:
	void OnAccept(const uint64_t id, const SOCKADDR_IN& remoteAddr) override;
	void OnRecv(const uint64_t id, const char* payload, const int32_t len) override;
	void OnRelease(const uint64_t id) override;

private:
	struct Slot
	{
		uint64_t SessionID; // 0이면 빈 칸
		ULONGLONG AcceptTick;
		bool bAuthed;
	};

	Slot& getSlot(const uint64_t id); // 세션 인덱스의 칸 - mLock 안에서
	bool isAuthed(const uint64_t id);
	void onLoginReq(const uint64_t id, const char* payload, const int32_t len);
	void onBanReq(const uint64_t id, const char* payload, const int32_t len);
	void runAuthWatch(); // 1초마다 - 인증 기한이 지난 세션을 끊는다(Disconnect는 아무 스레드에서나)

	GameServer* const mGame;
	const std::string mAdminKey;
	std::mutex mLock;          // 칸 목록 - 워커(수락·로그인·해제)와 감시 스레드가 함께 본다
	std::vector<Slot> mSlots;  // 세션과 같은 인덱스
	std::condition_variable mWatchWake;
	bool mbWatchStopping = false;
	std::thread mWatchThread;
};
