#pragma once
#include <WinSock2.h>
#include <cstdint>
#include <random>
#include "LoginCrypto.h"
#include "PacketHeader.h"
#include "RsaEchoProtocol.h"

constexpr int32_t ECHO_BATCH_MAX = 16;
constexpr int32_t SEALED_PACKET_SIZE = PACKET_HEADER_SIZE + LOGIN_SEALED_LEN;     // 잠근 메시지 하나 = 2 + 512
constexpr int32_t SEND_BUF_SIZE = SEALED_PACKET_SIZE * ECHO_BATCH_MAX;            // 에코 묶음이 가장 크다
constexpr int32_t RECV_BUF_SIZE = SEALED_PACKET_SIZE * 2;                         // 패킷 하나(가장 큰 것은 ServerHello 2 + 514)를 다 모을 만큼
constexpr int32_t SELECT_GROUP_SIZE = FD_SETSIZE;                                 // Windows select는 한 번에 소켓 64개까지

static_assert(PACKET_HEADER_SIZE + static_cast<int32_t>(sizeof(ServerHello)) <= RECV_BUF_SIZE, "ServerHello가 수신 버퍼에 들어가야 한다");

enum class eSessionState
{
	Idle,       // 소켓 없음 - 다음 바퀴에 접속을 시작
	Connecting, // 논블로킹 connect를 걸고 완료를 기다림
	Sending,    // SendBuf를 보내는 중
	Receiving,  // 보낸 것의 응답(PendingResCnt개)을 받는 중
	Paused,     // 에코 묶음의 응답을 다 받고 일시정지가 풀리기를 기다림
	Done,       // 종료 요청 뒤 닫음 - 다시 접속하지 않음
};

// 접속 한 번에서 지나는 단계 - 응답이 오면 다음 단계로
enum class eSessionStep
{
	Hello,    // ClientHello를 보내고 ServerHello를 기다림
	Register, // 잠근 가입 요청 - 이 세션의 첫 접속에서만
	Login,    // 잠근 로그인 요청
	Echo,     // 잠근 에코 묶음을 반복
};

// 세션 하나 - 맡은 select 스레드만 만진다
struct ClientSession
{
	int32_t Index = 0;
	eSessionState State = eSessionState::Idle;
	eSessionStep Step = eSessionStep::Hello;
	SOCKET Sock = INVALID_SOCKET;
	LoginChannel Channel; // 접속마다 StartClient로 새 클라 임시 키 쌍

	char SendBuf[SEND_BUF_SIZE] = {};
	int32_t SendSize = 0;
	int32_t SentSize = 0;
	char RecvBuf[RECV_BUF_SIZE] = {};
	int32_t RecvSize = 0;      // 받았지만 아직 패킷으로 꺼내지 않은 바이트
	int32_t PendingResCnt = 0; // 보낸 요청 중 응답을 아직 못 받은 수

	bool Registered = false;   // 가입을 마침(Ok 또는 Exists) - 다시 접속하면 로그인만
	uint64_t NextValue = 0;    // 다음 에코 요청 값 - 접속마다 0부터(서버 응답 번호와 함께 처음부터)
	uint64_t ExpectValue = 0;  // 다음에 와야 할 에코 응답 값
	ULONGLONG CloseTick = 0;   // 이 시각이 지나면 묶음이 끝난 뒤 끊는다. 0이면 끊지 않는다
	ULONGLONG RetryTick = 0;   // 접속에 실패하면 이 시각 뒤에 다시 건다
	uint32_t ConnectCnt = 0;   // FIN/RST를 번갈아 고르는 데 쓴다
	std::mt19937 Rng;          // 수명 무작위 - 세션 번호로 시드
};
