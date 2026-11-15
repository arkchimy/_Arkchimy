#pragma once
#include <cstdint>
#include "GameProtocol.h"

// 로그인 서버 프로토콜 - 로그인 서버와 클라가 같이 쓴다(설계 login_server.docx 2장 ①②③, 5-1)
// 게임 서버와 같은 라이브러리 헤더(길이 2바이트) 뒤에 종류 2바이트
// 접속하면 먼저 ClientHello → ServerHello(평문)로 키를 나누고, 그 뒤의 메시지는 모두 암호문(SecureChannel)이다
enum class eLoginType : uint16_t
{
	ClientHello = 5, // 클라 → 로그인 서버(평문) - 이번 연결의 클라 임시 공개 키
	ServerHello = 6, // 로그인 서버 → 클라(평문) - 서버 임시 공개 키 + 서버 고정 키의 서명
	RegisterReq = 1, // 클라 → 로그인 서버
	RegisterRes = 2, // 로그인 서버 → 클라
	AuthReq = 3,     // 클라 → 로그인 서버 - 아이디·비밀번호 확인
	AuthRes = 4,     // 로그인 서버 → 클라 - 성공이면 AccountNo와 세션 키
};

enum : int32_t
{
	PASSWORD_MIN_LEN = 8,
	PASSWORD_MAX_LEN = 64, // 널 제외
	SESSION_KEY_LEN = 32,
	LOGIN_FAIL_MAX = 5,    // 한 세션에서 로그인이 이만큼 틀리면 끊는다(설계 6장)
	SESSION_KEY_TTL_SEC = 30,
};

enum class eRegisterResult : uint8_t
{
	Ok = 0,
	BadID = 1,       // 비었거나 15바이트 넘음
	BadPassword = 2, // PASSWORD_MIN_LEN–PASSWORD_MAX_LEN이 아님
	Exists = 3,      // 이미 있는 아이디
	ServerError = 4, // DB 오류 - 잠시 뒤 다시
};

enum class eAuthResult : uint8_t
{
	Ok = 0,
	Fail = 1,        // 아이디가 없거나 비밀번호가 틀림 - 어느 쪽인지 나누지 않는다(가입 응답의 "이미 있음"은 아이디가 있는지 알려 주므로 숨기는 것은 아님)
	ServerError = 2, // DB·Redis 오류 - 잠시 뒤 다시
};

enum : int32_t
{
	KX_PUBLIC_KEY_LEN = 32,      // X25519 공개 키
	SIGN_PUBLIC_KEY_LEN = 32,    // Ed25519 공개 키(클라가 미리 가진 서버 고정 키)
	SIGN_SECRET_KEY_LEN = 64,    // Ed25519 비밀 키(로그인 서버 login.cfg)
	SIGN_LEN = 64,               // Ed25519 서명
	CHANNEL_KEY_LEN = 32,        // 방향별 통로 키
	CHANNEL_TAG_LEN = 16,        // 암호문마다 붙는 확인 값
};

#pragma pack(push, 1)
struct ClientHello
{
	eLoginType Type; // ClientHello
	uint8_t ClientPublicKey[KX_PUBLIC_KEY_LEN];
};

struct ServerHello
{
	eLoginType Type; // ServerHello
	uint8_t ServerPublicKey[KX_PUBLIC_KEY_LEN];
	uint8_t Signature[SIGN_LEN]; // 서버 고정 키로 (서버 임시 공개 키 + 클라 임시 공개 키)에 서명 - 클라가 진짜 서버인지 확인한다
};

struct RegisterReq
{
	eLoginType Type;                      // RegisterReq
	char AccountID[ACCOUNT_ID_LEN];       // 널로 끝나야 한다
	char Password[PASSWORD_MAX_LEN + 1];  // 널로 끝나야 한다
};

struct RegisterRes
{
	eLoginType Type; // RegisterRes
	eRegisterResult Result;
};

struct AuthReq
{
	eLoginType Type; // AuthReq
	char AccountID[ACCOUNT_ID_LEN];
	char Password[PASSWORD_MAX_LEN + 1];
};

struct AuthRes
{
	eLoginType Type; // AuthRes
	eAuthResult Result;
	uint64_t AccountNo;                  // 게임 서버 입장 때 보낸다
	uint8_t SessionKey[SESSION_KEY_LEN]; // 게임 서버와의 비밀 - 게임 연결로는 보내지 않는다
};
#pragma pack(pop)
