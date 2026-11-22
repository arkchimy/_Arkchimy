#pragma once
#include <WinSock2.h>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "MonitorProtocol.h"

// 화면에 그릴 서버 하나
struct ServerView
{
	std::string Name;
	eServerKind Kind = eServerKind::Game;
	eServerState State = eServerState::Online;
	std::deque<MonRateRow> Live; // 최근 LIVE_KEEP_SEC초(시각 순)
};

struct EventView
{
	std::string Name;
	int64_t UnixMs = 0;
	std::string Text; // UTF-8
};

// 지난 기록 요청 하나 - 모니터가 한 번에 최대 128행씩 주므로 다 올 때까지 이어서 요청한다
struct HistoryView
{
	std::string Name;
	bool bMinute = false;
	int64_t FromSec = 0;
	int64_t ToSec = 0;
	bool bLoading = false;
	bool bDbError = false; // 모니터의 DB 조회가 실패해 메모리에 있던 최근 행만 왔다
	std::vector<MonRateRow> Rows;
};

// 모니터링 서버와의 연결 - 받는 스레드 하나가 상태를 고치고, 화면(메인 스레드)은 Snapshot으로 복사해 그린다
// 끊기면 2초마다 다시 붙는다
class MonitorLink final
{
public:
	enum : int32_t
	{
		LIVE_KEEP_SEC = 600, // 실시간 그래프 10분
		EVENT_KEEP = 200,
		RETRY_MS = 2000,      // 끊긴 뒤 다시 연결
		KEY_RETRY_MS = 10000, // 키가 틀렸을 때 - 모니터 콘솔에 거절이 너무 자주 쌓이지 않게
		CONNECT_WAIT_STEPS = 15, // 연결 기다리기 200ms × 15 = 3초(사이마다 Stop 확인)
	};

	struct Snapshot
	{
		std::string Status;
		bool bConnected = false;
		std::vector<ServerView> Servers;
		std::vector<EventView> Events; // 최근 것이 뒤
		HistoryView History;
		uint64_t HistoryVersion = UINT64_MAX; // History를 복사한 판 - 같으면 TakeSnapshot이 다시 복사하지 않는다(화면이 Snapshot을 계속 들고 있는다)
	};

	~MonitorLink();

	void Start(const std::string& host, const int32_t port, const std::string& key);
	void Stop();
	void TakeSnapshot(Snapshot* outSnapshot);
	void RequestHistory(const std::string& name, const int64_t fromSec, const int64_t toSec, const bool bMinute);

private:
	void run();
	bool connectAndHello();
	bool recvAll(char* buf, const int32_t len);
	bool sendHistoryReqLocked(); // mLock 안에서 - mHistory의 다음 구간
	void onPacket(const char* payload, const int32_t len);
	ServerView& getServerLocked(const std::string& name);
	void closeSocket();

	std::string mHost;
	int32_t mPort = 0;
	std::string mKey;
	SOCKET mSock = INVALID_SOCKET;
	bool mbStopping = false;
	std::thread mThread;

	std::mutex mLock; // 아래 - 받는 스레드와 화면 스레드
	std::mutex mSendLock;
	std::string mStatus = "연결 전";
	bool mbConnected = false;
	std::vector<ServerView> mServers;
	std::deque<EventView> mEvents;
	HistoryView mHistory;
	uint32_t mHistoryReqID = 0;
	int64_t mHistoryNextFrom = 0;
	uint64_t mHistoryVersion = 0; // mHistory가 바뀔 때마다 1씩
	int32_t mRetryMs = RETRY_MS;  // 이번 실패 뒤 기다릴 시간
};
