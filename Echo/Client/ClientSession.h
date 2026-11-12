#pragma once
#include <WinSock2.h>
#include <cstdint>
#include "EchoProtocol.h"
#include "PacketHeader.h"

constexpr int32_t MSG_SIZE = PACKET_HEADER_SIZE + static_cast<int32_t>(sizeof(EchoReq)); // 요청·응답 하나 = 헤더 2 + 종류 2 + 값 8 = 12바이트
constexpr int32_t BATCH_MSG_CNT = 100;                                                  // 묶음 하나에 담는 요청 수 - 응답을 모두 받아야 다음 묶음
constexpr int32_t BATCH_BUF_SIZE = MSG_SIZE * BATCH_MSG_CNT;                            // 1200바이트
constexpr int32_t SEND_CHUNK_SIZE = 11;                                                 // send 한 번에 보내는 바이트 - 12와 공약수가 1이라 잘리는 위치가 매번 한 칸씩 밀린다
constexpr int32_t SELECT_GROUP_SIZE = FD_SETSIZE;                                       // Windows select는 한 번에 소켓 64개까지
constexpr int STALL_RCVBUF_SIZE = 1024;                                                 // 송신 정체 세션의 수신 버퍼 - 수신 윈도우를 줄여 응답이 빨리 막히게(루프백은 이보다 크게 잡힌다)
constexpr ULONGLONG STALL_CHECK_MS = 500;                                               // 송신 정체 세션이 요청 100개를 보내 끊겼는지 확인하는 주기 - 응답 2.4KB/s라 송신 링(32KB)이 차기 전에 정체 판정(3초)이 먼저 온다

static_assert(sizeof(EchoReq) == sizeof(EchoRes), "요청과 응답의 크기가 같아야 기대 응답을 같은 위치에 만들 수 있다");

enum class eSessionRole
{
	Normal,    // 묶음을 보내고 응답을 모두 받아 비교
	BadHeader, // 헤더 길이를 범위 밖으로 보내고 서버가 끊기를 기다림
	SendStall, // 요청을 보내고 응답을 받지 않음 - 서버가 송신 정체로 끊기를 기다림
};

enum class eSessionState
{
	Idle,       // 소켓 없음 - 다음 바퀴에 접속을 시작
	Connecting, // 논블로킹 connect를 걸고 완료를 기다림
	Sending,    // 묶음을 SEND_CHUNK_SIZE씩 보내는 중
	Receiving,  // 보낸 묶음의 응답을 받는 중
	WaitClose,  // (BadHeader) 서버가 끊기를 기다림
	StallWait,  // (SendStall) 받지 않고 기다리며 주기적으로 send로 끊김을 확인
	Paused,     // (Normal) 묶음의 응답을 다 받고 일시정지가 풀리기를 기다림 - 서버가 더 보내거나 끊으면 잡도록 읽기 알림은 받는다
	Done,      // 종료 요청 뒤 닫음 - 다시 접속하지 않음
};

// 세션 하나 - select 스레드만 만진다
struct ClientSession
{
	eSessionRole Role = eSessionRole::Normal;
	eSessionState State = eSessionState::Idle;
	SOCKET Sock = INVALID_SOCKET;

	// 보낼 묶음과 기대하는 응답 - 같은 순서로 만든다(종류만 EchoReq / EchoRes로 다름)
	char SendBuf[BATCH_BUF_SIZE] = {};
	char ExpectBuf[BATCH_BUF_SIZE] = {};
	int32_t BatchSize = 0; // 이번 묶음 바이트 수
	int32_t SentSize = 0;  // 보낸 바이트
	int32_t RecvSize = 0;  // 받아서 비교한 바이트

	uint64_t NextValue = 0;    // 다음 요청에 넣을 값 - 세션마다 (세션 번호 << 32)에서 시작해 1씩 늘린다
	ULONGLONG CloseTick = 0;   // 이 시각이 지나면 진행 중인 묶음의 응답을 다 받은 뒤 끊는다. 0이면 끊지 않는다
	ULONGLONG RetryTick = 0;   // 접속에 실패하면 이 시각 뒤에 다시 건다 - 서버가 없을 때 접속을 계속 반복하지 않게
	uint32_t ConnectCnt = 0;   // 이 세션의 접속 횟수 - 끊을 때 FIN/RST를 번갈아 고르는 데 쓴다
	ULONGLONG ConnectedTick = 0; // 접속한 시각 - (SendStall) 끊김까지 걸린 시간을 잰다
	ULONGLONG NextCheckTick = 0; // (SendStall) 다음 확인 send 시각
};
