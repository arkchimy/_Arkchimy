#pragma once
#include <cstdint>

// 게임 규칙 - 시나리오 표(CSV)가 아니라 규칙이라 server.cfg에서 읽는다(설계 3-9). 키가 없으면 아래 기본값
struct GameRule
{
	int32_t PlayerHp = 20;         // 던전을 시작할 때의 체력
	int32_t PlayerAttack = 2;      // 공격 판정 보너스(장비가 더한다)
	int32_t PlayerDefense = 12;    // 적의 공격 판정 기준(장비가 더한다)
	int32_t PlayerInvestigate = 0; // 조사·함정 판정 보너스(장비가 더한다)
	int32_t TurnTimeMs = 10000;    // 한 차례의 제한 시간
	int32_t DungeonGold = 50;      // 보스를 잡으면 파티원 각자 받는 골드
	int32_t AuctionDurationSec = 60; // 경매 등록부터 마감까지
};
