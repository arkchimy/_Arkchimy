#include <sodium.h>
#include "SessionAuth.h"
#include "Memory.h"
#include "SecureChannel.h"

static_assert(sizeof(uint8_t[SESSION_KEY_LEN]) == crypto_kdf_KEYBYTES, "세션 키 크기 = KDF 키 크기");
static_assert(ENTER_PROOF_LEN == crypto_aead_xchacha20poly1305_ietf_ABYTES, "증명 크기 = AEAD 확인 값 크기");

static const char KDF_CONTEXT[crypto_kdf_CONTEXTBYTES + 1] = "ArkGame1"; // 파생 용도 이름(8바이트) - 다른 곳에서 같은 키를 써도 다른 하위 키가 나오게

enum : uint64_t
{
	SUBKEY_ENTER_PROOF = 1,       // 입장 증명(논스 0 한 번만)
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
	uint8_t key[CHANNEL_KEY_LEN];
	deriveSubkey(sessionKey, SUBKEY_ENTER_PROOF, key);
	uint8_t nonce[crypto_aead_xchacha20poly1305_ietf_NPUBBYTES] = {}; // 증명용 키는 이 한 번만 쓴다 - 논스 0
	uint8_t ad[ENTER_AD_LEN];
	makeEnterAd(accountNo, challenge, ad);
	unsigned long long outLen = 0;
	crypto_aead_xchacha20poly1305_ietf_encrypt(outProof, &outLen, nullptr, 0, ad, sizeof(ad), nullptr, nonce, key);
	sodium_memzero(key, sizeof(key));
}

bool VerifyEnterProof(const uint8_t (&sessionKey)[SESSION_KEY_LEN], const uint64_t accountNo, const uint8_t (&challenge)[ENTER_CHALLENGE_LEN],
	const uint8_t (&proof)[ENTER_PROOF_LEN])
{
	uint8_t key[CHANNEL_KEY_LEN];
	deriveSubkey(sessionKey, SUBKEY_ENTER_PROOF, key);
	uint8_t nonce[crypto_aead_xchacha20poly1305_ietf_NPUBBYTES] = {};
	uint8_t ad[ENTER_AD_LEN];
	makeEnterAd(accountNo, challenge, ad);
	uint8_t dummy[1];
	unsigned long long outLen = 0;
	const bool bOk = crypto_aead_xchacha20poly1305_ietf_decrypt(dummy, &outLen, nullptr, proof, ENTER_PROOF_LEN, ad, sizeof(ad), nonce, key) == 0;
	sodium_memzero(key, sizeof(key));
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
