#pragma once
#include <cstdint>
#include <string>
#include "LoginProtocol.h" // SESSION_KEY_LEN, GameProtocol.h

class ChannelCipher;

// 게임 서버 입장(설계 게임서버_입장_변경.docx 2판) - 클라·봇과 게임 서버가 같이 쓴다
// ① 클라 Hello [type | AccountNo] → ② 서버 Challenge [type | SeqNumber 시작값] → ③ 클라 입장 요청(통일 모양, 본문 = AccountNo, SeqNumber = 시작값)
// 입장 요청을 만드는 것과 푸는 것이 같은 통일 모양 함수(ChannelCipher::SealPacket/OpenPacket)를 지난다 - 입장 요청만의 다른 모양이 없다

bool InitCrypto(); // libsodium 초기화(16진수 바꾸기 등) - 프로그램 시작에 한 번

// 클라: 받은 SeqNumber 시작값으로 통로 키를 정하고(두 방향) 입장 요청 패킷을 만든다
void MakeEnterRequest(const uint8_t (&sessionKey)[SESSION_KEY_LEN], const uint64_t startSeq, const uint64_t accountNo, ChannelCipher* outCipher, std::string* outPacket);

// 게임 서버(DB 스레드): Redis에서 꺼낸 세션 키와 이 연결의 SeqNumber 시작값으로 입장 요청을 푼다
// SeqNumber·태그가 맞고, type이 LoginReq이고, 본문의 AccountNo가 Hello의 값과 같으면 true(통로 키가 정해짐, 받을 SeqNumber = 시작값 + 1). 아니면 false(통로 키를 지움)
bool OpenEnterRequest(const uint8_t (&sessionKey)[SESSION_KEY_LEN], const uint64_t startSeq, const uint64_t helloAccountNo,
	const char* packet, const size_t packetLen, ChannelCipher* outCipher);
