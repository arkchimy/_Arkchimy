#pragma once
#include <WinSock2.h>
#include <ws2tcpip.h>
#include "PacketHeader.h"
#include "ChatProtocol.h"
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
	int msgSize = 32;	// 헤더 포함 메시지 1개 크기 - MIN_MSG_SIZE ~ MAX_PACKET_SIZE
	int ratePerSession = 0;	// 0이면 closed-loop(묶음). 양수면 open-loop: 세션마다 초당 이만큼 응답과 상관없이 보낸다(수용량 측정용)
	int roomSize = 0;	// 0이면 에코. 양수면 채팅: 세션 번호 / roomSize 번 방에 Join하고, 자기 메시지가 다 돌아오면 다음 묶음 (서버 chatRoomCnt ≥ 방 수)
};

// 처리량 측정용 부하 클라이언트 - 메시지를 overSend개씩 묶어 send 한 번에 보내고, 묶음이 전부 에코되면 다음 묶음을 보낸다
// 정확성 검증(무작위로 잘라 보내기, 바이트 단위 비교)은 EchoClient가 맡고, 여기서는 순번만 확인해 클라 비용을 줄인다
// 응답이 와야 다음을 보내는 closed-loop라 처리량 측정용이다 - 서버가 느려지면 덜 보내므로 지연 측정에는 쓰지 않는다(탐침 사용)
// ratePerSession > 0이면 open-loop - 서버 속도와 상관없이 정해진 속도로 보내므로, 속도를 올려 가며 지연이 무너지는 지점(수용량)을 찾을 수 있다
class LoadClient
{
public:
	~LoadClient();
	bool init(const LoadClientConfig& cfg);	// 타겟 주소 설정 + 워커 스레드 시작
	void stop();	// 종료 신호 + join

	static constexpr int MIN_MSG_SIZE = PACKET_HEADER_SIZE + sizeof(uint64_t);	// 페이로드 앞 8바이트 = 순번, 나머지는 채움
	// 묶음 2개가 서버 송신 링(제어 패킷 자리 1KB 제외)에 들어가야 넘침으로 안 끊긴다
	// 클라는 에코를 다 받으면 바로 다음 묶음을 보내지만, 서버는 송신 완료를 처리해야 링에서 빼므로 이전 묶음이 아직 남아 있을 수 있다
	static constexpr int MAX_BATCH_BYTES = (16384 - 1024) / 2;
	// 채팅 - 메시지 = 헤더 + ChatMsg(+ 채움). 받는 세션 하나에 방 전원의 묶음이 몰리므로
	// 방 크기 × 묶음이 서버 넘침 한도(아직 WSASend에 안 넘긴 몫 16KB)에서 핑 자리 1KB를 뺀 값 안에 들어가야 정상 클라가 끊기지 않는다
	static constexpr int CHAT_MIN_MSG_SIZE = PACKET_HEADER_SIZE + sizeof(ChatMsg);
	static constexpr int MAX_ROOM_BATCH_BYTES = 16384 - 1024;

private:
	static constexpr int MAX_SESSION_PER_THREAD = 64;	// FD_SETSIZE 기본값
	// open-loop에서 아직 못 보낸 바이트 한도 - 넘으면 그 몫은 쌓지 않고 건너뜀으로 센다(서버가 못 받아 TCP 창이 막힌 것 = 수용량 초과 신호)
	static constexpr size_t MAX_CLIENT_BACKLOG = 64 * 1024;

	enum class SessionState
	{
		Disconnect,	// 소켓 생성 완료 - connect 요청 전
		ConnectWait,	// connect 요청 완료 - select 반응 대기
		Connected	// 송수신 중
	};

	struct LoadSession
	{
		SessionState mState = SessionState::Disconnect;
		int mSlot = 0;	// 클라 안의 세션 번호(0 ~ 세션 수-1) - 다시 연결해도 유지. 채팅에서는 방 번호 = mSlot / roomSize
		uint64_t mSendSeq = 0;	// 다음에 보낼 메시지 순번
		uint64_t mRecvSeq = 0;	// 다음에 돌아와야 할 순번 - 에코는 보낸 순서대로 와야 한다 (채팅은 자기 메시지의 순번)
		bool mJoined = false;	// 채팅: JoinOk를 받았으면 true - 그 뒤부터 Chat을 보낸다
		uint64_t mSenderKey = 0;	// 채팅: 연결마다 정하는 무작위 값 - 방 전원에게 돌아오는 메시지 중 자기 것을 알아본다
		uint64_t mRoomSeq = 0;	// 채팅: 마지막으로 받은 방 순번 (0이면 입장 후 아직 없음 - 서버는 1부터 매긴다)
		int mWaiting = 0;	// 이번 묶음에서 아직 안 돌아온 메시지 수 - 0이 되면 다음 묶음
		int64_t mStartQpc = 0;	// open-loop: 연결된 시각 - 경과 시간 × 속도만큼 보낸다
		int64_t mDue = 0;	// open-loop: 지금까지 보냈어야 할 메시지 수 (쌓은 것 + 건너뛴 것)
		std::vector<char> mSendBuf;	// 보낼 바이트 (묶음 + 퐁) - 항상 완성된 패킷만 들어 있어 퐁을 뒤에 붙여도 경계가 안 깨진다
		size_t mSendOff = 0;	// mSendBuf에서 다음에 보낼 위치
		std::vector<char> mRecvBuf;	// 받은 바이트 - 완성된 패킷을 꺼내고 잘린 나머지만 남긴다
	};

	void workerLoop();	// 스레드 1개가 세션 sessionPerThread개를 돌림
	void createSession(std::unordered_map<SOCKET, LoadSession>& sessions, int slot);	// socket → linger(0) → 논블로킹 → Disconnect로 삽입
	void connectSessions(std::unordered_map<SOCKET, LoadSession>& sessions, std::vector<SOCKET>& closeSockets);	// Disconnect 전부 connect → ConnectWait
	void selectSessions(std::unordered_map<SOCKET, LoadSession>& sessions, std::vector<SOCKET>& closeSockets);	// set 구성 → select → 연결 확인/송신/수신
	void queueBatch(LoadSession& session);	// 메시지 overSend개를 송신 버퍼에 붙인다
	void queueMessages(LoadSession& session, int64_t count);	// 메시지 count개를 송신 버퍼에 붙인다
	void queueOpenLoop(LoadSession& session, int64_t nowQpc);	// open-loop: 경과 시간 × 속도에서 모자란 만큼 붙인다
	bool sendPending(SOCKET s, LoadSession& session);	// 송신 버퍼를 보낼 수 있는 만큼 보낸다, 끊김이면 false
	bool recvMessages(SOCKET s, LoadSession& session);	// 받아서 패킷 단위로 처리 - Data는 순번 확인, Ping은 퐁. 끊김이면 false
	void queueJoin(LoadSession& session);	// 채팅: 연결 직후 Join(방 번호)을 송신 버퍼에 붙인다
	void onChatPacket(LoadSession& session, const char* payload, uint16_t len, int64_t& ownCount, int64_t& chatCount);	// 채팅: JoinOk면 첫 묶음, Chat이면 방 순번 검사 + 자기 메시지면 묶음 진행
	void closeSession(std::unordered_map<SOCKET, LoadSession>& sessions, SOCKET s);	// erase → close(RST) → createSession
	void statLoop();	// 1초마다 메시지 처리량/CPU/이상 지표 출력

	sockaddr_in mTargetAddr{};
	int mThreadCnt = 0;
	int mSessionPerThread = 0;
	int mOverSend = 0;
	int mMsgSize = 0;
	int mPayloadSize = 0;	// mMsgSize - 헤더
	int mRatePerSession = 0;	// 0이면 closed-loop
	int mRoomSize = 0;	// 0이면 에코
	int64_t mQpcFreq = 0;
	std::vector<std::thread> mWorkers;
	std::thread mStatThread;
	std::atomic<int> mReadyCount = 0;
	std::atomic<bool> mRunning = false;
	std::atomic<int64_t> mEchoMsgCount = 0;	// 순번 확인을 통과한 에코 메시지 수 (채팅은 돌아온 자기 메시지 수 = 보낸 메시지 수)
	std::atomic<int64_t> mChatRecvCount = 0;	// 채팅: 받은 Chat 수(자기 것 포함) - 보낸 수의 약 방 크기배
	std::atomic<int64_t> mJoinedCount = 0;	// 채팅: 지금 방에 들어가 있는 세션 수
	std::atomic<int64_t> mBatchCount = 0;	// 전부 돌아온 묶음 수
	std::atomic<int64_t> mConnectedCount = 0;	// 지금 연결된 세션 수
	std::atomic<int64_t> mUnexpectedClose = 0;	// 서버가 끊은 수 - 연결을 유지하는 클라이므로 0이어야 정상
	std::atomic<int64_t> mFailCount = 0;	// connect 실패
	std::atomic<int64_t> mPongCount = 0;	// 서버 핑에 답한 수
	std::atomic<int64_t> mQueuedMsgCount = 0;	// open-loop: 송신 버퍼에 쌓은 메시지 수
	std::atomic<int64_t> mSkippedMsgCount = 0;	// open-loop: 못 보낸 바이트가 한도를 넘어 건너뛴 메시지 수 - 0보다 크면 서버가 그 속도를 못 받는다
};
