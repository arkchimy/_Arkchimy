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

enum : uint32_t
{
	DIRECTION_CLIENT_TO_SERVER = 1,
	DIRECTION_SERVER_TO_CLIENT = 2,
};

// 논스 = [방향 값 4바이트 | 순번 8바이트] - 방향 값이 달라 두 방향이 같은 키·같은 순번을 써도 겹치지 않는다
static void makeNonce(const uint32_t direction, const uint64_t seq, uint8_t (&outNonce)[CHANNEL_NONCE_LEN])
{
	MemCpy(outNonce, &direction, sizeof(direction)); // 클라·서버 모두 x64(리틀 엔디언)
	MemCpy(outNonce + sizeof(direction), &seq, sizeof(seq));
}

ChannelCipher::~ChannelCipher()
{
	Clear();
}

void ChannelCipher::SetKey(const uint8_t (&key)[CHANNEL_KEY_LEN], const bool bServer, const uint64_t startSeq)
{
	Clear();
	mKey = makeKey(key);
	mTxDirection = bServer ? DIRECTION_SERVER_TO_CLIENT : DIRECTION_CLIENT_TO_SERVER;
	mRxDirection = bServer ? DIRECTION_CLIENT_TO_SERVER : DIRECTION_SERVER_TO_CLIENT;
	mTxSeq = startSeq; // 두 방향 모두 SeqNumber 시작값부터 - 방향 값이 달라 논스는 겹치지 않는다
	mRxSeq = startSeq;
}

void ChannelCipher::Clear()
{
	destroyKey(&mKey); // 키 값은 CNG 키 객체 안에만 있고, 지우면 CNG가 함께 지운다
	mTxDirection = 0;
	mRxDirection = 0;
	mTxSeq = 0;
	mRxSeq = 0;
}

bool ChannelCipher::IsReady() const
{
	return mKey != nullptr;
}

void ChannelCipher::SealPacket(const eGameType type, const void* body, const size_t bodyLen, std::string* outPacket)
{
	Assert(mKey != nullptr && outPacket != nullptr, "ChannelCipher::SealPacket: 키를 정하기 전이거나 outPacket이 null");
	const PacketHead head{ type, mTxSeq };
	uint8_t nonce[CHANNEL_NONCE_LEN];
	makeNonce(mTxDirection, mTxSeq, nonce);
	++mTxSeq; // 넘쳐도 0부터 이어진다 - 한 연결에서 2^64개를 보낼 일은 없다
	outPacket->resize(PACKET_HEAD_LEN + bodyLen + CHANNEL_TAG_LEN);
	PUCHAR const out = reinterpret_cast<PUCHAR>(outPacket->data());
	MemCpy(out, &head, PACKET_HEAD_LEN); // 평문 앞부분 - 태그 계산에 AD로 들어간다

	BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
	BCRYPT_INIT_AUTH_MODE_INFO(info);
	info.pbNonce = nonce;
	info.cbNonce = sizeof(nonce);
	info.pbAuthData = out; // AD = PacketHead(type | SeqNumber)
	info.cbAuthData = PACKET_HEAD_LEN;
	info.pbTag = out + PACKET_HEAD_LEN + bodyLen; // 태그는 암호문 바로 뒤
	info.cbTag = CHANNEL_TAG_LEN;
	ULONG written = 0;
	const bool bOk = BCRYPT_SUCCESS(BCryptEncrypt(static_cast<BCRYPT_KEY_HANDLE>(mKey), static_cast<PUCHAR>(const_cast<void*>(body)), static_cast<ULONG>(bodyLen),
		&info, nullptr, 0, out + PACKET_HEAD_LEN, static_cast<ULONG>(bodyLen), &written, 0)) && written == bodyLen;
	Assert(bOk, "ChannelCipher::SealPacket: AES-GCM 잠그기 실패(CNG)"); // 키와 길이가 맞으면 실패하지 않는다
}

bool ChannelCipher::OpenPacket(const void* packet, const size_t packetLen, eGameType* outType, std::string* outBody)
{
	Assert(outType != nullptr && outBody != nullptr, "ChannelCipher::OpenPacket: out 매개변수가 null");
	outBody->clear();
	if (mKey == nullptr || packetLen < static_cast<size_t>(SEALED_OVERHEAD))
	{
		return false; // 밖에서 온 길이 - 끊을 일이지 멈출 일이 아니다
	}
	const uint8_t* const in = static_cast<const uint8_t*>(packet);
	PacketHead head;
	MemCpy(&head, in, PACKET_HEAD_LEN);
	if (head.Seq != mRxSeq)
	{
		return false; // ① 기대한 SeqNumber가 아니다(다시 보내기·건너뛰기·다른 연결의 패킷) - 풀어 보지도 않는다
	}
	const size_t bodyLen = packetLen - SEALED_OVERHEAD;
	uint8_t nonce[CHANNEL_NONCE_LEN];
	makeNonce(mRxDirection, head.Seq, nonce);
	outBody->resize(bodyLen);

	BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
	BCRYPT_INIT_AUTH_MODE_INFO(info);
	info.pbNonce = nonce;
	info.cbNonce = sizeof(nonce);
	info.pbAuthData = const_cast<PUCHAR>(in); // AD = 받은 PacketHead 그대로 - type·SeqNumber를 바꿨으면 태그가 틀린다
	info.cbAuthData = PACKET_HEAD_LEN;
	info.pbTag = const_cast<PUCHAR>(in + PACKET_HEAD_LEN + bodyLen);
	info.cbTag = CHANNEL_TAG_LEN;
	ULONG written = 0;
	const bool bOk = BCRYPT_SUCCESS(BCryptDecrypt(static_cast<BCRYPT_KEY_HANDLE>(mKey), const_cast<PUCHAR>(in + PACKET_HEAD_LEN), static_cast<ULONG>(bodyLen),
		&info, nullptr, 0, reinterpret_cast<PUCHAR>(outBody->data()), static_cast<ULONG>(bodyLen), &written, 0)) && written == bodyLen;
	if (bOk)
	{
		++mRxSeq; // 풀린 것만 센다 - 틀린 메시지 뒤에는 어차피 연결을 끊는다
		*outType = head.Type;
	}
	else
	{
		outBody->clear(); // ② 태그가 틀림(키가 다름·변조·방향이 다름) - CNG가 평문을 지우지만 길이는 남는다
	}
	return bOk;
}

uint64_t MakeStartSeq()
{
	uint64_t seq = 0;
	const bool bOk = BCRYPT_SUCCESS(BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&seq), sizeof(seq), BCRYPT_USE_SYSTEM_PREFERRED_RNG));
	Assert(bOk, "MakeStartSeq: BCryptGenRandom 실패"); // 시스템 난수 생성기는 실패하지 않는다
	return seq;
}
bool HexToBytes(const std::string& hex, uint8_t* outBytes, const size_t size)
{
	Assert(outBytes != nullptr, "HexToBytes: outBytes가 null");
	size_t len = 0;
	return sodium_hex2bin(outBytes, size, hex.c_str(), hex.size(), nullptr, &len, nullptr) == 0 && len == size;
}
