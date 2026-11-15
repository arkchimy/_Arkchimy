#pragma once
#include <string>
#include "LoginProtocol.h"

// 클라·봇이 로그인 서버에 요청할 때 - 접속해 요청 하나를 보내고 응답 하나를 받은 뒤 끊는다(블록, 응답이 늦으면 실패)
// 소켓 초기화(WSAStartup)는 부르는 쪽이 이미 했어야 한다. false면 접속·송수신 실패(서버 결과가 아님)
bool RequestRegister(const char* ip, const int port, const std::string& accountID, const std::string& password, eRegisterResult* outResult);
bool RequestAuth(const char* ip, const int port, const std::string& accountID, const std::string& password, AuthRes* outRes);
