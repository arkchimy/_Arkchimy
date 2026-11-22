#pragma once
#include <cstdint>
#include "PacketHeader.h"

// 모니터링 서버 프로토콜(설계 portfolio/design/모니터링서버_설계.docx) - 라이브러리 헤더 + 첫 바이트 종류
// 보내는 쪽(로그인·게임 서버의 MonitorAgent)과 보는 쪽(MonitorClient)이 같은 포트로 붙고, 첫 패킷(AgentHello / ViewerHello)으로 역할을 정한다
// 서버는 누적치(RAW_*)를 보내고 모니터가 1초 값(MON_*)을 계산한다 - 표본이 늦거나 빠져도 값이 틀어지지 않게(설계 4-2)
#pragma pack(push, 1)

enum class eMonType : uint8_t
{
	AgentHello = 1,   // 서버 → 모니터: 키, 이름, 종류
	Sample = 2,       // 서버 → 모니터: 1초마다 누적치
	Bye = 3,          // 서버 → 모니터: 정상 종료(응답 없음과 구분)
	ViewerHello = 10, // 클라 → 모니터: 키
	HelloRes = 11,    // 모니터 → 클라: 키 결과
	ServerState = 12, // 모니터 → 클라: 서버 하나의 상태(접속·응답 없음·종료)
	Rate = 13,        // 모니터 → 클라: 서버 하나의 1초 값(실시간)
	Event = 14,       // 모니터 → 클라: 이벤트 한 줄
	HistoryReq = 15,  // 클라 → 모니터: 지난 기록(한 번에 하나만 - 답을 다 받은 뒤 다음)
	HistoryRes = 16,  // 모니터 → 클라: 기록 행 묶음
};

enum class eServerKind : uint8_t
{
	Login = 1,
	Game = 2,
};

enum class eServerState : uint8_t
{
	Online = 1,
	NoResponse = 2, // 표본이 MON_NO_RESPONSE_SEC 넘게 오지 않음 - 죽었거나 멈췄거나 망이 끊김
	Stopped = 3,    // Bye를 받음(정상 종료)
};

enum : int32_t
{
	MON_KEY_LEN = 32,        // 키 최대 길이(널 포함) - db.cfg의 monitorKey
	MON_NAME_LEN = 16,       // 서버 이름(널 포함) - "login", "game"
	MON_EVENT_TEXT_LEN = 64,
	MON_CONTENT_CNT = 8,     // 서버 종류마다 뜻이 다른 칸(MON_CONTENT_LABELS)
	MON_NO_RESPONSE_SEC = 3, // 설계 4-4
	MON_HISTORY_ROWS = 32,   // HistoryRes 한 패킷의 최대 행 수
	MON_HISTORY_PACKETS = 4, // HistoryReq 하나에 보내는 최대 패킷 수 - 보는 쪽 송신 링(32KB)을 넘지 않게
};

// 서버가 보내는 누적치(또는 그 순간 값) - 칸 번호. 정수 칸 번호로만 쓰므로 이름 없는 상수 묶음(CODING_STANDARD 예외)
enum : int32_t
{
	RAW_PROC_CPU_100NS = 0, // 프로세스 CPU 시간(유저 + 커널, 100ns 단위, 누적)
	RAW_PRIVATE_BYTES,      // 사설 메모리(바이트, 순간 값)
	RAW_HANDLE_CNT,         // 핸들 수(순간 값)
	RAW_SYS_BUSY_100NS,     // PC 전체 CPU가 일한 시간(누적) - GetSystemTimes 커널 + 유저 - 유휴
	RAW_SYS_TOTAL_100NS,    // PC 전체 CPU 시간(누적) - 커널 + 유저
	RAW_SYS_AVAIL_MB,       // PC 남은 물리 메모리(MB, 순간 값)
	RAW_SESSION,            // 세션 수(순간 값)
	RAW_ACCEPT,             // 이하 누적 - 라이브러리 NetStat
	RAW_RECV_PACKET,
	RAW_SEND_PACKET,
	RAW_RECV_BYTES,
	RAW_SEND_BYTES,
	RAW_OVERFLOW,           // 끊은 이유: 송신 링 가득 참
	RAW_SEND_STALL,         // 끊은 이유: 송신 정체
	RAW_SESSION_FULL,       // 끊은 이유: 빈 세션 없음
	RAW_BAD_HEADER,         // 끊은 이유: 헤더 길이(라이브러리)
	RAW_BAD_PACKET,         // 끊은 이유: 종류·길이(컨텐츠)
	RAW_CONTENT,            // 여기부터 MON_CONTENT_CNT칸 - 서버 종류마다 뜻이 다르다
	RAW_CNT = RAW_CONTENT + MON_CONTENT_CNT,
};

// 모니터가 계산한 1초 값 - 칸 번호. DB 열 순서도 같다(MON_VALUE_COLUMNS). 이름 없는 상수 묶음(위와 같은 이유)
enum : int32_t
{
	MON_PROC_CPU = 0, // 코어(1.0 = 코어 하나를 다 씀)
	MON_MEM_MB,
	MON_HANDLES,
	MON_SYS_CPU_PCT,  // PC 전체 CPU %
	MON_SYS_AVAIL_MB,
	MON_SESSION,
	MON_ACCEPT_PS,    // 이하 초당
	MON_RECV_PACKET_PS,
	MON_SEND_PACKET_PS,
	MON_RECV_KB_PS,
	MON_SEND_KB_PS,
	MON_OVERFLOW_PS,
	MON_SEND_STALL_PS,
	MON_SESSION_FULL_PS,
	MON_BAD_HEADER_PS,
	MON_BAD_PACKET_PS,
	MON_CONTENT,      // 여기부터 MON_CONTENT_CNT칸 - 누적 칸은 초당, 순간 칸은 그 값
	MON_VALUE_CNT = MON_CONTENT + MON_CONTENT_CNT,
};

constexpr const char* MON_VALUE_COLUMNS[MON_VALUE_CNT] = {
	"proc_cpu", "mem_mb", "handles", "sys_cpu_pct", "sys_avail_mb", "session",
	"accept_ps", "recv_packet_ps", "send_packet_ps", "recv_kb_ps", "send_kb_ps",
	"overflow_ps", "send_stall_ps", "session_full_ps", "bad_header_ps", "bad_packet_ps",
	"c0", "c1", "c2", "c3", "c4", "c5", "c6", "c7",
};

// 서버 종류별 컨텐츠 칸의 이름(클라 표시용). 순서는 각 서버 main의 표본 채우기와 같다
constexpr const char* MON_LOGIN_CONTENT_LABELS[MON_CONTENT_CNT] = {
	"로그인성공/s", "로그인실패/s", "가입/s", "처리중워커", "키쌍", "키쌍부족끊김/s", "요청기한끊김/s", "확인시간초과/s",
};
constexpr const char* MON_GAME_CONTENT_LABELS[MON_CONTENT_CNT] = {
	"입장한 계정", "방인원", "채팅/s", "DB대기", "DB실패/s", "입장기한끊김/s", "입장확인실패/s", "입장거절/s",
};
// 순간 값(누적이 아님)인 컨텐츠 칸 - 비트 i가 칸 i
constexpr uint32_t MON_LOGIN_GAUGE_MASK = (1u << 3) | (1u << 4);
constexpr uint32_t MON_GAME_GAUGE_MASK = (1u << 0) | (1u << 1) | (1u << 3);

inline bool IsMonContentGauge(const eServerKind kind, const int32_t slot)
{
	const uint32_t mask = (kind == eServerKind::Login) ? MON_LOGIN_GAUGE_MASK : MON_GAME_GAUGE_MASK;
	return (mask & (1u << slot)) != 0;
}

// 서버 이름 - 영문·숫자·_·- 만, 1–15자(모니터가 SQL에 그대로 넣는다). 보내는 쪽은 시작할 때, 모니터는 받을 때 검사한다
inline bool IsValidMonName(const char* name, const size_t len)
{
	if (len == 0 || len >= MON_NAME_LEN)
	{
		return false;
	}
	for (size_t i = 0; i < len; ++i)
	{
		const char c = name[i];
		const bool bOk = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
		if (!bOk)
		{
			return false;
		}
	}
	return true;
}

inline const char* GetMonContentLabel(const eServerKind kind, const int32_t slot)
{
	return (kind == eServerKind::Login) ? MON_LOGIN_CONTENT_LABELS[slot] : MON_GAME_CONTENT_LABELS[slot];
}

struct MonAgentHello
{
	eMonType Type; // AgentHello
	char Key[MON_KEY_LEN];
	char Name[MON_NAME_LEN];
	eServerKind Kind;
};

struct MonSample
{
	eMonType Type;  // Sample
	int64_t UnixMs; // 서버 시각 - 행 시각 표시에만 쓴다. 여러 PC면 시간 동기화(w32tm)가 필요하다
	int64_t MonoMs; // 서버의 단조 시각(GetTickCount64) - 1초 값의 경과 시간은 이것으로(시계를 맞춰 벽시계가 건너뛰어도 값이 튀지 않게)
	int64_t Raw[RAW_CNT];
};

struct MonBye
{
	eMonType Type; // Bye
};

struct MonViewerHello
{
	eMonType Type; // ViewerHello
	char Key[MON_KEY_LEN];
};

struct MonHelloRes
{
	eMonType Type; // HelloRes
	uint8_t bOk;
};

struct MonServerState
{
	eMonType Type; // ServerState
	char Name[MON_NAME_LEN];
	eServerKind Kind;
	eServerState State;
};

struct MonRateRow
{
	int64_t UnixSec;
	float Values[MON_VALUE_CNT];
};

struct MonRate
{
	eMonType Type; // Rate
	char Name[MON_NAME_LEN];
	MonRateRow Row;
};

struct MonEvent
{
	eMonType Type; // Event
	char Name[MON_NAME_LEN];
	int64_t UnixMs;
	char Text[MON_EVENT_TEXT_LEN]; // UTF-8 - 보는 쪽(ImGui)이 그대로 그린다
};

struct MonHistoryReq
{
	eMonType Type; // HistoryReq
	uint32_t ReqID;
	char Name[MON_NAME_LEN];
	int64_t FromSec; // [FromSec, ToSec]
	int64_t ToSec;
	uint8_t bMinute; // 1이면 1분 요약(평균), 0이면 1초 원본
};

struct MonHistoryRes
{
	eMonType Type; // HistoryRes
	uint32_t ReqID;
	uint8_t RowCnt;
	uint8_t bLast;       // 1이면 이 요청의 마지막 패킷
	uint8_t bDbError;    // 1이면 DB 조회가 실패해 메모리에 있던 최근 행만 담았다
	int64_t NextFromSec; // bLast이고 0이 아니면 행이 더 있다 - 이 시각부터 다시 요청
	MonRateRow Rows[MON_HISTORY_ROWS];
};

#pragma pack(pop)

static_assert(sizeof(MonSample) <= MAX_PAYLOAD_SIZE, "MonSample이 최대 페이로드보다 큼");
static_assert(sizeof(MonHistoryRes) <= MAX_PAYLOAD_SIZE, "MonHistoryRes가 최대 페이로드보다 큼");
// 32768 = 라이브러리 SEND_RING_SIZE(NetConfig.h) - 클라는 라이브러리 헤더를 보지 않아 숫자로 적는다. 송신 링을 바꾸면 여기도 고친다
