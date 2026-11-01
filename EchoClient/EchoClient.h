#pragma once
#include <WinSock2.h>
#include <ws2tcpip.h>
#include "PacketHeader.h"
#include <string>
#include <thread>
#include <vector>
#include <atomic>
#include <cstdint>
#include <memory>
#include <random>
#include <unordered_map>
#pragma comment(lib, "ws2_32.lib")

struct EchoClientConfig
{
	std::string ip;
	USHORT port = 0;
	int threadCnt = 1;
	int64_t bytesPerConn = 0;	// 연결 1개가 주고받을 총량 - 도달하면 끊고 재연결
	bool noRecv = false;	// 받지 않는 모드 - 서버 송신 링 넘침 테스트
	bool badPacket = false;	// 비정상 패킷 모드 - 연결마다 잘못된 헤더를 보내고 서버가 끊는지 확인
	int64_t maxInflight = 16384 - 1024;	// 돌아오지 않은 에코 바이트 한도 (최대 패킷 크기 – 15360)
};

// 스레드마다 소켓 64개로 { 논블로킹 connect → 결정적 패킷 스트림 송신 + 수신 검증 → 목표량 도달 시 RST close → 재연결 } 반복
// 패킷 길이와 페이로드가 시드로 정해진다 - 보낸 데이터를 저장하지 않고 유실/중복/순서 바뀜/변조를 검출한다
// 스트림을 무작위 크기로 잘라 보내므로 서버에는 헤더가 잘리거나 여러 패킷이 붙은 채로 도착한다
class EchoClient
{
public:
	~EchoClient();
	bool init(const EchoClientConfig& cfg);	// 타겟 주소 설정 + 워커 스레드 시작
	void stop();	// 종료 신호 + join

private:
	static constexpr int SESSION_PER_THREAD = 64;	// FD_SETSIZE 기본값
	// 돌아오지 않은 에코 바이트 한도 - 서버 넘침 한도(아직 WSASend에 안 넘긴 몫 16384)에서 여유 1KB를 뺀 값
	// 이 이하로 유지하면 서버가 이 연결을 넘침으로 끊지 않는다
	static constexpr int64_t MAX_INFLIGHT = 16384 - 1024;
	static constexpr int MAX_SEND_SIZE = 4096;	// send 1회 최대 크기 (1~이 값 사이 무작위)

	enum class SessionState
	{
		Disconnect,	// 소켓 생성 완료 - connect 요청 전
		ConnectWait,	// connect 요청 완료 - select 반응 대기
		Connected	// 송수신 중
	};

	// 연결 하나의 스트림 = [헤더][페이로드][헤더][페이로드]... 를 1바이트씩 만든다
	// 패킷 길이(0~MAX_PAYLOAD_SIZE)는 시드로 만든 난수, 페이로드 n번째 바이트 = (시드 + n) % 251
	// 송신 쪽과 검증 쪽이 같은 시드로 하나씩 가지면 같은 스트림이 나온다
	struct PacketStream
	{
		std::mt19937 mRng;	// 패킷 길이용
		uint32_t mSeed = 0;
		int64_t mPayloadPos = 0;	// 지금까지 만든 페이로드 바이트 수
		char mHeader[PACKET_HEADER_SIZE] = {};
		int32_t mHeaderLeft = 0;	// 현재 패킷에서 아직 안 만든 헤더 바이트
		int32_t mPayloadLeft = 0;	// 현재 패킷에서 아직 안 만든 페이로드 바이트

		void init(uint32_t seed);
		char next();
		bool atBoundary() const { return mHeaderLeft == 0 && mPayloadLeft == 0; }	// 패킷 사이 - 여기서 멈추면 잘린 패킷이 없다
	};

	struct EchoSession
	{
		SessionState mState = SessionState::Disconnect;
		int64_t mSendPos = 0;	// 보낸 에코 바이트 수
		int64_t mRecvPos = 0;	// 받아서 검증한 에코 바이트 수
		PacketStream mSendStream;	// 보낼 바이트 생성
		PacketStream mRecvStream;	// 받은 바이트와 비교할 기대값 생성
		std::vector<char> mPending;	// 만들었지만 send가 일부만 보내서 남은 바이트 - 생성기는 되돌릴 수 없으므로 보관
		size_t mPendingOff = 0;	// mPending에서 다음에 보낼 위치
		bool mSendDone = false;	// 목표량을 넘긴 뒤 패킷 경계에서 생성을 멈춤
		bool mBadSent = false;	// 비정상 패킷 모드: 잘못된 헤더를 보냄
	};

	bool hasMoreToSend(const EchoSession& session) const;	// 보낼 에코 바이트가 남았나 (생성 전 + 보관 중)

	void workerLoop();	// 스레드 1개가 세션 64개를 돌림
	void createSession(std::unordered_map<SOCKET, EchoSession>& sessions, std::mt19937& rng);	// socket → linger(0) → 논블로킹 → Disconnect로 삽입
	void connectSessions(std::unordered_map<SOCKET, EchoSession>& sessions, std::vector<SOCKET>& closeSockets, std::mt19937& rng);	// Disconnect 전부 connect → ConnectWait
	void selectSessions(std::unordered_map<SOCKET, EchoSession>& sessions, std::vector<SOCKET>& closeSockets, std::mt19937& rng);	// set 구성 → select → 연결 확인/송신/수신 검증
	bool sendSome(SOCKET s, EchoSession& session, std::mt19937& rng);	// 무작위 크기로 송신, 실패 시 false
	bool sendBadHeader(SOCKET s, EchoSession& session);	// 비정상 패킷 모드: 최대를 넘는 길이의 헤더 1개 송신, 실패 시 false
	bool recvAndVerify(SOCKET s, EchoSession& session);	// 수신 + 바이트마다 생성기 기대값과 비교. 끊김이면 false (불일치는 즉시 중단)
	void floodSessions(std::unordered_map<SOCKET, EchoSession>& sessions, std::vector<SOCKET>& closeSockets, std::mt19937& rng);	// 받지 않는 모드: select 없이 연결된 소켓마다 송신
	void closeSession(std::unordered_map<SOCKET, EchoSession>& sessions, SOCKET s, std::mt19937& rng);	// erase → close(RST) → createSession
	void statLoop();	// 1초마다 에코 바이트/완료 연결/예상 밖 끊김/CPU 출력

	sockaddr_in mTargetAddr{};
	int mThreadCnt = 0;
	int64_t mBytesPerConn = 0;	// 연결 1개가 주고받을 총량 - 도달하면 끊고 재연결(세션 해제/재사용 경로 검증)
	bool mNoRecv = false;	// 받지 않는 모드 - 서버 송신 링 넘침 테스트
	bool mBadPacket = false;	// 비정상 패킷 모드 - 연결마다 잘못된 헤더를 보내고 서버가 끊는지 확인
	int64_t mMaxInflight = MAX_INFLIGHT;	// 돌아오지 않은 에코 바이트 한도
	std::atomic<int> mReadyCount = 0;
	std::vector<std::thread> mWorkers;
	std::thread mStatThread;
	std::atomic<bool> mRunning = false;
	std::atomic<int64_t> mEchoBytes = 0;	// 검증 통과한 수신 바이트 총량
	std::atomic<int64_t> mDoneCount = 0;	// 목표량을 다 주고받고 정상 종료한 연결 수
	std::atomic<int64_t> mUnexpectedClose = 0;	// 목표량 전에 서버가 끊은 수 - 0이어야 정상
	std::atomic<int64_t> mFailCount = 0;	// connect 실패
	std::atomic<int64_t> mEarlyClose = 0;	// 받지 않는 모드에서 16KB도 보내기 전에 서버가 끊은 수 - 서버 슬롯 누수 신호
	std::atomic<int64_t> mBadClosed = 0;	// 비정상 패킷 모드에서 서버가 끊은 수 - 이 모드에서는 이게 정상
};
