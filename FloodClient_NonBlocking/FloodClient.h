#pragma once
#include <WinSock2.h>
#include <ws2tcpip.h>
#include <string>
#include <thread>
#include <vector>
#include <atomic>
#pragma comment(lib, "ws2_32.lib")

// 논블로킹 connect() 후 handshake 완료 전에 바로 close - SYN flood 재현용 클라이언트
// ACK를 보내지 않으므로 서버 backlog의 half-open 슬롯을 점유시키는 효과를 냄
class FloodClient
{
public:
	~FloodClient();
	bool init(const std::string& ip, USHORT port, int threadCnt, int targetRate);	// 타겟 주소 설정 + 워커 스레드 시작. targetRate: 초당 목표 시도 수 (0이면 제한 없음)
	void stop();	// 종료 신호 + join

private:
	void workerLoop();	// { 논블로킹 connect() → 바로 close() } 반복
	void statLoop();	// 1초마다 시도 횟수 콘솔 출력

	sockaddr_in mTargetAddr{};
	int mThreadCnt = 0;
	int mTargetRate = 0;	// 전체 스레드 합산 초당 목표 시도 수 (0이면 제한 없음)
	std::atomic<int> mReadyCount = 0;
	std::vector<std::thread> mWorkers;
	std::thread mStatThread;
	std::atomic<bool> mRunning = false;
	std::atomic<long long> mAttemptCount = 0;	// connect 시도 총횟수 (ConnectTPS 산출용). 수립/거절 여부는 클라가 판단하지 않음
};
