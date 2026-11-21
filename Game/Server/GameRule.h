#pragma once
#include <cstdint>

// 게임 서버 규칙 - server.cfg에서 읽는다. 키가 없으면 아래 기본값
struct GameRule
{
	int32_t EntryLimitSec = 60; // 접속한 뒤 이 시간 안에 입장하지 않으면 끊는다(0이면 끔) - 아무것도 보내지 않는 연결이 세션 칸을 채우지 못하게
	                            // 시험 클라는 띄우자마자 접속하고 사람이 가입·로그인을 치므로 짧게 두지 않는다
};
