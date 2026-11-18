#pragma once
#include <cstdint>
#include "RsaEchoProtocol.h"
#include "AesGcm.h"

// AES 에코 - RSA 에코(RsaEcho)의 비교군. 실제 로그인 서버 → 게임 서버 흐름과 같게 키를 나눈다
//   ① ClientHello / ServerHello, ② 가입·로그인은 RSA 에코와 같다(LoginChannel - 잠근 메시지 512바이트)
//   ③ 로그인 응답(RSA로 잠김)에 서버가 만든 세션 키 32바이트를 싣는다
//   ④ 그 뒤 에코는 세션 키로 AES-256-GCM - 패킷 = [암호문(평문과 같은 길이) | 태그 16바이트]
// 에코 메시지(RsaEchoMsg 110바이트)·묶음·계정 처리는 RSA 에코와 같다 - 다른 것은 에코를 잠그는 방법뿐
// 서버 고정 키는 RSA 에코의 것을 그대로 쓴다(..\..\RsaEcho\Server\rsa_echo.cfg, 클라는 rsa_echo_public.cfg) - 따로 만들 것이 없다

enum : int32_t
{
	AES_ECHO_PORT = 32200,
};

#pragma pack(push, 1)
struct AesLoginRes
{
	eRsaMsgType Type; // LoginRes
	eAuthResult Result;
	uint64_t AccountNo;
	uint8_t SessionKey[AES_KEY_LEN]; // Ok일 때만 - 이 연결의 에코를 잠그는 키
};
#pragma pack(pop)

static_assert(sizeof(AesLoginRes) <= RSA_MAX_PLAIN_LEN, "로그인 응답은 RSA 한 번에 들어가야 한다");

inline const char* const AES_ECHO_SECRET_CFG = "..\\..\\RsaEcho\\Server\\rsa_echo.cfg"; // 서버 작업 폴더(AesEcho\Server) 기준
