#pragma once
#include <cstdint>
#include <string>

// RSA 에코 시험 클라 설정 - client.cfg에서 읽는다
struct ClientConfig
{
	std::string IP = "127.0.0.1";
	uint16_t Port = 0;
	int32_t SessionCnt = 0;   // 세션 수 - 처음 접속에서 가입 → 로그인, 다시 접속하면 로그인만, 그 뒤 에코 묶음을 반복
	int32_t ThreadCnt = 1;    // select 스레드 수 - 세션을 나눠 맡는다(클라도 메시지마다 RSA 비밀 키 연산 2번이라 스레드 하나면 클라가 먼저 찬다)
	int32_t EchoBatch = 4;    // 에코 묶음 하나의 메시지 수 - 응답을 모두 받아야 다음 묶음
	int32_t LifeMinSec = 0;   // 끊고 다시 접속하기까지(무작위, 최소–최대) - Hello·로그인을 다시 하게
	int32_t LifeMaxSec = 0;   // 0이면 끊지 않는다
	std::string KeyFile = "..\\Server\\rsa_echo_public.cfg"; // 서버 고정 공개 키(serverPublicKey)
};
