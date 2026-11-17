#include <sodium.h>
#include "SecureChannel.h"
#include "Memory.h"

static_assert(CHANNEL_KEY_LEN == crypto_aead_xchacha20poly1305_ietf_KEYBYTES, "통로 키 크기");
static_assert(CHANNEL_TAG_LEN == crypto_aead_xchacha20poly1305_ietf_ABYTES, "확인 값 크기");

// 순번을 논스 앞 8바이트에(나머지는 0) - 방향마다 키가 달라 두 방향이 같은 순번을 써도 겹치지 않는다
static void makeNonce(const uint64_t seq, uint8_t (&outNonce)[crypto_aead_xchacha20poly1305_ietf_NPUBBYTES])
{
	MemSet(outNonce, 0, sizeof(outNonce));
	MemCpy(outNonce, &seq, sizeof(seq));
}

ChannelCipher::~ChannelCipher()
{
	Clear();
}

void ChannelCipher::SetKeys(const uint8_t (&rxKey)[CHANNEL_KEY_LEN], const uint8_t (&txKey)[CHANNEL_KEY_LEN])
{
	MemCpy(mRxKey, rxKey, sizeof(mRxKey));
	MemCpy(mTxKey, txKey, sizeof(mTxKey));
	mRxSeq = 0;
	mTxSeq = 0;
	mbReady = true;
}

void ChannelCipher::Clear()
{
	sodium_memzero(mRxKey, sizeof(mRxKey));
	sodium_memzero(mTxKey, sizeof(mTxKey));
	mRxSeq = 0;
	mTxSeq = 0;
	mbReady = false;
}

bool ChannelCipher::IsReady() const
{
	return mbReady;
}

void ChannelCipher::Seal(const void* plain, const size_t plainLen, std::string* outCipher)
{
	Assert(mbReady && outCipher != nullptr, "ChannelCipher::Seal: 키를 나누기 전이거나 outCipher가 null");
	uint8_t nonce[crypto_aead_xchacha20poly1305_ietf_NPUBBYTES];
	makeNonce(mTxSeq, nonce);
	++mTxSeq;
	outCipher->resize(plainLen + CHANNEL_TAG_LEN);
	unsigned long long cipherLen = 0;
	crypto_aead_xchacha20poly1305_ietf_encrypt(reinterpret_cast<unsigned char*>(outCipher->data()), &cipherLen, static_cast<const unsigned char*>(plain), plainLen,
		nullptr, 0, nullptr, nonce, mTxKey);
}

bool ChannelCipher::Open(const void* cipher, const size_t cipherLen, std::string* outPlain)
{
	Assert(outPlain != nullptr, "ChannelCipher::Open: outPlain이 null");
	if (!mbReady || cipherLen < CHANNEL_TAG_LEN)
	{
		return false; // 밖에서 온 길이 - 끊을 일이지 멈출 일이 아니다
	}
	uint8_t nonce[crypto_aead_xchacha20poly1305_ietf_NPUBBYTES];
	makeNonce(mRxSeq, nonce);
	outPlain->resize(cipherLen - CHANNEL_TAG_LEN);
	unsigned long long plainLen = 0;
	const bool bOk = crypto_aead_xchacha20poly1305_ietf_decrypt(reinterpret_cast<unsigned char*>(outPlain->data()), &plainLen, nullptr,
		static_cast<const unsigned char*>(cipher), cipherLen, nullptr, 0, nonce, mRxKey) == 0;
	if (bOk)
	{
		++mRxSeq; // 풀린 것만 센다 - 틀린 메시지 뒤에는 어차피 연결을 끊는다
	}
	return bOk;
}

bool HexToBytes(const std::string& hex, uint8_t* outBytes, const size_t size)
{
	Assert(outBytes != nullptr, "HexToBytes: outBytes가 null");
	size_t len = 0;
	return sodium_hex2bin(outBytes, size, hex.c_str(), hex.size(), nullptr, &len, nullptr) == 0 && len == size;
}
