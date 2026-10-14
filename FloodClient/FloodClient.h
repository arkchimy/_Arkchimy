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
	bool init(const std::string& ip, USHORT port, int threadCnt);	// 타겟 주소 설정 + 워커 스레드 시작
	void stop();	// 종료 신호 + join

private:
	void workerLoop();	// { 논블로킹 connect() → 바로 close() } 반복
	void statLoop();	// 1초마다 시도 횟수 콘솔 출력

	sockaddr_in mTargetAddr{};
	int mThreadCnt = 0;
	std::atomic<int> mReadyCount = 0;
	std::vector<std::thread> mWorkers;
	std::thread mStatThread;
	std::atomic<bool> mRunning = false;
	std::atomic<long long> mAttemptCount = 0;
};
