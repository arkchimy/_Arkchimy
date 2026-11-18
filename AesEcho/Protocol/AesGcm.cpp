#include <Windows.h>
#include <bcrypt.h>
#include "AesGcm.h"
#include "Memory.h"

#pragma comment(lib, "bcrypt.lib")

enum : uint32_t
{
	DIRECTION_CLIENT = 1, // 클라 → 서버
	DIRECTION_SERVER = 2, // 서버 → 클라
};

// 알고리즘 핸들은 프로그램에서 한 번 연다(GCM 모드로) - 여러 스레드가 같이 써도 된다(CNG 문서)
class AesProvider final
{
public:
	AesProvider()
	{
		const bool bOk = BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&mAes, BCRYPT_AES_ALGORITHM, nullptr, 0))
			&& BCRYPT_SUCCESS(BCryptSetProperty(mAes, BCRYPT_CHAINING_MODE, reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),
				sizeof(BCRYPT_CHAIN_MODE_GCM), 0));
		Assert(bOk, "AesProvider: Windows CNG의 AES-GCM을 열지 못함");
	}
	~AesProvider()
	{
		BCryptCloseAlgorithmProvider(mAes, 0);
	}
	AesProvider(const AesProvider&) = delete;
	AesProvider& operator=(const AesProvider&) = delete;

	BCRYPT_ALG_HANDLE GetAes() const
	{
		return mAes;
	}

private:
	BCRYPT_ALG_HANDLE mAes = nullptr;
};

static const AesProvider& getProvider()
{
	static const AesProvider sProvider; // 처음 쓸 때 연다(여러 스레드가 동시에 불러도 한 번만)
	return sProvider;
}

AesChannel::~AesChannel()
{
	Clear();
}

bool AesChannel::Start(const uint8_t (&key)[AES_KEY_LEN], const bool bServer)
{
	Clear();
	BCRYPT_KEY_HANDLE handle = nullptr;
	// 키 객체 버퍼를 넘기지 않으면 CNG가 할당한다(Windows 7부터)
	if (!BCRYPT_SUCCESS(BCryptGenerateSymmetricKey(getProvider().GetAes(), &handle, nullptr, 0, const_cast<PUCHAR>(key), sizeof(key), 0)))
	{
		return false;
	}
	mKey = handle;
	mbServer = bServer;
	mTxSeq = 0;
	mRxSeq = 0;
	return true;
}

void AesChannel::makeNonce(const bool bFromServer, const uint64_t seq, uint8_t (&outNonce)[AES_NONCE_LEN]) const
{
	const uint32_t direction = bFromServer ? DIRECTION_SERVER : DIRECTION_CLIENT;
	MemCpy(outNonce, &direction, sizeof(direction));
	MemCpy(outNonce + sizeof(direction), &seq, sizeof(seq));
}

bool AesChannel::Seal(const void* plain, const size_t plainLen, std::string* outPacket)
{
	Assert(outPacket != nullptr, "AesChannel::Seal: outPacket이 null");
	if (mKey == nullptr || plainLen == 0 || plainLen > AES_MAX_PLAIN_LEN)
	{
		return false;
	}
	uint8_t nonce[AES_NONCE_LEN];
	makeNonce(mbServer, mTxSeq, nonce);
	outPacket->resize(plainLen + AES_TAG_LEN);
	PUCHAR const out = reinterpret_cast<PUCHAR>(outPacket->data());

	BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
	BCRYPT_INIT_AUTH_MODE_INFO(info);
	info.pbNonce = nonce;
	info.cbNonce = sizeof(nonce);
	info.pbTag = out + plainLen; // 태그는 암호문 바로 뒤
	info.cbTag = AES_TAG_LEN;
	ULONG written = 0;
	const bool bOk = BCRYPT_SUCCESS(BCryptEncrypt(static_cast<BCRYPT_KEY_HANDLE>(mKey), static_cast<PUCHAR>(const_cast<void*>(plain)), static_cast<ULONG>(plainLen),
		&info, nullptr, 0, out, static_cast<ULONG>(plainLen), &written, 0)) && written == plainLen;
	if (!bOk)
	{
		return false;
	}
	++mTxSeq;
	return true;
}

bool AesChannel::Open(const void* packet, const size_t packetLen, std::string* outPlain)
{
	Assert(outPlain != nullptr, "AesChannel::Open: outPlain이 null");
	if (mKey == nullptr || packetLen <= AES_TAG_LEN || packetLen > AES_MAX_PLAIN_LEN + AES_TAG_LEN)
	{
		return false;
	}
	const size_t cipherLen = packetLen - AES_TAG_LEN;
	const uint8_t* const in = static_cast<const uint8_t*>(packet);
	uint8_t nonce[AES_NONCE_LEN];
	makeNonce(!mbServer, mRxSeq, nonce); // 상대 방향의 번호
	outPlain->resize(cipherLen);

	BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
	BCRYPT_INIT_AUTH_MODE_INFO(info);
	info.pbNonce = nonce;
	info.cbNonce = sizeof(nonce);
	info.pbTag = const_cast<PUCHAR>(in + cipherLen);
	info.cbTag = AES_TAG_LEN;
	ULONG written = 0;
	const bool bOk = BCRYPT_SUCCESS(BCryptDecrypt(static_cast<BCRYPT_KEY_HANDLE>(mKey), const_cast<PUCHAR>(in), static_cast<ULONG>(cipherLen),
		&info, nullptr, 0, reinterpret_cast<PUCHAR>(outPlain->data()), static_cast<ULONG>(cipherLen), &written, 0)) && written == cipherLen;
	if (!bOk)
	{
		outPlain->clear(); // 태그가 틀리면 CNG가 평문을 지우지만 길이는 남는다
		return false;
	}
	++mRxSeq;
	return true;
}

bool AesChannel::IsReady() const
{
	return mKey != nullptr;
}

void AesChannel::Clear()
{
	if (mKey != nullptr)
	{
		BCryptDestroyKey(static_cast<BCRYPT_KEY_HANDLE>(mKey));
		mKey = nullptr;
	}
	mTxSeq = 0;
	mRxSeq = 0;
}
