#pragma once
#include <cstdint>

// 게임 서버 규칙 - server.cfg에서 읽는다. 키가 없으면 아래 기본값
struct GameRule
{
	int32_t EntryLimitSec = 60; // 접속한 뒤 이 시간 안에 입장하지 않으면 끊는다(0이면 끔) - 아무것도 보내지 않는 연결이 세션 칸을 채우지 못하게
	                            // 시험 클라는 띄우자마자 접속하고 사람이 가입·로그인을 치므로 짧게 두지 않는다
	int32_t IdleLimitSec = 30;  // 입장한 뒤 이 시간 동안 아무것도 받지 못하면 끊는다(0이면 끔) - 클라는 HEARTBEAT_INTERVAL_SEC마다 하트비트를 보낸다.
	                            // 상대가 사라져도 TCP는 보낼 것이 없으면 알아채지 못한다(송신 정체는 보낼 것이 있을 때만 걸린다)
};
