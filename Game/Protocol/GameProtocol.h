#pragma once
#include <cstddef>
#include <cstdint>

// 게임 서버 프로토콜 - 서버와 클라가 같이 쓴다(설계 게임서버_입장_변경.docx 2판, 흐름 게임서버_클라_흐름.docx)
// 범위(2026-11-20 사용자 결정): 입장(인증 분산)과 계정 DB를 먼저 시험으로 다지고, 컨텐츠는 방 하나의 채팅만 둔다
//
// 패킷(라이브러리 헤더 = 길이 2바이트 뒤의 payload)
//  - Hello·Challenge만 평문 구조체 그대로 - 아직 키를 쓸 수 없는 단계
//  - 그 뒤는 입장 요청을 포함해 모두 한 모양: [PacketHead = type | SeqNumber](평문, AD) + 암호문(본문) + 태그 16바이트
//    논스 = 방향 4바이트(클라→서버 1, 서버→클라 2) + SeqNumber 8바이트. SeqNumber는 방향마다 "SeqNumber 시작값"부터 메시지 하나에 1씩
//    SeqNumber 시작값 = 서버가 Hello를 받을 때 그 연결을 위해 만든 무작위 8바이트(Challenge로 알려 준다) - 연결마다 달라 가로챈 패킷을 다른 연결에서 쓸 수 없다
//    받는 쪽: ① SeqNumber가 기대한 값인가(아니면 풀지 않고 끊기) ② 태그(AD = PacketHead)가 맞는가 ③ type으로 분기
//  - 게임 서버는 입장에 실패하면 이유와 상관없이 응답 없이 끊는다 - 그래서 LoginRes는 늘 Ok(eLoginResult의 실패 값은 서버 안에서 이유를 나눠 세는 데만 쓴다)
enum class eGameType : uint16_t
{
	Hello = 1,      // 클라 → 서버, 평문 - 접속하자마자. AccountNo로 서버가 Redis에서 어느 키를 꺼낼지 안다
	Challenge = 2,  // 서버 → 클라, 평문 - 이 연결의 SeqNumber 시작값
	LoginReq = 3,   // 클라 → 서버, 통일 모양 - 본문 = AccountNo(Hello와 같아야 한다). SeqNumber = 시작값
	LoginRes = 4,   // 서버 → 클라, 통일 모양 - 본문 = 결과(늘 Ok). SeqNumber = 시작값
	ChatReq = 5,    // 클라 → 서버, 통일 모양 - 본문 = 글자(가변)
	ChatNotify = 6, // 서버 → 방의 모든 세션(보낸 사람 포함), 통일 모양 - 본문 = 보낸 사람 아이디 + 글자(가변). 방 락 안에서 보내 모두 같은 순서
};

enum : int32_t
{
	ACCOUNT_ID_LEN = 16,     // 아이디 최대 길이(널 포함)
	CHANNEL_TAG_LEN = 16,    // 통일 모양 패킷 끝의 태그(확인 값)
	CHAT_TEXT_MAX = 200,     // 채팅 한 줄 최대 바이트(UTF-8, 널 없음) - 1바이트 이상
};

enum class eLoginResult : uint8_t
{
	Ok = 0,
	AuthFail = 1, // SeqNumber·태그·AccountNo가 틀리거나 Redis에 세션 키가 없음(30초 지남·이미 씀) - 키는 지우지 않는다
	Banned = 2,   // 운영자가 입장을 막은 아이디 - 세션 키는 이미 지웠다
	// DbError는 어디서 실패했는지에 따라 다르다 - 대기열이 밀려 바로 거절했거나 Redis를 읽지 못했으면 키가 남아 있고, 키를 지운 뒤 계정을 읽지 못했으면 키는 이미 쓰였다
	DbError = 3,  // Redis·DB 오류, 또는 서버가 밀림
	// 같은 계정이 이미 입장해 있으면 실패가 아니다 - 기존 연결을 끊고 새 연결이 입장한다(설계 2판 4장, 사용자 결정)
};

// 구조체를 그대로 보내므로 서버·클라의 배치가 같도록 패딩을 없앤다
#pragma pack(push, 1)
struct Hello
{
	eGameType Type; // Hello
	uint64_t AccountNo;
};

struct Challenge
{
	eGameType Type; // Challenge
	uint64_t StartSeq; // SeqNumber 시작값
};

// 통일 모양 패킷의 평문 앞부분 - 태그 계산에 AD로 들어가 바꾸면 태그가 틀린다
struct PacketHead
{
	eGameType Type;
	uint64_t Seq; // SeqNumber
};

struct LoginReqBody
{
	uint64_t AccountNo; // Hello의 AccountNo와 같아야 한다 - 키로 확인된 값이 평문 Hello의 값과 같은지 한 번 더 본다(설계 2판 5-2)
};

struct LoginResBody
{
	eLoginResult Result;
};

// 채팅은 가변 길이 - ChatReq 본문은 글자 n바이트(1 ≤ n ≤ CHAT_TEXT_MAX), ChatNotify 본문은 SenderID + 글자 n바이트
struct ChatNotifyBody
{
	char SenderID[ACCOUNT_ID_LEN]; // 보낸 사람 아이디(15바이트 아이디면 널로 끝나지 않을 수 있다 - 널 포함 16)
	char Text[CHAT_TEXT_MAX];
};
#pragma pack(pop)

enum : int32_t
{
	PACKET_HEAD_LEN = sizeof(PacketHead),                                        // 10
	SEALED_OVERHEAD = PACKET_HEAD_LEN + CHANNEL_TAG_LEN,                         // 통일 모양 패킷 = 본문 + 26
	LOGIN_REQ_PACKET_LEN = SEALED_OVERHEAD + static_cast<int32_t>(sizeof(LoginReqBody)), // 34
	LOGIN_RES_PACKET_LEN = SEALED_OVERHEAD + static_cast<int32_t>(sizeof(LoginResBody)), // 27
};
static_assert(sizeof(Hello) == 10 && sizeof(Challenge) == 10, "Hello·Challenge는 같은 10바이트 - 서버가 받은 것보다 많이 보내지 않는다");
static_assert(SEALED_OVERHEAD + sizeof(ChatNotifyBody) <= 4096, "채팅 알림은 통일 모양으로도 패킷 하나(최대 페이로드 4096)에 들어가야 한다");
