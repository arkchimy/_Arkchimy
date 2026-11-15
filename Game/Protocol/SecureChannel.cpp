#include <sodium.h>
#include "SecureChannel.h"
#include "Memory.h"

static_assert(KX_PUBLIC_KEY_LEN == crypto_kx_PUBLICKEYBYTES && KX_PUBLIC_KEY_LEN == crypto_kx_SECRETKEYBYTES, "X25519 키 크기");
static_assert(CHANNEL_KEY_LEN == crypto_kx_SESSIONKEYBYTES && CHANNEL_KEY_LEN == crypto_aead_xchacha20poly1305_ietf_KEYBYTES, "통로 키 크기");
static_assert(SIGN_PUBLIC_KEY_LEN == crypto_sign_PUBLICKEYBYTES && SIGN_SECRET_KEY_LEN == crypto_sign_SECRETKEYBYTES && SIGN_LEN == crypto_sign_BYTES, "Ed25519 크기");
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

void MakeKxKeyPair(KxKeyPair* outPair)
{
	Assert(outPair != nullptr, "MakeKxKeyPair: outPair가 null");
	crypto_kx_keypair(outPair->PublicKey, outPair->SecretKey);
}

void WipeKxKeyPair(KxKeyPair* outPair)
{
	Assert(outPair != nullptr, "WipeKxKeyPair: outPair가 null");
	sodium_memzero(outPair, sizeof(*outPair));
}

// 서명할 내용 = 서버 임시 공개 키 + 클라 임시 공개 키 - 이번 연결의 클라 키에 묶여, 다른 연결의 ServerHello를 다시 보내도 통하지 않는다
static void makeSignedMessage(const uint8_t (&serverPublicKey)[KX_PUBLIC_KEY_LEN], const uint8_t (&clientPublicKey)[KX_PUBLIC_KEY_LEN],
	uint8_t (&outMessage)[KX_PUBLIC_KEY_LEN * 2])
{
	MemCpy(outMessage, serverPublicKey, KX_PUBLIC_KEY_LEN);
	MemCpy(outMessage + KX_PUBLIC_KEY_LEN, clientPublicKey, KX_PUBLIC_KEY_LEN);
}

bool AcceptClientHello(const ClientHello& hello, const uint8_t (&signSecretKey)[SIGN_SECRET_KEY_LEN], ServerHello* outHello, ChannelCipher* outCipher)
{
	Assert(outHello != nullptr && outCipher != nullptr, "AcceptClientHello: out 매개변수가 null");
	KxKeyPair mine;
	MakeKxKeyPair(&mine);
	uint8_t rx[CHANNEL_KEY_LEN];
	uint8_t tx[CHANNEL_KEY_LEN];
	const bool bOk = crypto_kx_server_session_keys(rx, tx, mine.PublicKey, mine.SecretKey, hello.ClientPublicKey) == 0;
	if (bOk)
	{
		outHello->Type = eLoginType::ServerHello;
		MemCpy(outHello->ServerPublicKey, mine.PublicKey, KX_PUBLIC_KEY_LEN);
		uint8_t message[KX_PUBLIC_KEY_LEN * 2];
		makeSignedMessage(mine.PublicKey, hello.ClientPublicKey, message);
		crypto_sign_detached(outHello->Signature, nullptr, message, sizeof(message), signSecretKey);
		outCipher->SetKeys(rx, tx);
	}
	WipeKxKeyPair(&mine); // 서버 임시 비밀 키는 키를 나눈 뒤 바로 버린다
	sodium_memzero(rx, sizeof(rx));
	sodium_memzero(tx, sizeof(tx));
	return bOk;
}

bool FinishClientHandshake(const KxKeyPair& mine, const ServerHello& hello, const uint8_t (&serverSignPublicKey)[SIGN_PUBLIC_KEY_LEN], ChannelCipher* outCipher)
{
	Assert(outCipher != nullptr, "FinishClientHandshake: outCipher가 null");
	uint8_t message[KX_PUBLIC_KEY_LEN * 2];
	makeSignedMessage(hello.ServerPublicKey, mine.PublicKey, message);
	if (hello.Type != eLoginType::ServerHello || crypto_sign_verify_detached(hello.Signature, message, sizeof(message), serverSignPublicKey) != 0)
	{
		return false; // 서버 고정 키로 서명하지 않음 - 가짜 서버(중간자)
	}
	uint8_t rx[CHANNEL_KEY_LEN];
	uint8_t tx[CHANNEL_KEY_LEN];
	const bool bOk = crypto_kx_client_session_keys(rx, tx, mine.PublicKey, mine.SecretKey, hello.ServerPublicKey) == 0;
	if (bOk)
	{
		outCipher->SetKeys(rx, tx);
	}
	sodium_memzero(rx, sizeof(rx));
	sodium_memzero(tx, sizeof(tx));
	return bOk;
}

bool HexToBytes(const std::string& hex, uint8_t* outBytes, const size_t size)
{
	Assert(outBytes != nullptr, "HexToBytes: outBytes가 null");
	size_t len = 0;
	return sodium_hex2bin(outBytes, size, hex.c_str(), hex.size(), nullptr, &len, nullptr) == 0 && len == size;
}
