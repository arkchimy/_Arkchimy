#pragma once
#include <WinSock2.h>
#include <ws2tcpip.h>
#include <string>
#include <thread>
#include <vector>
#include <atomic>
#include <cstdint>
#include <unordered_map>
#pragma comment(lib, "ws2_32.lib")

// 스레드마다 소켓 64개를 들고 { 논블로킹 connect → select로 핸드셰이크 완료 확인 → RST close } 반복
// 핸드셰이크가 끝난 뒤 닫으므로 서버 백로그에 정상 연결이 들어가고, 스레드 수를 적게 유지할 수 있음
// 속도 제한 없음 - 스레드당 64개 연결이 항상 진행 중인 closed-loop 부하
class FloodClient
{
public:
	~FloodClient();
	bool init(const std::string& ip, USHORT port, int threadCnt, int selectTimeoutMs);	// 타겟 주소 설정 + 워커 스레드 시작
	void stop();	// 종료 신호 + join

private:
	static constexpr int SESSION_PER_THREAD = 64;	// select 1회에 담을 수 있는 소켓 수(FD_SETSIZE 기본값)와 맞춤

	enum class SessionState
	{
		Disconnect,	// 소켓 생성 + linger + 논블로킹 완료 - connect 요청 전
		ConnectWait	// connect 요청 완료 - select 반응 대기
	};

	void workerLoop();	// 스레드 1개가 세션 64개를 돌림
	void createSession(std::unordered_map<SOCKET, SessionState>& sessions);	// socket → linger(0) → 논블로킹 → Disconnect로 삽입
	void connectSessions(std::unordered_map<SOCKET, SessionState>& sessions, std::vector<SOCKET>& closeSockets);	// Disconnect 전부 connect → ConnectWait
	void selectSessions(std::unordered_map<SOCKET, SessionState>& sessions, std::vector<SOCKET>& closeSockets);	// ConnectWait로 set 구성 → select → 반응 온 소켓 정리
	void closeSession(std::unordered_map<SOCKET, SessionState>& sessions, SOCKET s);	// erase → close(RST) → createSession
	void statLoop();	// 1초마다 성공/실패 횟수 콘솔 출력

	sockaddr_in mTargetAddr{};
	int mThreadCnt = 0;
	int mSelectTimeoutMs = 100;	// select 대기 시간 (0이면 즉시 반환)
	std::atomic<int> mReadyCount = 0;
	std::vector<std::thread> mWorkers;
	std::thread mStatThread;
	std::atomic<bool> mRunning = false;
	std::atomic<int64_t> mConnectCount = 0;	// 핸드셰이크 성공 총횟수 (writeSet 반응)
	std::atomic<int64_t> mFailCount = 0;	// 실패 총횟수 (exceptSet 반응 + connect 즉시 실패)
};
