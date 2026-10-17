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

	// 닫기 직전 상태 분류 (합 = mAttemptCount)
	std::atomic<long long> mConnectedCount = 0;	// 핸드셰이크 완료
	std::atomic<long long> mRefusedCount = 0;	// WSAECONNREFUSED(10061) - backlog 넘침 또는 리슨 없음
	std::atomic<long long> mOtherErrCount = 0;	// 그 외 에러
	std::atomic<long long> mPendingCount = 0;	// 아직 진행 중 (결과 모름)
	std::atomic<int> mLastOtherErr = 0;	// 마지막 기타 에러 코드 (원인 확인용)
};
