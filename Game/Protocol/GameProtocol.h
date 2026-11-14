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
	DungeonActionReq = 15,   // 클라 → 서버
	DungeonTurnNotify = 16,  // 서버 → 파티원 전원 - 차례가 바뀔 때마다(지금 상태 전체)
	DungeonEventNotify = 17, // 서버 → 파티원 전원 - 판정 하나마다
	DungeonEndNotify = 18,   // 서버 → 파티원 각자 - 성공/실패와 내가 받은 보상
	EquipReq = 19,           // 클라 → 서버 - 성공하면 EquipRes 뒤에 InventoryRes
	UnequipReq = 20,         // 클라 → 서버 - 성공하면 EquipRes 뒤에 InventoryRes
	EquipRes = 21,           // 서버 → 클라
	AuctionRegisterReq = 22,  // 클라 → 서버
	AuctionRegisterRes = 23,  // 서버 → 클라
	AuctionListReq = 24,      // 클라 → 서버
	AuctionListRes = 25,      // 서버 → 클라
	AuctionBidReq = 26,       // 클라 → 서버
	AuctionBidRes = 27,       // 서버 → 클라
	AuctionWatchReq = 28,     // 클라 → 서버 - 경매장 보기 켜기/끄기(응답 없음)
	AuctionNotify = 29,       // 서버 → 구경꾼 전원 - 새 등록·입찰·마감(경매장 락 안에서, 모두 같은 순서)
	AuctionResultNotify = 30, // 서버 → 당사자 - 낙찰·판매·유찰·밀려남·취소
};

enum : int32_t
{
	ACCOUNT_ID_LEN = 16,  // 아이디 최대 길이(널 포함)
	INVENTORY_MAX = 32,   // 인벤토리 최대 칸 - 응답 한 패킷에 다 들어가야 한다
	PARTY_MEMBER_MAX = 4, // 파티 최대 인원
	PARTY_START_MIN = 2,  // 던전을 시작할 수 있는 최소 인원
	PARTY_LIST_MAX = 32,  // 목록 응답에 담는 파티 수 - 넘으면 앞의 32개만
	DUNGEON_MONSTER_MAX = 4,   // 한 방의 몬스터 최대 수(박쥐 = 파티 인원)
	DUNGEON_REWARD_MAX = 5,    // 한 사람이 받을 수 있는 전리품 최대 수
	ACTOR_MONSTER_FLAG = 0x80, // Actor·Target 값에서 이 비트가 서면 몬스터 번호, 아니면 파티원 번호
	AUCTION_LIST_MAX = 32,     // 목록 응답에 담는 경매 수
	AUCTION_PER_PLAYER = 5,    // 한 사람이 동시에 등록할 수 있는 경매 수
	AUCTION_PRICE_MAX = 1000000,
	AUCTION_TOTAL_MAX = 1000,      // 경매장 전체 경매 수 - 공짜 계정으로 무한히 올리지 못하게
	AUCTION_RAISE_PERCENT = 5,     // 다음 입찰은 현재가 + 현재가의 5%(최소 1) 이상 - 1골드씩 올리는 입찰로 알림이 폭주하지 않게
	AUCTION_BID_INTERVAL_MS = 100, // 한 세션의 입찰 간격 - 짧게 몰아 보내면 거절
};

enum class eAuctionRegisterResult : uint8_t
{
	Ok = 0,
	NotFound = 1,  // 내 인벤토리에 없는 아이템
	Equipped = 2,  // 착용 중 - 먼저 해제
	InDungeon = 3, // 던전 중에는 등록할 수 없음
	TooMany = 4,   // AUCTION_PER_PLAYER개를 넘음
	BadPrice = 5,  // 1 ~ AUCTION_PRICE_MAX
	HouseFull = 6, // 경매장 전체가 AUCTION_TOTAL_MAX개
};

enum class eAuctionBidResult : uint8_t
{
	Ok = 0,
	NotFound = 1,      // 없는 경매(이미 마감)
	TooLow = 2,        // 첫 입찰은 시작가 이상, 그 뒤는 현재가 + 5%(최소 1) 이상
	NotEnoughGold = 3,
	OwnAuction = 4,    // 내가 올린 경매
	AlreadyTop = 5,    // 내가 이미 최고 입찰자
	TooFast = 6,       // AUCTION_BID_INTERVAL_MS 안에 다시 입찰
};

enum class eAuctionEvent : uint8_t
{
	Registered = 0,
	Bid = 1,
	Closed = 2,    // Entry.bHasBid면 BidderID가 낙찰자, 아니면 유찰
	Cancelled = 3, // 거래 취소(구매자 인벤토리 가득 참, 운영자 회수) - 입찰은 무효
};

enum class eAuctionResult : uint8_t
{
	Sold = 0,      // 판매자: 팔림 - Gold만큼 받음
	Won = 1,       // 구매자: 낙찰 - 아이템을 받음
	Outbid = 2,    // 입찰자: 더 높은 입찰에 밀림 - Gold만큼 돌려받음
	Unsold = 3,    // 판매자: 입찰 없이 마감 또는 구매자 쪽 취소 - 아이템을 돌려받음
	Cancelled = 4, // 입찰자: 거래 취소(인벤토리 가득 참, 운영자 회수) - Gold만큼 돌려받음
	Discarded = 5, // 판매자: 돌려받을 아이템을 인벤토리가 가득 차 버림
	Recalled = 6,  // 판매자: 운영자가 경매 중인 아이템을 회수
};

enum class eDungeonAction : uint8_t
{
	Attack = 0,
	Defend = 1,
	Investigate = 2, // 조사할 대상이 있는 방에서만
	UsePotion = 3,   // 회복약이 있을 때만
};

enum class eDungeonEvent : uint8_t
{
	EnterRoom = 0,      // 방에 들어감 - RoomNo
	Attack = 1,         // Actor가 Target을 공격 - Roll + Bonus ≥ Difficulty면 명중, Damage
	Defend = 2,         // 이번 라운드 받는 피해 절반
	AutoDefend = 3,     // 시간 초과·끊김으로 자동 방어
	Investigate = 4,    // 조사 판정 - 성공이면 방의 보상
	UsePotion = 5,      // 회복 - Damage에 회복량
	Trap = 6,           // 함정 판정 - 실패면 Damage
	Special = 7,        // 보스의 돌 부스러기 - 파티원마다 하나씩
	Down = 8,           // Actor(파티원·몬스터)가 쓰러짐
	ActionRejected = 9, // 할 수 없는 행동(조사 대상 없음, 회복약 없음) - 차례는 넘어가지 않는다
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

enum class eEquipResult : uint8_t
{
	Ok = 0,
	NotFound = 1,      // 내 인벤토리에 없는 아이템
	NotEquippable = 2, // 장비가 아님(회복약 등)
	InDungeon = 3,     // 던전 중에는 바꿀 수 없음(설계 3-9-1 - 던전 상태에 복사한 능력치와 어긋나지 않게)
	BadSlot = 4,       // 없는 장비 칸
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

struct DungeonActionReq
{
	eGameType Type;  // DungeonActionReq
	uint32_t TurnNo; // DungeonTurnNotify로 받은 턴 번호 - 이미 넘어간 턴이면 서버가 무시
	eDungeonAction Action;
	uint8_t Target;  // 공격할 몬스터 번호(DungeonTurnNotify의 몬스터 순서). 공격이 아니면 무시. 범위 밖·쓰러진 몬스터면 ActionRejected
};

struct DungeonTurnNotify
{
	eGameType Type; // DungeonTurnNotify
	uint32_t TurnNo;
	uint8_t RoomNo;       // 1부터
	uint8_t ActorIdx;     // 지금 차례인 파티원(PartyInfoNotify의 MemberIDs 순서)
	uint16_t TimeLimitMs; // 이 차례의 제한 시간
	uint8_t MemberCnt;
	int16_t MemberHp[PARTY_MEMBER_MAX]; // 0이면 쓰러짐
	uint8_t MonsterCnt;
	uint16_t MonsterKind[DUNGEON_MONSTER_MAX];
	int16_t MonsterHp[DUNGEON_MONSTER_MAX];
};

struct DungeonEventNotify
{
	eGameType Type; // DungeonEventNotify
	eDungeonEvent Event;
	uint8_t Actor;      // 파티원 번호, 몬스터면 ACTOR_MONSTER_FLAG | 몬스터 번호
	uint8_t Target;
	uint8_t Roll;       // 주사위 값(d20 판정이면 1–20), 판정이 없으면 0
	int8_t Bonus;       // 판정에 더한 능력치
	uint8_t Difficulty; // 기준값(적 방어·난이도)
	uint8_t bSuccess;
	int16_t Damage;     // 피해 또는 회복량
	uint8_t RoomNo;
};

struct DungeonEndNotify
{
	eGameType Type; // DungeonEndNotify
	uint8_t bSuccess;
	int32_t Gold;                       // 내가 받은 골드
	uint8_t ItemCnt;
	ItemInfo Items[DUNGEON_REWARD_MAX]; // 내가 받은 아이템
	uint8_t DiscardedCnt;               // 인벤토리가 가득 차 버린 아이템 수
};

struct EquipReq
{
	eGameType Type;   // EquipReq
	uint64_t ItemUID; // 착용할 아이템 - 같은 칸에 착용한 것이 있으면 바꾼다
};

struct UnequipReq
{
	eGameType Type; // UnequipReq
	eEquipSlot Slot;
};

struct EquipRes
{
	eGameType Type; // EquipRes
	eEquipResult Result;
};

struct AuctionRegisterReq
{
	eGameType Type;   // AuctionRegisterReq
	uint64_t ItemUID; // 착용 중이 아닌 내 아이템 - 등록하면 계정에서 빠져 경매장이 보관한다(설계 3-10-1)
	int32_t StartPrice;
};

struct AuctionRegisterRes
{
	eGameType Type; // AuctionRegisterRes
	eAuctionRegisterResult Result;
	uint64_t AuctionID; // 성공했을 때만
};

struct AuctionListReq
{
	eGameType Type;    // AuctionListReq
	uint16_t ItemKind; // 0이면 전체 - 이름 검색은 클라가 자기 데이터 파일에서 종류 번호로 바꾼다(설계 3-10-5)
	uint64_t AfterAuctionID; // 이 번호보다 큰 경매부터(0이면 처음부터) - 다음 쪽은 받은 마지막 번호를 넣는다
};

struct AuctionEntry
{
	uint64_t AuctionID;
	uint16_t ItemKind;
	char SellerID[ACCOUNT_ID_LEN];
	int32_t Price;  // 입찰이 있으면 최고 입찰가, 없으면 시작가
	uint8_t bHasBid;
	char BidderID[ACCOUNT_ID_LEN];
	uint16_t RemainSec;
};

struct AuctionListRes
{
	eGameType Type; // AuctionListRes
	uint8_t Cnt;
	AuctionEntry Entries[AUCTION_LIST_MAX];
};

struct AuctionBidReq
{
	eGameType Type; // AuctionBidReq
	uint64_t AuctionID;
	int32_t Price;  // 이만큼 골드를 미리 빼 둔다 - 밀리면 돌려받는다(설계 3-10-2)
};

struct AuctionBidRes
{
	eGameType Type; // AuctionBidRes
	eAuctionBidResult Result;
};

struct AuctionWatchReq
{
	eGameType Type; // AuctionWatchReq
	uint8_t bWatch;
};

struct AuctionNotify
{
	eGameType Type; // AuctionNotify
	eAuctionEvent Event;
	AuctionEntry Entry;
};

struct AuctionResultNotify
{
	eGameType Type; // AuctionResultNotify
	eAuctionResult Result;
	uint64_t AuctionID;
	ItemInfo Item;
	int32_t Gold;
};
#pragma pack(pop)

static_assert(sizeof(InventoryRes) <= 4096, "인벤토리 응답은 패킷 하나(최대 페이로드 4096)에 들어가야 한다");
static_assert(INVENTORY_MAX <= UINT8_MAX, "ItemCnt가 uint8_t라 인벤토리 칸 수는 255를 넘을 수 없다");
static_assert(sizeof(PartyListRes) <= 4096, "파티 목록은 패킷 하나에 들어가야 한다");
static_assert(sizeof(AuctionListRes) <= 4096, "경매 목록은 패킷 하나에 들어가야 한다");
static_assert(PARTY_MEMBER_MAX + 1 <= DUNGEON_REWARD_MAX, "한 사람이 전리품을 모두(파티 인원 + 숨은 상자 1) 받아도 DungeonEndNotify에 다 들어가야 한다");
static_assert(PARTY_LIST_MAX <= UINT8_MAX && PARTY_MEMBER_MAX <= UINT8_MAX, "PartyCnt·MemberCnt가 uint8_t");
