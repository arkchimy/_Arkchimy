#pragma once
#include <cstdint>
#include <string>

// 시험 클라 설정 - client.cfg에서 읽는다. 역할별 세션 수를 바꿔 시험을 고른다
// 시험 1(잘린 패킷 조립): normalCnt = 640
// 시험 2(길이 범위 밖 헤더): normalCnt = 576, badHeaderCnt = 64
// 시험 3(세션 반납): normalCnt = 640, lifeMinSec = 30, lifeMaxSec = 60
// 송신 정체: normalCnt = 576, sendStallCnt = 64 (서버 sendBufZero = 1, sendStallSec = 3)
// 몰아 보내기: normalCnt = 64, burstCnt = 64 (서버 skipLoopLimit 0이면 끊김 재현, 3이면 끊기지 않아야 함)
struct ClientConfig
{
	std::string IP = "127.0.0.1";
	uint16_t Port = 0;
	int32_t NormalCnt = 0;    // 정상 세션 - 응답을 모두 받은 뒤에만 다음 묶음을 보낸다
	int32_t BadHeaderCnt = 0; // 헤더 길이를 범위 밖(MAX_PAYLOAD_SIZE + 1, 65535)으로 보내고 서버가 끊는지 본다
	int32_t SendStallCnt = 0; // 요청을 보내고 응답을 받지 않는 세션 - 서버의 송신 정체 판정으로 끊기는지 본다
	int32_t BurstCnt = 0;     // 60KB를 다 보낸 뒤에야 받는 세션 - 서버의 즉시 완료 반복 동안 송신이 미뤄져 넘치면 끊긴다(설계 즉시완료_반복_상한). 회귀 시험용 - ClientSession.h의 eSessionRole::Burst 주석
	int32_t LifeMinSec = 0;   // 정상 세션이 끊기 전까지 주고받는 시간(무작위, 최소–최대)
	int32_t LifeMaxSec = 0;   // 0이면 끊지 않는다 - 끊을지는 이 값으로 정한다(LifeMinSec = 0, LifeMaxSec = 60이면 0–60초 뒤에 끊는다)
};
