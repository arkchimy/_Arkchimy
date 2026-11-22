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

	// server.cfg의 monitorHost·monitorPort·이름(nameKey - monitorGameName / monitorLoginName, 없으면 outConfig에 넣어 둔 기본 이름), db.cfg의 monitorKey
	// 밖에서 오는 값이라 형식이 틀리면 알리고 false
	static bool LoadConfig(const char* serverCfgPath, const char* dbCfgPath, const char* nameKey, MonitorAgentConfig* outConfig);
	// 프로세스·PC 칸(RAW_PROC_CPU_100NS – RAW_SYS_AVAIL_MB)
	static void FillProcessValues(MonSample* outSample);
	// 라이브러리 칸(RAW_SESSION – RAW_BAD_HEADER)
	static void FillNetValues(const NetStat& stat, const int64_t sessionCnt, MonSample* outSample);

	void Start(const MonitorAgentConfig& config); // 꺼진 설정이면 아무것도 하지 않는다
	void Stop();                                  // Bye를 보내고 끊는다. 두 번 불러도 된다
	void Post(const MonSample& sample);           // 최신 표본 하나만 둔다 - 아직 못 보낸 것은 덮어쓴다
	void PrintLog();                              // 에이전트가 모아 둔 알림 줄을 찍는다 - 1초 통계 줄을 다 찍은 뒤에(줄 중간에 끼어들지 않게)

private:
	void run();
	void addLog(const std::string& line);
	bool connectOnce();
	bool sendAll(const char* data, const int32_t len); // 비블록 - 다 못 보내면 false
	template <typename T>
	bool sendPacket(const T& body);
	void closeSocket();

	MonitorAgentConfig mConfig;
	SOCKET mSock = INVALID_SOCKET; // 에이전트 스레드만 쓴다
	ULONGLONG mNextConnectTick = 0; // 이 시각 전에는 다시 붙지 않는다 - 실패·끊김 뒤 5초
	bool mbReported = false;        // 연결 실패를 이미 알림
	std::string mSendError;         // 마지막 보내기 실패 내용

	std::mutex mLock; // 아래 - 1초 통계 스레드와 에이전트 스레드
	std::condition_variable mWake;
	MonSample mPending{};
	bool mbHasPending = false;
	std::string mLog; // PrintLog가 찍을 줄들
	bool mbStopping = false;
	std::thread mThread;
};
