#pragma once
#include <cstdint>

// 게임 서버 프로토콜 - 서버와 클라가 같이 쓴다
// 종류는 라이브러리 헤더가 아니라 payload 맨 앞 2바이트에 컨텐츠가 넣는다(라이브러리는 길이로 자르기만 한다)
enum class eGameType : uint16_t
{
	LoginReq = 1,     // 클라 → 서버
	LoginRes = 2,     // 서버 → 클라
	InventoryReq = 3, // 클라 → 서버
	InventoryRes = 4, // 서버 → 클라
	PartyCreateReq = 5,   // 클라 → 서버
	PartyCreateRes = 6,   // 서버 → 클라
	PartyListReq = 7,     // 클라 → 서버
	PartyListRes = 8,     // 서버 → 클라
	PartyJoinReq = 9,     // 클라 → 서버
	PartyJoinRes = 10,    // 서버 → 클라
	PartyLeaveReq = 11,   // 클라 → 서버 - 응답 없음, 바뀐 상태는 PartyInfoNotify로. 파티에 없으면 무시
	PartyStartReq = 12,   // 클라 → 서버
	PartyStartRes = 13,   // 서버 → 클라
	PartyInfoNotify = 14, // 서버 → 파티원 전원 - 참가·나가기·방장 변경·시작마다
	// 던전·경매는 다음 단계에서 이어서 번호를 붙인다
};

enum : int32_t
{
	ACCOUNT_ID_LEN = 16,  // 아이디 최대 길이(널 포함)
	INVENTORY_MAX = 32,   // 인벤토리 최대 칸 - 응답 한 패킷에 다 들어가야 한다
	PARTY_MEMBER_MAX = 4, // 파티 최대 인원
	PARTY_START_MIN = 2,  // 던전을 시작할 수 있는 최소 인원
	PARTY_LIST_MAX = 32,  // 목록 응답에 담는 파티 수 - 넘으면 앞의 32개만
};

enum class eLoginResult : uint8_t
{
	Ok = 0,
	BadID = 1,           // 비었거나 널로 끝나지 않음
	AlreadyLoggedIn = 2, // 같은 아이디가 이미 접속 중 - 서버가 이전 접속을 끊으므로 잠시 뒤 다시 로그인하면 된다
};

enum class ePartyState : uint8_t
{
	None = 0,       // 파티 없음 - 나간 사람에게 보내는 알림
	Recruiting = 1, // 모집 중 - 참가 가능
	InDungeon = 2,  // 던전 시작됨 - 참가 불가
};

enum class eCreatePartyResult : uint8_t
{
	Ok = 0, // 뒤이어 PartyInfoNotify가 온다
	AlreadyInParty = 1,
};

enum class eJoinPartyResult : uint8_t
{
	Ok = 0,            // 뒤이어 파티원 전원에게 PartyInfoNotify
	NotFound = 1,      // 없는 번호, 또는 방금 없어진 파티
	Full = 2,
	AlreadyStarted = 3,
	AlreadyInParty = 4,
};

enum class eStartPartyResult : uint8_t
{
	Ok = 0,               // 뒤이어 파티원 전원에게 PartyInfoNotify(InDungeon)
	NotInParty = 1,
	NotLeader = 2,
	NotEnoughMembers = 3, // PARTY_START_MIN명 미만
	AlreadyStarted = 4,
};

enum class eEquipSlot : uint8_t
{
	Weapon = 0,
	Head = 1,
	Accessory = 2,
	Armor = 3,
	Count = 4,
};

// 구조체를 그대로 보내므로 서버·클라의 배치가 같도록 패딩을 없앤다
#pragma pack(push, 1)
struct LoginReq
{
	eGameType Type;                 // LoginReq
	char AccountID[ACCOUNT_ID_LEN]; // 널로 끝나야 한다
};

struct LoginRes
{
	eGameType Type; // LoginRes
	eLoginResult Result;
};

struct InventoryReq
{
	eGameType Type; // InventoryReq
};

struct ItemInfo
{
	uint64_t ItemUID;  // 아이템 하나하나의 고유 번호 - 경매·회수는 이 번호로 가리킨다
	uint16_t ItemKind; // 아이템 데이터 파일의 번호(회복약, 녹슨 곡괭이 등) - 이름은 클라가 같은 파일에서 찾는다
};

struct InventoryRes
{
	eGameType Type; // InventoryRes
	int64_t Gold;
	uint64_t Equipped[static_cast<int32_t>(eEquipSlot::Count)]; // 칸마다 착용한 아이템의 ItemUID, 0이면 비어 있음
	uint8_t ItemCnt;                                            // Items 중 앞의 몇 개가 유효한지 - 패킷은 항상 고정 크기로 보낸다
	ItemInfo Items[INVENTORY_MAX];                              // 착용한 아이템도 여기에 들어 있다
};

struct PartyCreateReq
{
	eGameType Type; // PartyCreateReq
};

struct PartyCreateRes
{
	eGameType Type; // PartyCreateRes
	eCreatePartyResult Result;
	uint64_t PartyID; // 성공했을 때만 유효. 1부터, 0은 "파티 없음"
};

struct PartyListReq
{
	eGameType Type; // PartyListReq
};

struct PartyListEntry
{
	uint64_t PartyID;
	char LeaderID[ACCOUNT_ID_LEN];
	uint8_t MemberCnt;
};

struct PartyListRes
{
	eGameType Type;                         // PartyListRes
	uint8_t PartyCnt;                       // Entries 중 앞의 몇 개가 유효한지 - 모집 중인 파티만
	PartyListEntry Entries[PARTY_LIST_MAX];
};

struct PartyJoinReq
{
	eGameType Type; // PartyJoinReq
	uint64_t PartyID;
};

struct PartyJoinRes
{
	eGameType Type; // PartyJoinRes
	eJoinPartyResult Result;
};

struct PartyLeaveReq
{
	eGameType Type; // PartyLeaveReq
};

struct PartyStartReq
{
	eGameType Type; // PartyStartReq
};

struct PartyStartRes
{
	eGameType Type; // PartyStartRes
	eStartPartyResult Result;
};

// 파티가 바뀔 때마다 파티 락 안에서 파티원 전원에게 - 받는 사람 모두 같은 순서로 같은 상태를 본다(설계 3-8-5)
struct PartyInfoNotify
{
	eGameType Type;  // PartyInfoNotify
	uint64_t PartyID; // None이면 0
	ePartyState State;
	uint8_t LeaderIdx;                                 // MemberIDs 중 방장의 위치 - 지금은 항상 0(참가 순서상 맨 앞이 방장)
	uint8_t MemberCnt;
	char MemberIDs[PARTY_MEMBER_MAX][ACCOUNT_ID_LEN]; // 참가한 순서 - 방장이 나가면 다음 사람이 방장(설계 3-8-4)
};
#pragma pack(pop)

static_assert(sizeof(InventoryRes) <= 4096, "인벤토리 응답은 패킷 하나(최대 페이로드 4096)에 들어가야 한다");
static_assert(INVENTORY_MAX <= UINT8_MAX, "ItemCnt가 uint8_t라 인벤토리 칸 수는 255를 넘을 수 없다");
static_assert(sizeof(PartyListRes) <= 4096, "파티 목록은 패킷 하나에 들어가야 한다");
static_assert(PARTY_LIST_MAX <= UINT8_MAX && PARTY_MEMBER_MAX <= UINT8_MAX, "PartyCnt·MemberCnt가 uint8_t");
