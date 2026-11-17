#pragma once
#include <cstdint>
#include <string>
#include "LoginProtocol.h"

// RSA 에코 - 로그인 서버와 같은 RSA 흐름(LoginCrypto)으로 잠근 메시지를 주고받으며 RSA 연산 시간을 잰다
//   ① 클라 → ClientHello(클라 임시 공개 키)        ② 서버 → ServerHello(서버 임시 공개 키 + 서버 고정 키 서명)
//   ③ 클라 → 잠근 요청(평문 RSA_MAX_PLAIN_LEN 이하)  ④ 서버 → 같은 평문을 다시 잠근 응답
// 패킷 모양은 로그인 서버와 같다 - Hello는 LoginProtocol.h의 구조체, 그 뒤는 LOGIN_SEALED_LEN(512)바이트
// 서버 고정 키는 로그인 서버와 같게 RsaEchoServer.exe --genkey로 한 번 만든다(2026-11-17 사용자 결정) - 비밀 키는 서버의 rsa_echo.cfg,
// 공개 키는 클라가 읽는 rsa_echo_public.cfg에 넣는다. 둘 다 .gitignore(예시 파일만 저장소에)

enum : int32_t
{
	RSA_ECHO_PORT = 32100,
	RSA_ECHO_KEY_MAX_USE = 5,      // 서버 임시 키 하나를 이만큼의 세션이 쓰면 바꾼다(2026-11-17 사용자 결정)
	RSA_ECHO_PUBLIC_HEX_LEN = RSA_MODULUS_LEN * 2,
};

inline const char* const RSA_ECHO_SECRET_CFG = "rsa_echo.cfg";        // 서버 작업 폴더 - signSecretKey
inline const char* const RSA_ECHO_PUBLIC_CFG = "rsa_echo_public.cfg"; // 클라 기본값은 ..\Server\ 아래 - serverPublicKey

inline std::string ToHex(const uint8_t* data, const size_t size)
{
	static const char DIGITS[] = "0123456789abcdef";
	std::string hex;
	hex.reserve(size * 2);
	for (size_t i = 0; i < size; ++i)
	{
		hex.push_back(DIGITS[data[i] >> 4]);
		hex.push_back(DIGITS[data[i] & 0x0F]);
	}
	return hex;
}

// 16진수 → 바이트. 길이가 정확히 size × 2이고 모두 16진수 글자일 때만 true
inline bool FromHex(const std::string& hex, uint8_t* outData, const size_t size)
{
	if (outData == nullptr || hex.size() != size * 2)
	{
		return false;
	}
	bool bOk = true;
	for (size_t i = 0; i < size && bOk; ++i)
	{
		int value = 0;
		for (size_t k = 0; k < 2; ++k)
		{
			const char c = hex[i * 2 + k];
			int digit = -1;
			if (c >= '0' && c <= '9')
			{
				digit = c - '0';
			}
			else if (c >= 'a' && c <= 'f')
			{
				digit = c - 'a' + 10;
			}
			else if (c >= 'A' && c <= 'F')
			{
				digit = c - 'A' + 10;
			}
			bOk = bOk && digit >= 0;
			value = value * 16 + digit;
		}
		outData[i] = static_cast<uint8_t>(value);
	}
	return bOk;
}

// 16진수 → 바이트(길이가 정해지지 않은 값 - 비밀 키 형식). 글자 수가 홀수이거나 16진수가 아닌 글자가 있으면 false
inline bool HexToBytes(const std::string& hex, std::string* outBytes)
{
	if (outBytes == nullptr || hex.size() % 2 != 0)
	{
		return false;
	}
	outBytes->resize(hex.size() / 2);
	return FromHex(hex, reinterpret_cast<uint8_t*>(outBytes->data()), outBytes->size());
}
