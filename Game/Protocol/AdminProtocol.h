#pragma once
#include <cstdint>
#include "GameProtocol.h"

// 운영 툴 ↔ 게임 서버의 운영자 포트(설계 3-5) - 게임 프로토콜과 같은 방식(라이브러리 헤더 + 첫 바이트 종류)
// 로그 조회는 운영 툴이 DB를 직접 읽으므로 여기에 없다. 메모리와 DB를 함께 고쳐야 하는 회수·제재만 서버를 거친다(설계 3-4)
#pragma pack(push, 1)

enum class eAdminType : uint8_t
{
	LoginReq = 1, // 운영 툴 → 서버 - 운영자 키. 맞지 않으면 서버가 끊는다
	LoginRes = 2,
	RecallReq = 3,
	RecallRes = 4,
	BanReq = 5,
	BanRes = 6,
};

enum : int32_t
{
	ADMIN_KEY_LEN = 32,   // 운영자 키 최대 길이(널 포함) - db.cfg의 adminKey
	ADMIN_NOTE_LEN = 48,  // 운영자 메모(UTF-8) - 로그의 detail에 남는다
	ADMIN_BAN_MINUTES_MAX = 60 * 24 * 365,
};

enum class eRecallResult : uint8_t
{
	Ok = 0,
	NotFound = 1, // 그런 아이템이 없음(이미 쓰였거나 버려짐)
	Moved = 2,    // 회수하는 사이 아이템이 옮겨짐(경매 등록·낙찰) - 다시 시도
	DbError = 3,
};

enum class eRecallFrom : uint8_t
{
	Inventory = 0,      // 메모리에 있는 계정의 인벤토리(착용 중이면 벗기고)
	Auction = 1,        // 경매 중 - 경매를 취소하고 입찰자에게 환불
	OfflineAccount = 2, // 이번 실행에서 로그인하지 않은 계정 - DB에서만 지움
};

enum class eBanResult : uint8_t
{
	Ok = 0,
	NotFound = 1, // 그런 계정이 없음
	BadValue = 2, // 분이 범위 밖
	DbError = 3,
};

struct AdminLoginReq
{
	eAdminType Type; // LoginReq
	char Key[ADMIN_KEY_LEN];
};

struct AdminLoginRes
{
	eAdminType Type; // LoginRes
	uint8_t bOk;
};

struct AdminRecallReq
{
	eAdminType Type; // RecallReq
	uint64_t ItemUID;
	char Note[ADMIN_NOTE_LEN]; // 회수 사유
};

struct AdminRecallRes
{
	eAdminType Type; // RecallRes
	eRecallResult Result;
	eRecallFrom From;
	char OwnerID[ACCOUNT_ID_LEN];
	ItemInfo Item;
};

struct AdminBanReq
{
	eAdminType Type; // BanReq
	char AccountID[ACCOUNT_ID_LEN];
	int32_t Minutes; // 0이면 차단 해제. 차단하면 접속 중인 세션도 끊는다
	char Note[ADMIN_NOTE_LEN];
};

struct AdminBanRes
{
	eAdminType Type; // BanRes
	eBanResult Result;
	uint8_t bKicked; // 접속 중이라 끊었으면 1
};

#pragma pack(pop)
