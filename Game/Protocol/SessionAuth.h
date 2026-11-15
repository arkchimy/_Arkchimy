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

// 입장 증명: 증명용 하위 키로 만든 키 MAC(BLAKE2b 16바이트) - AccountNo와 이 연결의 EnterChallenge를 함께 넣어,
// 엿본 LoginReq를 다른 연결에서 다시 보내도 통하지 않는다. 능동 중간자가 자기 연결의 EnterChallenge를 넘겨 증명을 받아 쓰면
// 입장까지는 될 수 있지만 통로 키(세션 키에서 파생)가 없어 그 뒤 메시지를 보내지도 읽지도 못한다(입장을 막는 정도)
void MakeEnterProof(const uint8_t (&sessionKey)[SESSION_KEY_LEN], const uint64_t accountNo, const uint8_t (&challenge)[ENTER_CHALLENGE_LEN],
	uint8_t (&outProof)[ENTER_PROOF_LEN]);
bool VerifyEnterProof(const uint8_t (&sessionKey)[SESSION_KEY_LEN], const uint64_t accountNo, const uint8_t (&challenge)[ENTER_CHALLENGE_LEN],
	const uint8_t (&proof)[ENTER_PROOF_LEN]);

// 입장 뒤 게임 메시지의 방향별 키(클라→서버, 서버→클라) - bServer면 받기 = 클라→서버
void SetGameChannelKeys(const uint8_t (&sessionKey)[SESSION_KEY_LEN], const bool bServer, ChannelCipher* outCipher);
