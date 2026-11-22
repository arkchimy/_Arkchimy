#pragma once
#include <WinSock2.h>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include "MonitorProtocol.h"
#include "NetStat.h"

struct MonitorAgentConfig
{
	std::string Host;
	int32_t Port = 0; // 0이면 끔
	std::string Key;  // db.cfg의 monitorKey - 출력하지 않는다
	std::string Name;
	eServerKind Kind = eServerKind::Game;
};

// 로그인·게임 서버가 모니터링 서버에 1초 표본을 보내는 쪽(설계 4-1) - 자기 스레드 하나
// 1초 통계 스레드는 Post로 최신 표본을 넘기고 바로 돌아간다. 모니터가 없거나 느려도 서버는 막히지 않는다:
// 연결은 비블록(1초 기한), 보내기도 비블록이라 다 못 보내면 연결을 버리고 5초 뒤 다시 붙는다
class MonitorAgent final
{
public:
	~MonitorAgent();

	// server.cfg의 monitorHost·monitorPort, db.cfg의 monitorKey. 포트가 0이거나 키가 없으면 끈 채로 true
	// 밖에서 오는 값이라 형식이 틀리면 알리고 false
	static bool LoadConfig(const char* serverCfgPath, const char* dbCfgPath, MonitorAgentConfig* outConfig);
	// 프로세스·PC 칸(RAW_PROC_CPU_100NS – RAW_SYS_AVAIL_MB)
	static void FillProcessValues(MonSample* outSample);
	// 라이브러리 칸(RAW_SESSION – RAW_BAD_HEADER)
	static void FillNetValues(const NetStat& stat, const int64_t sessionCnt, MonSample* outSample);

	void Start(const MonitorAgentConfig& config); // 꺼진 설정이면 아무것도 하지 않는다
	void Stop();                                  // Bye를 보내고 끊는다. 두 번 불러도 된다
	void Post(const MonSample& sample);           // 최신 표본 하나만 둔다 - 아직 못 보낸 것은 덮어쓴다

private:
	void run();
	bool connectOnce();
	bool sendAll(const char* data, const int32_t len); // 비블록 - 다 못 보내면 false
	template <typename T>
	bool sendPacket(const T& body);
	void closeSocket();

	MonitorAgentConfig mConfig;
	SOCKET mSock = INVALID_SOCKET; // 에이전트 스레드만 쓴다
	ULONGLONG mLastConnectTick = 0;
	bool mbEverConnected = false;

	std::mutex mLock; // 아래 셋 - 1초 통계 스레드와 에이전트 스레드
	std::condition_variable mWake;
	MonSample mPending{};
	bool mbHasPending = false;
	bool mbStopping = false;
	std::thread mThread;
};
