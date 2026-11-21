#pragma once
#include <cstdint>
#include "GameProtocol.h"

// 운영 툴 ↔ 게임 서버의 운영자 포트(설계 3-5) - 게임 프로토콜과 같은 방식(라이브러리 헤더 + 첫 바이트 종류)
// 메모리와 DB를 함께 고쳐야 하는 차단·해제만 둔다(2026-11-20 범위 축소 - 아이템 회수·로그 조회는 지웠다)
#pragma pack(push, 1)

enum class eAdminType : uint8_t
{
	LoginReq = 1, // 운영 툴 → 서버 - 운영자 키. 맞지 않으면 서버가 끊는다
	LoginRes = 2,
	BanReq = 5,   // 3·4는 지운 회수 명령 - 다시 쓰지 않는다
	BanRes = 6,
};

enum : int32_t
{
	ADMIN_KEY_LEN = 32, // 운영자 키 최대 길이(널 포함) - db.cfg의 adminKey
	ADMIN_BAN_MINUTES_MAX = 60 * 24 * 365,
};

enum class eBanResult : uint8_t
{
	Ok = 0,
	NotFound = 1, // 그런 계정이 없음(게임 서버에 한 번도 입장하지 않은 아이디 포함 - 계정 행은 첫 입장 때 생긴다)
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

struct AdminBanReq
{
	eAdminType Type; // BanReq
	char AccountID[ACCOUNT_ID_LEN];
	int32_t Minutes; // 0이면 차단 해제. 차단하면 접속 중인 세션도 끊는다
};

struct AdminBanRes
{
	eAdminType Type; // BanRes
	eBanResult Result;
	uint8_t bKicked; // 접속 중이라 끊었으면 1
};

#pragma pack(pop)
