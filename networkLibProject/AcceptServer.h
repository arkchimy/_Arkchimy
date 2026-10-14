#pragma once
#include <WinSock2.h>
#include <thread>
#include <atomic>

// 블로킹 accept() 전담 스레드로 연결을 받는 실험용 서버
// 세션/Recv/Send 없음 - accept 처리량(TPS) 측정 전용
class AcceptServer
{
public:
	~AcceptServer();
	bool init(USHORT port, int backlog);	// 리슨 소켓 생성 + accept 스레드 시작
	void stop();	// accept 스레드 종료 + join

private:
	void acceptLoop();	// accept() 반복 → 받으면 바로 close
	void statLoop();	// 1초마다 TPS 콘솔 출력

	SOCKET mListenSock = INVALID_SOCKET;
	std::thread mAcceptThread;
	std::thread mStatThread;
	std::atomic<bool> mRunning = false;
	std::atomic<long long> mAcceptCount = 0;
};
