#pragma once
#include <string>
#include "LoginProtocol.h"

// 클라·봇이 로그인 서버에 요청할 때 - 접속해 키를 나누고(ClientHello/ServerHello), 암호문으로 요청 하나를 보내고 응답 하나를 받은 뒤 끊는다
// 블록 소켓이고 응답이 늦으면 실패한다. 소켓 초기화(WSAStartup)·InitCrypto는 부르는 쪽이 이미 했어야 한다
struct LoginServerAddr
{
	std::string IP;
	int Port = 33002;
	uint8_t SignPublicKey[SIGN_PUBLIC_KEY_LEN] = {}; // 미리 가진 서버 고정 공개 키 - ServerHello 서명을 확인한다
};

enum class eLoginCall : uint8_t
{
	Ok = 0,         // 응답을 받음(결과는 응답 안에)
	NoConnection,   // 접속 못 함
	BadServer,      // 서명이 서버 고정 키와 맞지 않음 - 가짜 서버(중간자)일 수 있어 비밀번호를 보내지 않았다
	Broken,         // 주고받다 끊김·시간 초과·응답이 풀리지 않음
};

// server_public.cfg의 serverPublicKey(16진수 64글자) - 없거나 틀리면 false
bool LoadServerPublicKey(const char* path, uint8_t (&outKey)[SIGN_PUBLIC_KEY_LEN]);

eLoginCall RequestRegister(const LoginServerAddr& addr, const std::string& accountID, const std::string& password, eRegisterResult* outResult);
eLoginCall RequestAuth(const LoginServerAddr& addr, const std::string& accountID, const std::string& password, AuthRes* outRes);

// 시험용: 한 연결에서 AuthReq를 count번 보내며 받은 응답 수(서버가 끊으면 거기까지)
int RequestAuthRepeat(const LoginServerAddr& addr, const std::string& accountID, const std::string& password, const int count);
// 시험용: 암호문 한 바이트를 바꿔 보낸다 - 서버가 응답 없이 끊으면 true
bool RequestAuthTampered(const LoginServerAddr& addr, const std::string& accountID, const std::string& password);
