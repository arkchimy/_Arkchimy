#pragma once
#include <WinSock2.h>
#include <ws2tcpip.h>
#include "PacketHeader.h"
#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#pragma comment(lib, "ws2_32.lib")

struct LoadClientConfig
{
	std::string ip;
	USHORT port = 0;
	int threadCnt = 1;
	int sessionPerThread = 64;	// select 한 번에 다루는 소켓 수 - FD_SETSIZE(64) 이하
	int overSend = 16;	// 한 묶음의 메시지 수 - 묶음이 전부 돌아와야 다음 묶음을 보낸다
};

// 처리량 측정용 부하 클라이언트 - 메시지를 overSend개씩 묶어 send 한 번에 보내고, 묶음이 전부 에코되면 다음 묶음을 보낸다
// 정확성 검증(무작위로 잘라 보내기, 바이트 단위 비교)은 EchoClient가 맡고, 여기서는 순번만 확인해 클라 비용을 줄인다
// 응답이 와야 다음을 보내는 closed-loop라 처리량 측정용이다 - 서버가 느려지면 덜 보내므로 지연 측정에는 쓰지 않는다(탐침 사용)
class LoadClient
{
public:
	~LoadClient();
	bool init(const LoadClientConfig& cfg);	// 타겟 주소 설정 + 워커 스레드 시작
	void stop();	// 종료 신호 + join

	static constexpr int MSG_SIZE = 32;	// 헤더 포함 메시지 1개 크기
	static constexpr int PAYLOAD_SIZE = MSG_SIZE - PACKET_HEADER_SIZE;	// 앞 8바이트 = 순번, 나머지는 채움
	static constexpr int MAX_OVERSEND = (16384 - 1024) / MSG_SIZE;	// 한 묶음이 서버 송신 링(제어 패킷 자리 1KB 제외)에 다 들어가야 넘침으로 안 끊긴다

private:
	static constexpr int MAX_SESSION_PER_THREAD = 64;	// FD_SETSIZE 기본값

	enum class SessionState
	{
		Disconnect,	// 소켓 생성 완료 - connect 요청 전
		ConnectWait,	// connect 요청 완료 - select 반응 대기
		Connected	// 송수신 중
	};

	struct LoadSession
	{
		SessionState mState = SessionState::Disconnect;
		uint64_t mSendSeq = 0;	// 다음에 보낼 메시지 순번
		uint64_t mRecvSeq = 0;	// 다음에 돌아와야 할 순번 - 에코는 보낸 순서대로 와야 한다
		int mWaiting = 0;	// 이번 묶음에서 아직 안 돌아온 메시지 수 - 0이 되면 다음 묶음
		std::vector<char> mSendBuf;	// 보낼 바이트 (묶음 + 퐁) - 항상 완성된 패킷만 들어 있어 퐁을 뒤에 붙여도 경계가 안 깨진다
		size_t mSendOff = 0;	// mSendBuf에서 다음에 보낼 위치
		std::vector<char> mRecvBuf;	// 받은 바이트 - 완성된 패킷을 꺼내고 잘린 나머지만 남긴다
	};

	void workerLoop();	// 스레드 1개가 세션 sessionPerThread개를 돌림
	void createSession(std::unordered_map<SOCKET, LoadSession>& sessions);	// socket → linger(0) → 논블로킹 → Disconnect로 삽입
	void connectSessions(std::unordered_map<SOCKET, LoadSession>& sessions, std::vector<SOCKET>& closeSockets);	// Disconnect 전부 connect → ConnectWait
	void selectSessions(std::unordered_map<SOCKET, LoadSession>& sessions, std::vector<SOCKET>& closeSockets);	// set 구성 → select → 연결 확인/송신/수신
	void queueBatch(LoadSession& session);	// 메시지 overSend개를 송신 버퍼에 붙인다
	bool sendPending(SOCKET s, LoadSession& session);	// 송신 버퍼를 보낼 수 있는 만큼 보낸다, 끊김이면 false
	bool recvMessages(SOCKET s, LoadSession& session);	// 받아서 패킷 단위로 처리 - Data는 순번 확인, Ping은 퐁. 끊김이면 false
	void closeSession(std::unordered_map<SOCKET, LoadSession>& sessions, SOCKET s);	// erase → close(RST) → createSession
	void statLoop();	// 1초마다 메시지 처리량/CPU/이상 지표 출력

	sockaddr_in mTargetAddr{};
	int mThreadCnt = 0;
	int mSessionPerThread = 0;
	int mOverSend = 0;
	std::vector<std::thread> mWorkers;
	std::thread mStatThread;
	std::atomic<int> mReadyCount = 0;
	std::atomic<bool> mRunning = false;
	std::atomic<int64_t> mEchoMsgCount = 0;	// 순번 확인을 통과한 에코 메시지 수
	std::atomic<int64_t> mBatchCount = 0;	// 전부 돌아온 묶음 수
	std::atomic<int64_t> mConnectedCount = 0;	// 지금 연결된 세션 수
	std::atomic<int64_t> mUnexpectedClose = 0;	// 서버가 끊은 수 - 연결을 유지하는 클라이므로 0이어야 정상
	std::atomic<int64_t> mFailCount = 0;	// connect 실패
	std::atomic<int64_t> mPongCount = 0;	// 서버 핑에 답한 수
};
