#include <Windows.h>
#include <bcrypt.h>
#include "LoginCrypto.h"
#include "Memory.h"

#pragma comment(lib, "bcrypt.lib") // Windows에 기본으로 들어 있다 - 따로 설치할 것이 없다

// RSA 에코 서버만 켠다(RsaEchoServer.vcxproj) - 로그인 서버·클라·봇은 ProfilerLib를 링크하지 않는다
#ifdef RSA_ECHO_PROFILE
#include "Profiler.h"
#define CRYPTO_PROFILE_SCOPE(name) ProfileScope cryptoScope(name)
#else
#define CRYPTO_PROFILE_SCOPE(name)
#endif

static_assert(RSA_MODULUS_LEN * 8 == RSA_KEY_BITS && RSA_SIGN_LEN == RSA_MODULUS_LEN, "RSA 2048: n·서명 모두 256바이트");

enum : int32_t
{
	SHA256_LEN = 32,
	PUBLIC_EXPONENT_LEN = 3, // 65537 = 01 00 01
};

static const uint8_t PUBLIC_EXPONENT[PUBLIC_EXPONENT_LEN] = { 0x01, 0x00, 0x01 };

// 알고리즘 핸들은 프로그램에서 한 번 연다 - 여러 스레드가 같이 써도 된다(CNG 문서)
class CngProviders final
{
public:
	CngProviders()
	{
		const bool bOk = BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&mRsa, BCRYPT_RSA_ALGORITHM, nullptr, 0))
			&& BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&mSha256, BCRYPT_SHA256_ALGORITHM, nullptr, 0));
		Assert(bOk, "CngProviders: Windows CNG의 RSA·SHA-256을 열지 못함");
	}
	~CngProviders()
	{
		BCryptCloseAlgorithmProvider(mRsa, 0);
		BCryptCloseAlgorithmProvider(mSha256, 0);
	}
	CngProviders(const CngProviders&) = delete;
	CngProviders& operator=(const CngProviders&) = delete;

	BCRYPT_ALG_HANDLE GetRsa() const
	{
		return mRsa;
	}
	BCRYPT_ALG_HANDLE GetSha256() const
	{
		return mSha256;
	}

private:
	BCRYPT_ALG_HANDLE mRsa = nullptr;
	BCRYPT_ALG_HANDLE mSha256 = nullptr;
};

static const CngProviders& getProviders()
{
	static const CngProviders sProviders; // 처음 쓸 때 연다(C++11부터 여러 스레드가 동시에 불러도 한 번만)
	return sProviders;
}

static bool hashSha256(const void* message, const size_t messageLen, uint8_t (&outHash)[SHA256_LEN])
{
	return BCRYPT_SUCCESS(BCryptHash(getProviders().GetSha256(), nullptr, 0, static_cast<PUCHAR>(const_cast<void*>(message)), static_cast<ULONG>(messageLen),
		outHash, sizeof(outHash)));
}

RsaKey::~RsaKey()
{
	Clear();
}

bool RsaKey::Generate()
{
	std::lock_guard<std::mutex> lock(mLock);
	if (mKey != nullptr)
	{
		BCryptDestroyKey(mKey);
		mKey = nullptr;
	}
	BCRYPT_KEY_HANDLE key = nullptr;
	if (!BCRYPT_SUCCESS(BCryptGenerateKeyPair(getProviders().GetRsa(), &key, RSA_KEY_BITS, 0)))
	{
		return false;
	}
	if (!BCRYPT_SUCCESS(BCryptFinalizeKeyPair(key, 0))) // 공개 지수는 CNG 기본값 65537(ExportPublic이 확인한다)
	{
		BCryptDestroyKey(key);
		return false;
	}
	mKey = key;
	return true;
}

bool RsaKey::ImportPublic(const uint8_t (&modulus)[RSA_MODULUS_LEN])
{
	// CNG 공개 키 형식 = 머리(BCRYPT_RSAKEY_BLOB) + e + n(큰 쪽부터)
	uint8_t blob[sizeof(BCRYPT_RSAKEY_BLOB) + PUBLIC_EXPONENT_LEN + RSA_MODULUS_LEN];
	BCRYPT_RSAKEY_BLOB header{};
	header.Magic = BCRYPT_RSAPUBLIC_MAGIC;
	header.BitLength = RSA_KEY_BITS;
	header.cbPublicExp = PUBLIC_EXPONENT_LEN;
	header.cbModulus = RSA_MODULUS_LEN;
	MemCpy(blob, &header, sizeof(header));
	MemCpy(blob + sizeof(header), PUBLIC_EXPONENT, PUBLIC_EXPONENT_LEN);
	MemCpy(blob + sizeof(header) + PUBLIC_EXPONENT_LEN, modulus, RSA_MODULUS_LEN);
	std::lock_guard<std::mutex> lock(mLock);
	if (mKey != nullptr)
	{
		BCryptDestroyKey(mKey);
		mKey = nullptr;
	}
	if ((modulus[0] & 0x80) == 0 || (modulus[RSA_MODULUS_LEN - 1] & 0x01) == 0)
	{
		return false; // 2048비트 n은 맨 위 비트가 1이고 홀수다 - 밖에서 온 값이라 끊을 일
	}
	BCRYPT_KEY_HANDLE key = nullptr;
	const bool bOk = BCRYPT_SUCCESS(BCryptImportKeyPair(getProviders().GetRsa(), nullptr, BCRYPT_RSAPUBLIC_BLOB, &key, blob, sizeof(blob), 0));
	if (bOk)
	{
		mKey = key;
	}
	return bOk;
}

bool RsaKey::ImportPrivateBlob(const std::string& blob)
{
	std::lock_guard<std::mutex> lock(mLock);
	if (mKey != nullptr)
	{
		BCryptDestroyKey(mKey);
		mKey = nullptr;
	}
	BCRYPT_KEY_HANDLE key = nullptr;
	if (blob.size() < sizeof(BCRYPT_RSAKEY_BLOB)
		|| !BCRYPT_SUCCESS(BCryptImportKeyPair(getProviders().GetRsa(), nullptr, BCRYPT_RSAFULLPRIVATE_BLOB, &key,
			reinterpret_cast<PUCHAR>(const_cast<char*>(blob.data())), static_cast<ULONG>(blob.size()), 0)))
	{
		return false;
	}
	mKey = key;
	return true;
}

bool RsaKey::ExportPublic(uint8_t (&outModulus)[RSA_MODULUS_LEN]) const
{
	uint8_t blob[sizeof(BCRYPT_RSAKEY_BLOB) + 16 + RSA_MODULUS_LEN] = {};
	ULONG size = 0;
	{
		std::lock_guard<std::mutex> lock(mLock);
		if (mKey == nullptr || !BCRYPT_SUCCESS(BCryptExportKey(mKey, nullptr, BCRYPT_RSAPUBLIC_BLOB, blob, sizeof(blob), &size, 0)))
		{
			return false;
		}
	}
	BCRYPT_RSAKEY_BLOB header;
	MemCpy(&header, blob, sizeof(header));
	if (header.BitLength != RSA_KEY_BITS || header.cbModulus != RSA_MODULUS_LEN || header.cbPublicExp != PUBLIC_EXPONENT_LEN
		|| size != sizeof(header) + PUBLIC_EXPONENT_LEN + RSA_MODULUS_LEN || memcmp(blob + sizeof(header), PUBLIC_EXPONENT, PUBLIC_EXPONENT_LEN) != 0)
	{
		return false; // 주고받는 것은 n뿐이라 e가 65537이 아니면 상대가 다른 키로 읽는다
	}
	MemCpy(outModulus, blob + sizeof(header) + PUBLIC_EXPONENT_LEN, RSA_MODULUS_LEN);
	return true;
}

bool RsaKey::ExportPrivateBlob(std::string* outBlob) const
{
	Assert(outBlob != nullptr, "RsaKey::ExportPrivateBlob: outBlob이 null");
	std::lock_guard<std::mutex> lock(mLock);
	ULONG size = 0;
	if (mKey == nullptr || !BCRYPT_SUCCESS(BCryptExportKey(mKey, nullptr, BCRYPT_RSAFULLPRIVATE_BLOB, nullptr, 0, &size, 0)))
	{
		return false;
	}
	outBlob->resize(size);
	return BCRYPT_SUCCESS(BCryptExportKey(mKey, nullptr, BCRYPT_RSAFULLPRIVATE_BLOB, reinterpret_cast<PUCHAR>(outBlob->data()), size, &size, 0));
}

void RsaKey::Clear()
{
	std::lock_guard<std::mutex> lock(mLock);
	if (mKey != nullptr)
	{
		BCryptDestroyKey(mKey); // 임시 비밀 키는 여기서 사라진다 - 나중에 서버 고정 키가 새도 지난 연결을 풀 수 없다
		mKey = nullptr;
	}
}

bool RsaKey::IsReady() const
{
	std::lock_guard<std::mutex> lock(mLock);
	return mKey != nullptr;
}

bool RsaKey::Lock(const void* plain, const size_t plainLen, uint8_t (&outLocked)[RSA_MODULUS_LEN]) const
{
	if (plainLen > RSA_MAX_PLAIN_LEN)
	{
		return false;
	}
	BCRYPT_OAEP_PADDING_INFO oaep{ BCRYPT_SHA256_ALGORITHM, nullptr, 0 };
	ULONG size = 0;
	std::lock_guard<std::mutex> lock(mLock);
	return mKey != nullptr
		&& BCRYPT_SUCCESS(BCryptEncrypt(mKey, static_cast<PUCHAR>(const_cast<void*>(plain)), static_cast<ULONG>(plainLen), &oaep, nullptr, 0,
			outLocked, sizeof(outLocked), &size, BCRYPT_PAD_OAEP))
		&& size == sizeof(outLocked);
}

bool RsaKey::Unlock(const uint8_t (&locked)[RSA_MODULUS_LEN], std::string* outPlain) const
{
	Assert(outPlain != nullptr, "RsaKey::Unlock: outPlain이 null");
	BCRYPT_OAEP_PADDING_INFO oaep{ BCRYPT_SHA256_ALGORITHM, nullptr, 0 };
	uint8_t plain[RSA_MODULUS_LEN];
	ULONG size = 0;
	bool bOk = false;
	{
		std::lock_guard<std::mutex> lock(mLock);
		bOk = mKey != nullptr
			&& BCRYPT_SUCCESS(BCryptDecrypt(mKey, const_cast<PUCHAR>(locked), sizeof(locked), &oaep, nullptr, 0, plain, sizeof(plain), &size, BCRYPT_PAD_OAEP));
	}
	if (bOk)
	{
		outPlain->assign(reinterpret_cast<const char*>(plain), size);
	}
	SecureZeroMemory(plain, sizeof(plain)); // 비밀번호·세션 키가 들어 있다
	return bOk;
}

bool RsaKey::Sign(const void* message, const size_t messageLen, uint8_t (&outSign)[RSA_SIGN_LEN]) const
{
	uint8_t hash[SHA256_LEN];
	if (!hashSha256(message, messageLen, hash)) // 서명은 내용을 SHA-256으로 줄인 값에 - 내용이 n보다 길어도 된다
	{
		return false;
	}
	BCRYPT_PSS_PADDING_INFO pss{ BCRYPT_SHA256_ALGORITHM, SHA256_LEN };
	ULONG size = 0;
	// 락 없이 서명한다(2026-11-18 사용자 결정) - 서버 고정 키는 모든 세션이 함께 써서 락이면 워커끼리 기다린다(30초: 서명 1.04 → 0.78ms).
	// CNG 문서에 같은 키 핸들의 동시 서명을 보장하는 문장은 없다. 근거는 시험: 워커 2개, 1시간 서명 3,837,100번, 클라 확인 실패 0
	// 조건: 서명하는 동안 키를 바꾸지 않는다(Generate·Import·Clear는 서명하는 스레드가 없을 때만)
	return mKey != nullptr && BCRYPT_SUCCESS(BCryptSignHash(mKey, &pss, hash, sizeof(hash), outSign, sizeof(outSign), &size, BCRYPT_PAD_PSS)) && size == sizeof(outSign);
}

bool RsaKey::Verify(const void* message, const size_t messageLen, const uint8_t (&sign)[RSA_SIGN_LEN]) const
{
	uint8_t hash[SHA256_LEN];
	if (!hashSha256(message, messageLen, hash))
	{
		return false;
	}
	BCRYPT_PSS_PADDING_INFO pss{ BCRYPT_SHA256_ALGORITHM, SHA256_LEN };
	std::lock_guard<std::mutex> lock(mLock);
	return mKey != nullptr && BCRYPT_SUCCESS(BCryptVerifySignature(mKey, &pss, hash, sizeof(hash), const_cast<PUCHAR>(sign), sizeof(sign), BCRYPT_PAD_PSS));
}

bool LoginChannel::StartClient(HelloServer* outHello)
{
	Assert(outHello != nullptr, "LoginChannel::StartClient: outHello가 null");
	Clear();
	mbServer = false;
	outHello->Type = eLoginType::HelloServer;
	mMine = MakeUnique<RsaKey>();
	if (!mMine->Generate() || !mMine->ExportPublic(mClientPublicKey))
	{
		return false;
	}
	MemCpy(outHello->ClientPublicKey, mClientPublicKey, RSA_MODULUS_LEN);
	return true;
}

bool LoginChannel::FinishClient(const HelloClient& hello, const uint8_t (&serverSignPublicKey)[RSA_MODULUS_LEN])
{
	if (hello.Type != eLoginType::HelloClient || !mPinned.ImportPublic(serverSignPublicKey))
	{
		return false;
	}
	std::string message(reinterpret_cast<const char*>(hello.ServerPublicKey), RSA_MODULUS_LEN);
	message.append(reinterpret_cast<const char*>(mClientPublicKey), RSA_MODULUS_LEN);
	if (!mPinned.Verify(message.data(), message.size(), hello.Signature))
	{
		return false; // 서버 고정 키로 서명하지 않음 - 가짜 서버(중간자)
	}
	if (!mPeer.ImportPublic(hello.ServerPublicKey))
	{
		return false;
	}
	mServerSign = &mPinned;
	return true;
}

bool LoginChannel::AcceptClient(const HelloServer& hello, const RsaKey* signKey, UniquePtr<RsaKey> serverKey, HelloClient* outHello)
{
	Assert(signKey != nullptr && serverKey != nullptr && outHello != nullptr, "LoginChannel::AcceptClient: signKey·serverKey·outHello가 null");
	Clear();
	mbServer = true;
	mMine = std::move(serverKey); // 실패해도 이 연결이 가져간다 - Clear에서 지운다(다른 연결에 다시 쓰지 않는다)
	if (hello.Type != eLoginType::HelloServer || !mPeer.ImportPublic(hello.ClientPublicKey))
	{
		return false; // 밖에서 온 값 - 잘못된 공개 키
	}
	MemCpy(mClientPublicKey, hello.ClientPublicKey, RSA_MODULUS_LEN);
	outHello->Type = eLoginType::HelloClient;
	if (!mMine->ExportPublic(outHello->ServerPublicKey))
	{
		return false;
	}
	std::string message(reinterpret_cast<const char*>(outHello->ServerPublicKey), RSA_MODULUS_LEN);
	message.append(reinterpret_cast<const char*>(mClientPublicKey), RSA_MODULUS_LEN);
	if (!signKey->Sign(message.data(), message.size(), outHello->Signature))
	{
		return false;
	}
	mServerSign = signKey;
	return true;
}

void LoginChannel::makeSignedMessage(const uint8_t (&locked)[RSA_MODULUS_LEN], const uint64_t seq, const bool bFromServer, std::string* outMessage) const
{
	Assert(outMessage != nullptr, "LoginChannel::makeSignedMessage: outMessage가 null");
	// 요청 = [잠근 요청 | 번호], 응답 = [잠근 응답 | 클라 임시 공개 키 | 번호] - 보내지 않고 양쪽이 각자 만든다
	outMessage->assign(reinterpret_cast<const char*>(locked), RSA_MODULUS_LEN);
	if (bFromServer)
	{
		outMessage->append(reinterpret_cast<const char*>(mClientPublicKey), RSA_MODULUS_LEN);
	}
	outMessage->append(reinterpret_cast<const char*>(&seq), sizeof(seq));
}

bool LoginChannel::Seal(const void* plain, const size_t plainLen, std::string* outPacket)
{
	Assert(outPacket != nullptr && mServerSign != nullptr, "LoginChannel::Seal: 키를 나누기 전이거나 outPacket이 null");
	uint8_t locked[RSA_MODULUS_LEN];
	bool bLocked = false;
	{
		CRYPTO_PROFILE_SCOPE("Response encrypt: OAEP"); // 상대 임시 공개 키로 잠그기(공개 키 연산)
		bLocked = mPeer.Lock(plain, plainLen, locked);
	}
	if (!bLocked)
	{
		return false;
	}
	std::string message;
	makeSignedMessage(locked, mTxSeq, mbServer, &message);
	uint8_t sign[RSA_SIGN_LEN];
	const RsaKey& signer = mbServer ? *mServerSign : *mMine; // 응답은 서버 고정 키, 요청은 클라 임시 키로 서명
	bool bSigned = false;
	{
		CRYPTO_PROFILE_SCOPE("Response encrypt: sign"); // 비밀 키 서명 - 서버는 서버 고정 키(락 없이 모든 워커가 함께)
		bSigned = signer.Sign(message.data(), message.size(), sign);
	}
	if (!bSigned)
	{
		return false;
	}
	outPacket->assign(reinterpret_cast<const char*>(locked), RSA_MODULUS_LEN);
	outPacket->append(reinterpret_cast<const char*>(sign), RSA_SIGN_LEN);
	++mTxSeq;
	return true;
}

bool LoginChannel::Open(const void* packet, const size_t packetLen, std::string* outPlain)
{
	Assert(outPlain != nullptr, "LoginChannel::Open: outPlain이 null");
	if (mServerSign == nullptr || packetLen != LOGIN_SEALED_LEN)
	{
		return false; // 밖에서 온 길이 - 끊을 일이지 멈출 일이 아니다
	}
	uint8_t locked[RSA_MODULUS_LEN];
	uint8_t sign[RSA_SIGN_LEN];
	MemCpy(locked, packet, RSA_MODULUS_LEN);
	MemCpy(sign, static_cast<const char*>(packet) + RSA_MODULUS_LEN, RSA_SIGN_LEN);
	std::string message;
	makeSignedMessage(locked, mRxSeq, !mbServer, &message);
	const RsaKey& verifier = mbServer ? mPeer : *mServerSign; // 요청은 클라 임시 공개 키, 응답은 서버 고정 공개 키로 확인
	bool bVerified = false;
	{
		CRYPTO_PROFILE_SCOPE("Request decrypt: verify"); // 서명 확인(공개 키 연산) - 서버는 클라 임시 공개 키
		bVerified = verifier.Verify(message.data(), message.size(), sign);
	}
	if (!bVerified)
	{
		return false; // 확인이 틀리면 풀지 않는다
	}
	bool bUnlocked = false;
	{
		CRYPTO_PROFILE_SCOPE("Request decrypt: OAEP"); // OAEP 풀기(비밀 키 연산) - 서버는 서버 임시 비밀 키
		bUnlocked = mMine->Unlock(locked, outPlain);
	}
	if (!bUnlocked)
	{
		return false;
	}
	++mRxSeq; // 풀린 것만 센다 - 틀린 메시지 뒤에는 어차피 연결을 끊는다
	return true;
}

bool LoginChannel::IsReady() const
{
	return mServerSign != nullptr;
}

UniquePtr<RsaKey> LoginChannel::ReleaseMine()
{
	mServerSign = nullptr; // 키가 없는 통로로 Seal·Open하지 않게 - IsReady도 false가 된다
	return std::move(mMine);
}

void LoginChannel::Clear()
{
	mMine.reset(); // 임시 비밀 키는 여기서 사라진다(RsaKey 소멸자가 CNG 키를 지운다)
	mPeer.Clear();
	mPinned.Clear();
	mServerSign = nullptr;
	SecureZeroMemory(mClientPublicKey, sizeof(mClientPublicKey));
	mTxSeq = 0;
	mRxSeq = 0;
}
