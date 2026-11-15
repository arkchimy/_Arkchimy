#pragma once
#include <cstdint>
#include "LoginProtocol.h" // ENTER_PROOF_LEN·ENTER_CHALLENGE_LEN(GameProtocol.h), SESSION_KEY_LEN

class ChannelCipher;

// 세션 키로 하는 일 - 게임 서버·클라·봇이 같이 쓴다(libsodium)
// 세션 키는 로그인 서버가 만들어 클라(로그인 응답)와 게임 서버(Redis)에 준다. 게임 연결로는 키를 보내지 않고,
// 그 키로 만든 증명(입장)과 암호문(입장 뒤 모든 메시지)으로만 키를 가졌음을 보인다
// 세션 키에서 용도별 하위 키를 따로 파생한다(crypto_kdf) - 증명과 메시지 암호화가 같은 키·논스를 다시 쓰지 않게

// 프로세스 시작 때 한 번 - 실패하면 false(난수 원천을 못 얻음)
bool InitCrypto();

// 입장 증명: 증명용 하위 키로 빈 메시지를 암호화한 확인 값. AccountNo와 이 연결의 EnterChallenge를 함께 묶어,
// 다른 번호나 다른 연결(가로챈 LoginReq를 다시 보냄)에서는 통하지 않는다
void MakeEnterProof(const uint8_t (&sessionKey)[SESSION_KEY_LEN], const uint64_t accountNo, const uint8_t (&challenge)[ENTER_CHALLENGE_LEN],
	uint8_t (&outProof)[ENTER_PROOF_LEN]);
bool VerifyEnterProof(const uint8_t (&sessionKey)[SESSION_KEY_LEN], const uint64_t accountNo, const uint8_t (&challenge)[ENTER_CHALLENGE_LEN],
	const uint8_t (&proof)[ENTER_PROOF_LEN]);

// 입장 뒤 게임 메시지의 방향별 키(클라→서버, 서버→클라) - bServer면 받기 = 클라→서버
void SetGameChannelKeys(const uint8_t (&sessionKey)[SESSION_KEY_LEN], const bool bServer, ChannelCipher* outCipher);
