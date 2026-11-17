#pragma once
#include <string>
#include "LoginProtocol.h"

// 클라·봇이 로그인 서버에 요청할 때 - 접속해 임시 공개 키를 주고받고(ClientHello/ServerHello), RSA로 잠근 요청 하나를 보내고 응답 하나를 받은 뒤 끊는다
// 블록 소켓이고 응답이 늦으면 실패한다. 소켓 초기화(WSAStartup)는 부르는 쪽이 이미 했어야 한다. 접속마다 RSA 키 쌍을 만들어 수십 ms가 더 걸린다
struct LoginServerAddr
{
	std::string IP;
	int Port = 33002;
	uint8_t SignPublicKey[RSA_MODULUS_LEN] = {}; // 미리 가진 서버 고정 공개 키(n) - ServerHello와 응답의 서명을 확인한다
};

enum class eLoginCall : uint8_t
{
	Ok = 0,         // 응답을 받음(결과는 응답 안에)
	NoConnection,   // 접속 못 함
	BadServer,      // 서명이 서버 고정 키와 맞지 않음 - 가짜 서버(중간자)일 수 있어 비밀번호를 보내지 않았다
	Broken,         // 주고받다 끊김·시간 초과·응답이 풀리지 않음
};

// server_public.cfg의 serverPublicKey(16진수 512글자) - 없거나 틀리면 false
bool LoadServerPublicKey(const char* path, uint8_t (&outKey)[RSA_MODULUS_LEN]);

eLoginCall RequestRegister(const LoginServerAddr& addr, const std::string& accountID, const std::string& password, eRegisterResult* outResult);
eLoginCall RequestAuth(const LoginServerAddr& addr, const std::string& accountID, const std::string& password, AuthRes* outRes);

// 시험용: 한 연결에서 AuthReq를 count번 보내며 받은 응답 수(서버가 끊으면 거기까지)
int RequestAuthRepeat(const LoginServerAddr& addr, const std::string& accountID, const std::string& password, const int count);
// 시험용: 잠근 요청 한 바이트를 바꿔 보낸다(서명이 맞지 않게 된다) - 서버가 응답 없이 끊으면 true
bool RequestAuthTampered(const LoginServerAddr& addr, const std::string& accountID, const std::string& password);

// 시험용: 클라가 서명을 제대로 확인하는지(설계 login_rsa_change.docx 2-6) - 연결 둘을 열어 ServerHello를 받고, 한 연결에서 로그인 응답 둘을 받는다
struct LoginSignatureCheck
{
	bool bConnected = false;           // 두 연결 모두 ServerHello·응답 둘까지 받음
	bool bTamperedHelloRejected = false; // 서명 한 바이트를 바꾼 ServerHello → 거절
	bool bOtherHelloRejected = false;    // 다른 연결이 받은 ServerHello(클라 공개 키가 다름) → 거절
	bool bReplayRejected = false;        // 앞 응답을 다음 응답 자리에 다시 → 거절(응답 번호)
	bool bNextAccepted = false;          // 그 뒤 진짜 다음 응답 → 통과
};
LoginSignatureCheck CheckLoginSignatures(const LoginServerAddr& addr, const std::string& accountID, const std::string& password);
