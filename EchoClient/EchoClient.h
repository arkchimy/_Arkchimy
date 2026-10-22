#pragma once
#include <WinSock2.h>
#include <ws2tcpip.h>
#include <string>
#include <thread>
#include <vector>
#include <atomic>
#include <cstdint>
#include <random>
#include <unordered_map>
#pragma comment(lib, "ws2_32.lib")

// 스레드마다 소켓 64개로 { 논블로킹 connect → 결정적 바이트 스트림 송신 + 수신 검증 → 목표량 도달 시 RST close → 재연결 } 반복
// n번째 바이트 = (시드 + n) % 251 - 보낸 데이터를 저장하지 않고 유실/중복/순서 바뀜/변조를 검출한다
class EchoClient
{
public:
	~EchoClient();
	bool init(const std::string& ip, USHORT port, int threadCnt, int64_t bytesPerConn);	// 타겟 주소 설정 + 워커 스레드 시작
	void stop();	// 종료 신호 + join

private:
	static constexpr int SESSION_PER_THREAD = 64;	// FD_SETSIZE 기본값
	static constexpr int64_t MAX_INFLIGHT = 16384;	// 서버 송신 링 크기 - 이 이하로 유지하면 서버 링이 넘치지 않는다
	static constexpr int MAX_SEND_SIZE = 4096;	// send 1회 최대 크기 (1~이 값 사이 무작위)

	enum class SessionState
	{
		Disconnect,	// 소켓 생성 완료 - connect 요청 전
		ConnectWait,	// connect 요청 완료 - select 반응 대기
		Connected	// 송수신 중
	};

	struct EchoSession
	{
		SessionState mState = SessionState::Disconnect;
		uint32_t mSeed = 0;	// 연결마다 다른 시작값
		int64_t mSendPos = 0;	// 보낸 바이트 수 = 다음에 보낼 위치
		int64_t mRecvPos = 0;	// 받아서 검증한 바이트 수
	};

	void workerLoop();	// 스레드 1개가 세션 64개를 돌림
	void createSession(std::unordered_map<SOCKET, EchoSession>& sessions, std::mt19937& rng);	// socket → linger(0) → 논블로킹 → Disconnect로 삽입
	void connectSessions(std::unordered_map<SOCKET, EchoSession>& sessions, std::vector<SOCKET>& closeSockets, std::mt19937& rng);	// Disconnect 전부 connect → ConnectWait
	void selectSessions(std::unordered_map<SOCKET, EchoSession>& sessions, std::vector<SOCKET>& closeSockets, std::mt19937& rng);	// set 구성 → select → 연결 확인/송신/수신 검증
	bool sendSome(SOCKET s, EchoSession& session, std::mt19937& rng);	// 무작위 크기로 송신, 실패 시 false
	bool recvAndVerify(SOCKET s, EchoSession& session);	// 수신 + 바이트 검증, 끊김이면 false (불일치는 즉시 중단)
	void closeSession(std::unordered_map<SOCKET, EchoSession>& sessions, SOCKET s, std::mt19937& rng);	// erase → close(RST) → createSession
	void statLoop();	// 1초마다 에코 바이트/완료 연결/예상 밖 끊김 출력

	sockaddr_in mTargetAddr{};
	int mThreadCnt = 0;
	int64_t mBytesPerConn = 0;	// 연결 1개가 주고받을 총량 - 도달하면 끊고 재연결(세션 해제/재사용 경로 검증)
	std::atomic<int> mReadyCount = 0;
	std::vector<std::thread> mWorkers;
	std::thread mStatThread;
	std::atomic<bool> mRunning = false;
	std::atomic<int64_t> mEchoBytes = 0;	// 검증 통과한 수신 바이트 총량
	std::atomic<int64_t> mDoneCount = 0;	// 목표량을 다 주고받고 정상 종료한 연결 수
	std::atomic<int64_t> mUnexpectedClose = 0;	// 목표량 전에 서버가 끊은 수 - 0이어야 정상
	std::atomic<int64_t> mFailCount = 0;	// connect 실패
};
