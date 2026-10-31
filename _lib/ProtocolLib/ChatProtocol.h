#pragma once
#include <cstdint>

// 채팅 데모의 컨텐츠 패킷 - 서버(ChatServer)와 부하 클라(LoadClient chat 모드)가 같이 쓴다
// Data 페이로드 맨 앞 1바이트가 종류다. 라이브러리 헤더에는 종류를 넣지 않는다(라이브러리는 길이로 자르기만 한다)
enum class eChatType : uint8_t
{
	Join = 1,	// 클라 → 서버: 방 입장 (다른 방에 있었으면 옮긴다)
	JoinOk = 2,	// 서버 → 클라: 입장 완료 - 방 락 안에서 보내므로, 이 뒤에 오는 Chat부터가 이 방의 메시지다
	Chat = 3,	// 클라 → 서버 → 방 전원(보낸 사람 포함)
};

#pragma pack(push, 1)
struct ChatJoin
{
	eChatType type;	// Join
	uint32_t room;
};

struct ChatJoinOk
{
	eChatType type;	// JoinOk
	uint32_t room;
};

// 서버는 받은 바이트를 그대로 방 전원에게 보내되 roomSeq만 방 락 안에서 채운다. 뒤에 채움 바이트가 붙을 수 있다
struct ChatMsg
{
	eChatType type;	// Chat
	uint64_t roomSeq;	// 서버가 매김 - 방마다 1씩 증가. 받는 쪽은 +1씩 오는지로 "받는 사람 모두 같은 순서, 누락 없음"을 검사한다
	uint64_t senderKey;	// 보낸 클라가 세션마다 정한 값 - 자기 메시지를 알아보는 데 쓴다
	uint64_t senderSeq;	// 보낸 사람별 순번 - 자기 메시지가 빠지지 않았는지 확인
};
#pragma pack(pop)
static_assert(sizeof(ChatJoin) == 5, "Join은 패딩 없이 5바이트");
static_assert(sizeof(ChatJoinOk) == 5, "JoinOk는 패딩 없이 5바이트");
static_assert(sizeof(ChatMsg) == 25, "Chat은 패딩 없이 25바이트");
