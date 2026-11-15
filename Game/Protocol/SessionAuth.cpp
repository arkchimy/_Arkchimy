#include <sodium.h>
#include "SessionAuth.h"
#include "LoginProtocol.h"
#include "Memory.h"

static_assert(sizeof(uint8_t[SESSION_KEY_LEN]) == crypto_kdf_KEYBYTES, "세션 키 크기 = KDF 키 크기");
static_assert(ENTER_PROOF_LEN == crypto_aead_xchacha20poly1305_ietf_ABYTES, "증명 크기 = AEAD 확인 값 크기");

static const char KDF_CONTEXT[crypto_kdf_CONTEXTBYTES + 1] = "ArkGame1"; // 파생 용도 이름(8바이트) - 다른 곳에서 같은 키를 써도 다른 하위 키가 나오게

enum : uint64_t
{
	SUBKEY_CLIENT_TO_SERVER = 1,
	SUBKEY_SERVER_TO_CLIENT = 2,
};

bool InitCrypto()
{
	return sodium_init() >= 0; // 1이면 이미 됨
}

static void deriveClientToServer(const uint8_t (&sessionKey)[SESSION_KEY_LEN], uint8_t (&outKey)[crypto_aead_xchacha20poly1305_ietf_KEYBYTES])
{
	crypto_kdf_derive_from_key(outKey, sizeof(outKey), SUBKEY_CLIENT_TO_SERVER, KDF_CONTEXT, sessionKey);
}

static void makeEnterAd(const uint64_t accountNo, uint8_t (&outAd)[sizeof(uint64_t) + 5])
{
	MemCpy(outAd, &accountNo, sizeof(accountNo)); // 클라·서버 모두 x64(리틀 엔디언)
	MemCpy(outAd + sizeof(accountNo), "enter", 5);
}

void MakeEnterProof(const uint8_t (&sessionKey)[SESSION_KEY_LEN], const uint64_t accountNo, uint8_t (&outProof)[ENTER_PROOF_LEN])
{
	uint8_t key[crypto_aead_xchacha20poly1305_ietf_KEYBYTES];
	deriveClientToServer(sessionKey, key);
	uint8_t nonce[crypto_aead_xchacha20poly1305_ietf_NPUBBYTES] = {}; // 순번 0
	uint8_t ad[sizeof(uint64_t) + 5];
	makeEnterAd(accountNo, ad);
	unsigned long long outLen = 0;
	crypto_aead_xchacha20poly1305_ietf_encrypt(outProof, &outLen, nullptr, 0, ad, sizeof(ad), nullptr, nonce, key);
	sodium_memzero(key, sizeof(key));
}

bool VerifyEnterProof(const uint8_t (&sessionKey)[SESSION_KEY_LEN], const uint64_t accountNo, const uint8_t (&proof)[ENTER_PROOF_LEN])
{
	uint8_t key[crypto_aead_xchacha20poly1305_ietf_KEYBYTES];
	deriveClientToServer(sessionKey, key);
	uint8_t nonce[crypto_aead_xchacha20poly1305_ietf_NPUBBYTES] = {};
	uint8_t ad[sizeof(uint64_t) + 5];
	makeEnterAd(accountNo, ad);
	uint8_t dummy[1];
	unsigned long long outLen = 0;
	const bool bOk = crypto_aead_xchacha20poly1305_ietf_decrypt(dummy, &outLen, nullptr, proof, ENTER_PROOF_LEN, ad, sizeof(ad), nonce, key) == 0;
	sodium_memzero(key, sizeof(key));
	return bOk;
}
