#include <sodium.h>
#include "PasswordHash.h"
#include "Assert.h"

static_assert(crypto_auth_hmacsha256_KEYBYTES == 32, "페퍼는 32바이트");

PasswordHasher::PasswordHasher(const PasswordHashConfig& config)
	: mConfig(config)
{
	Assert(mConfig.Pepper.size() == crypto_auth_hmacsha256_KEYBYTES, "PasswordHasher: 페퍼는 32바이트(설정을 읽을 때 확인)");
	const bool bOk = Hash("dummy-password-for-timing", &mDummyHash);
	Assert(bOk, "PasswordHasher: 시작 때 해시 실패(메모리 부족)");
}

void PasswordHasher::pepperPassword(const std::string& password, unsigned char* outMac) const
{
	Assert(outMac != nullptr, "pepperPassword: outMac이 null");
	crypto_auth_hmacsha256(outMac, reinterpret_cast<const unsigned char*>(password.data()), password.size(),
		reinterpret_cast<const unsigned char*>(mConfig.Pepper.data()));
}

bool PasswordHasher::Hash(const std::string& password, std::string* outHash) const
{
	Assert(outHash != nullptr, "PasswordHasher::Hash: outHash가 null");
	unsigned char mac[crypto_auth_hmacsha256_BYTES];
	pepperPassword(password, mac);
	char hash[crypto_pwhash_STRBYTES];
	// crypto_pwhash_str은 libsodium 기본 알고리즘을 쓴다 - 기본값이 바뀌어도 Argon2id 1.3이 되도록 알고리즘을 직접 지정하는 함수를 쓴다
	const bool bOk = crypto_pwhash_str_alg(hash, reinterpret_cast<const char*>(mac), sizeof(mac), mConfig.OpsLimit, mConfig.MemLimit,
		crypto_pwhash_ALG_ARGON2ID13) == 0;
	sodium_memzero(mac, sizeof(mac));
	if (bOk)
	{
		*outHash = hash;
	}
	return bOk;
}

bool PasswordHasher::Verify(const std::string& password, const std::string& hash) const
{
	unsigned char mac[crypto_auth_hmacsha256_BYTES];
	pepperPassword(password, mac);
	const bool bOk = crypto_pwhash_str_verify(hash.c_str(), reinterpret_cast<const char*>(mac), sizeof(mac)) == 0;
	sodium_memzero(mac, sizeof(mac));
	return bOk;
}

bool PasswordHasher::NeedsRehash(const std::string& hash) const
{
	return crypto_pwhash_str_needs_rehash(hash.c_str(), mConfig.OpsLimit, mConfig.MemLimit) != 0;
}

void PasswordHasher::VerifyDummy(const std::string& password) const
{
	Verify(password, mDummyHash);
}
