#include <sodium.h>
#include "SessionAuth.h"
#include "Memory.h"
#include "SecureChannel.h"

static_assert(sizeof(uint8_t[SESSION_KEY_LEN]) == crypto_kdf_KEYBYTES, "세션 키 크기 = KDF 키 크기");
static_assert(ENTER_PROOF_LEN >= crypto_generichash_BYTES_MIN && CHANNEL_KEY_LEN >= crypto_generichash_KEYBYTES_MIN, "증명 = BLAKE2b 키 MAC 16바이트");

static const char KDF_CONTEXT[crypto_kdf_CONTEXTBYTES + 1] = "ArkGame1"; // 파생 용도 이름(8바이트) - 다른 곳에서 같은 키를 써도 다른 하위 키가 나오게

enum : uint64_t
{
	SUBKEY_ENTER_PROOF = 1,       // 입장 증명(키 MAC)
	SUBKEY_CLIENT_TO_SERVER = 3,  // 입장 뒤 게임 메시지
	SUBKEY_SERVER_TO_CLIENT = 4,
};

enum : int32_t
{
	ENTER_AD_LEN = sizeof(uint64_t) + 5 + ENTER_CHALLENGE_LEN, // AccountNo + "enter" + EnterChallenge
};

bool InitCrypto()
{
	return sodium_init() >= 0; // 1이면 이미 됨
}

static void deriveSubkey(const uint8_t (&sessionKey)[SESSION_KEY_LEN], const uint64_t subkeyID, uint8_t (&outKey)[CHANNEL_KEY_LEN])
{
	crypto_kdf_derive_from_key(outKey, sizeof(outKey), subkeyID, KDF_CONTEXT, sessionKey);
}

static void makeEnterAd(const uint64_t accountNo, const uint8_t (&challenge)[ENTER_CHALLENGE_LEN], uint8_t (&outAd)[ENTER_AD_LEN])
{
	MemCpy(outAd, &accountNo, sizeof(accountNo)); // 클라·서버 모두 x64(리틀 엔디언)
	MemCpy(outAd + sizeof(accountNo), "enter", 5);
	MemCpy(outAd + sizeof(accountNo) + 5, challenge, ENTER_CHALLENGE_LEN);
}

void MakeEnterProof(const uint8_t (&sessionKey)[SESSION_KEY_LEN], const uint64_t accountNo, const uint8_t (&challenge)[ENTER_CHALLENGE_LEN],
	uint8_t (&outProof)[ENTER_PROOF_LEN])
{
	// 키 MAC(BLAKE2b, 출력 16바이트) - 논스가 없어 같은 키로 여러 번 만들어도 안전하다
	// (처음에는 AEAD를 논스 0으로 썼는데, 증명이 틀리면 키가 남아 같은 키로 여러 번 만들 수 있어 두 확인 값으로 위조할 수 있었다 - 리뷰 지적)
	uint8_t key[CHANNEL_KEY_LEN];
	deriveSubkey(sessionKey, SUBKEY_ENTER_PROOF, key);
	uint8_t ad[ENTER_AD_LEN];
	makeEnterAd(accountNo, challenge, ad);
	crypto_generichash(outProof, sizeof(outProof), ad, sizeof(ad), key, sizeof(key));
	sodium_memzero(key, sizeof(key));
}

bool VerifyEnterProof(const uint8_t (&sessionKey)[SESSION_KEY_LEN], const uint64_t accountNo, const uint8_t (&challenge)[ENTER_CHALLENGE_LEN],
	const uint8_t (&proof)[ENTER_PROOF_LEN])
{
	uint8_t expected[ENTER_PROOF_LEN];
	MakeEnterProof(sessionKey, accountNo, challenge, expected);
	const bool bOk = sodium_memcmp(expected, proof, sizeof(expected)) == 0; // 처음부터 끝까지 비교 - 어디서 달라졌는지 시간으로 드러나지 않게
	sodium_memzero(expected, sizeof(expected));
	return bOk;
}

void SetGameChannelKeys(const uint8_t (&sessionKey)[SESSION_KEY_LEN], const bool bServer, ChannelCipher* outCipher)
{
	Assert(outCipher != nullptr, "SetGameChannelKeys: outCipher가 null");
	uint8_t clientToServer[CHANNEL_KEY_LEN];
	uint8_t serverToClient[CHANNEL_KEY_LEN];
	deriveSubkey(sessionKey, SUBKEY_CLIENT_TO_SERVER, clientToServer);
	deriveSubkey(sessionKey, SUBKEY_SERVER_TO_CLIENT, serverToClient);
	if (bServer)
	{
		outCipher->SetKeys(clientToServer, serverToClient); // 받기, 보내기
	}
	else
	{
		outCipher->SetKeys(serverToClient, clientToServer);
	}
	sodium_memzero(clientToServer, sizeof(clientToServer));
	sodium_memzero(serverToClient, sizeof(serverToClient));
}
