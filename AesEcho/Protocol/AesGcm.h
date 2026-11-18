#pragma once
#include <cstdint>
#include <string>

// AES-256-GCM 통로 하나 - Windows CNG(bcrypt). RSA(LoginCrypto)와 같은 라이브러리라 따로 설치할 것이 없다
// 논스 12바이트 = [방향 4바이트 | 번호 8바이트]. 번호는 방향마다 0부터 하나씩 늘리고 보내지 않는다
// (RSA 통로의 번호처럼 - 바꾸거나 앞 패킷을 다시 보내면 태그가 맞지 않는다). 방향이 논스에 들어가 한 키를 두 방향이 써도 논스가 겹치지 않는다
// 보내기(Seal)와 받기(Open)는 번호가 따로라 서로 다른 스레드가 해도 된다. 같은 방향을 두 스레드가 동시에 하지 않는다

enum : int32_t
{
	AES_KEY_LEN = 32,      // AES-256
	AES_NONCE_LEN = 12,
	AES_TAG_LEN = 16,
	AES_MAX_PLAIN_LEN = 1024,
};

class AesChannel final
{
public:
	AesChannel() = default;
	AesChannel(const AesChannel&) = delete; // 핸들이 둘이 되면 한쪽이 지운 뒤 다른 쪽이 쓴다
	AesChannel& operator=(const AesChannel&) = delete;
	~AesChannel();

	bool Start(const uint8_t (&key)[AES_KEY_LEN], const bool bServer); // 번호를 0으로
	bool Seal(const void* plain, const size_t plainLen, std::string* outPacket);  // 평문 → [암호문 | 태그]
	bool Open(const void* packet, const size_t packetLen, std::string* outPlain); // 태그·번호가 틀리면 false - 그 연결은 끊는다
	bool IsReady() const;
	void Clear();

private:
	void makeNonce(const bool bFromServer, const uint64_t seq, uint8_t (&outNonce)[AES_NONCE_LEN]) const;

	void* mKey = nullptr; // BCRYPT_KEY_HANDLE - 헤더에서 Windows.h를 끌어오지 않으려고(README_ERRORS 1-26)
	bool mbServer = false;
	uint64_t mTxSeq = 0;
	uint64_t mRxSeq = 0;
};
