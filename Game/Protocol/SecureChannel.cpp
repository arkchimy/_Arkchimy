#include <Windows.h>
#include <bcrypt.h>
#include <sodium.h>
#include "SecureChannel.h"
#include "Memory.h"

#pragma comment(lib, "bcrypt.lib")

enum : int32_t
{
	CHANNEL_NONCE_LEN = 12, // AES-GCM 기본 논스 길이
};
static_assert(CHANNEL_KEY_LEN == 32, "통로 키 크기 = AES-256 키");
static_assert(CHANNEL_TAG_LEN == 16, "확인 값 크기 = GCM 태그");

// 알고리즘 핸들은 프로그램에서 한 번 연다(GCM 모드로) - 여러 스레드가 같이 써도 된다(CNG 문서)
class GcmProvider final
{
public:
	GcmProvider()
	{
		const bool bOk = BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&mAes, BCRYPT_AES_ALGORITHM, nullptr, 0))
			&& BCRYPT_SUCCESS(BCryptSetProperty(mAes, BCRYPT_CHAINING_MODE, reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),
				sizeof(BCRYPT_CHAIN_MODE_GCM), 0));
		Assert(bOk, "GcmProvider: Windows CNG의 AES-GCM을 열지 못함");
	}
	~GcmProvider()
	{
		BCryptCloseAlgorithmProvider(mAes, 0);
	}
	GcmProvider(const GcmProvider&) = delete;
	GcmProvider& operator=(const GcmProvider&) = delete;

	BCRYPT_ALG_HANDLE GetAes() const
	{
		return mAes;
	}

private:
	BCRYPT_ALG_HANDLE mAes = nullptr;
};

static const GcmProvider& getProvider()
{
	static const GcmProvider sProvider; // 처음 쓸 때 연다(여러 스레드가 동시에 불러도 한 번만)
	return sProvider;
}

// 키 32바이트 → CNG 키 핸들. 키 객체 버퍼를 넘기지 않으면 CNG가 할당한다(Windows 7부터)
static void* makeKey(const uint8_t (&key)[CHANNEL_KEY_LEN])
{
	BCRYPT_KEY_HANDLE handle = nullptr;
	const bool bOk = BCRYPT_SUCCESS(BCryptGenerateSymmetricKey(getProvider().GetAes(), &handle, nullptr, 0, const_cast<PUCHAR>(key), sizeof(key), 0));
	Assert(bOk, "ChannelCipher: AES 키 핸들을 만들지 못함");
	return handle;
}

static void destroyKey(void** key)
{
	if (*key != nullptr)
	{
		BCryptDestroyKey(static_cast<BCRYPT_KEY_HANDLE>(*key));
		*key = nullptr;
	}
}

// 순번을 논스 앞 8바이트에(나머지는 0) - 방향마다 키가 달라 두 방향이 같은 순번을 써도 겹치지 않는다
static void makeNonce(const uint64_t seq, uint8_t (&outNonce)[CHANNEL_NONCE_LEN])
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
	Clear();
	mRxKey = makeKey(rxKey);
	mTxKey = makeKey(txKey);
	mbReady = true;
}

void ChannelCipher::Clear()
{
	destroyKey(&mRxKey); // 키 값은 CNG 키 객체 안에만 있고, 지우면 CNG가 함께 지운다
	destroyKey(&mTxKey);
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
	uint8_t nonce[CHANNEL_NONCE_LEN];
	makeNonce(mTxSeq, nonce);
	++mTxSeq;
	outCipher->resize(plainLen + CHANNEL_TAG_LEN);
	PUCHAR const out = reinterpret_cast<PUCHAR>(outCipher->data());

	BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
	BCRYPT_INIT_AUTH_MODE_INFO(info);
	info.pbNonce = nonce;
	info.cbNonce = sizeof(nonce);
	info.pbTag = out + plainLen; // 확인 값은 암호문 바로 뒤
	info.cbTag = CHANNEL_TAG_LEN;
	ULONG written = 0;
	const bool bOk = BCRYPT_SUCCESS(BCryptEncrypt(static_cast<BCRYPT_KEY_HANDLE>(mTxKey), static_cast<PUCHAR>(const_cast<void*>(plain)), static_cast<ULONG>(plainLen),
		&info, nullptr, 0, out, static_cast<ULONG>(plainLen), &written, 0)) && written == plainLen;
	Assert(bOk, "ChannelCipher::Seal: AES-GCM 잠그기 실패(CNG)"); // 키와 길이가 맞으면 실패하지 않는다
}

bool ChannelCipher::Open(const void* cipher, const size_t cipherLen, std::string* outPlain)
{
	Assert(outPlain != nullptr, "ChannelCipher::Open: outPlain이 null");
	if (!mbReady || cipherLen < CHANNEL_TAG_LEN)
	{
		return false; // 밖에서 온 길이 - 끊을 일이지 멈출 일이 아니다
	}
	const size_t plainLen = cipherLen - CHANNEL_TAG_LEN;
	const uint8_t* const in = static_cast<const uint8_t*>(cipher);
	uint8_t nonce[CHANNEL_NONCE_LEN];
	makeNonce(mRxSeq, nonce);
	outPlain->resize(plainLen);

	BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
	BCRYPT_INIT_AUTH_MODE_INFO(info);
	info.pbNonce = nonce;
	info.cbNonce = sizeof(nonce);
	info.pbTag = const_cast<PUCHAR>(in + plainLen);
	info.cbTag = CHANNEL_TAG_LEN;
	ULONG written = 0;
	const bool bOk = BCRYPT_SUCCESS(BCryptDecrypt(static_cast<BCRYPT_KEY_HANDLE>(mRxKey), const_cast<PUCHAR>(in), static_cast<ULONG>(plainLen),
		&info, nullptr, 0, reinterpret_cast<PUCHAR>(outPlain->data()), static_cast<ULONG>(plainLen), &written, 0)) && written == plainLen;
	if (bOk)
	{
		++mRxSeq; // 풀린 것만 센다 - 틀린 메시지 뒤에는 어차피 연결을 끊는다
	}
	else
	{
		outPlain->clear(); // 확인 값이 틀리면 CNG가 평문을 지우지만 길이는 남는다
	}
	return bOk;
}

bool HexToBytes(const std::string& hex, uint8_t* outBytes, const size_t size)
{
	Assert(outBytes != nullptr, "HexToBytes: outBytes가 null");
	size_t len = 0;
	return sodium_hex2bin(outBytes, size, hex.c_str(), hex.size(), nullptr, &len, nullptr) == 0 && len == size;
}
