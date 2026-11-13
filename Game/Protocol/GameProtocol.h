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
	// 파티·던전·경매는 다음 단계에서 이어서 번호를 붙인다
};

enum : int32_t
{
	ACCOUNT_ID_LEN = 16, // 아이디 최대 길이(널 포함)
	INVENTORY_MAX = 32,  // 인벤토리 최대 칸 - 응답 한 패킷에 다 들어가야 한다
};

enum class eLoginResult : uint8_t
{
	Ok = 0,
	BadID = 1,           // 비었거나 널로 끝나지 않음
	AlreadyLoggedIn = 2, // 같은 아이디가 이미 접속 중 - 중복 로그인을 막는다
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
#pragma pack(pop)

static_assert(sizeof(InventoryRes) <= 4096, "인벤토리 응답은 패킷 하나(최대 페이로드 4096)에 들어가야 한다");
