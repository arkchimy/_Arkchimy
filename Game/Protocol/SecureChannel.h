#pragma once
#include <cstdint>
#include <string>
#include "LoginProtocol.h"

// 암호 통로 - 로그인 서버와 클라가 같이 쓴다(설계 login_server.docx 5-1)
// 키 나누기: 양쪽이 연결마다 임시 X25519 키 쌍을 만들어 공개 키만 주고받고(crypto_kx) 방향별 키 두 개를 각자 계산한다.
//            서버는 자기 임시 공개 키에 고정 Ed25519 키로 서명하고, 클라는 미리 가진 서버 공개 키로 확인한다(중간자 막기).
//            임시 키는 연결이 끝나면 버리므로 나중에 고정 키가 새도 지난 연결은 풀 수 없다(전방 비밀성)
// 메시지: XChaCha20-Poly1305. 논스 = 방향별 순번(0, 1, 2…)이라 보내지 않고, 받는 쪽은 순번이 하나씩 늘지 않으면 풀리지 않는다(다시 보내기 막기)
// 한 객체를 두 스레드가 동시에 쓰지 않는다 - 보내는 쪽(Seal)은 부르는 쪽이 락으로 묶는다
class ChannelCipher final
{
public:
	ChannelCipher() = default;
	ChannelCipher(const ChannelCipher&) = delete;            // 복사하면 키·순번이 둘이 되어 같은 (키, 논스)로 두 번 암호화한다
	ChannelCipher& operator=(const ChannelCipher&) = delete;
	~ChannelCipher();

	void SetKeys(const uint8_t (&rxKey)[CHANNEL_KEY_LEN], const uint8_t (&txKey)[CHANNEL_KEY_LEN]); // 순번은 0부터
	void Clear();
	bool IsReady() const;

	// 평문 → 암호문(평문 길이 + CHANNEL_TAG_LEN). 보낼 때마다 순번 +1
	void Seal(const void* plain, const size_t plainLen, std::string* outCipher);
	// 암호문 → 평문. 확인 값이 틀리거나(변조·순번 어긋남) 너무 짧으면 false - 그 연결은 끊는다
	bool Open(const void* cipher, const size_t cipherLen, std::string* outPlain);

private:
	uint8_t mRxKey[CHANNEL_KEY_LEN] = {};
	uint8_t mTxKey[CHANNEL_KEY_LEN] = {};
	uint64_t mRxSeq = 0;
	uint64_t mTxSeq = 0;
	bool mbReady = false;
};

struct KxKeyPair
{
	uint8_t PublicKey[KX_PUBLIC_KEY_LEN];
	uint8_t SecretKey[KX_SECRET_KEY_LEN];
};

void MakeKxKeyPair(KxKeyPair* outPair); // 이번 연결용 임시 키 쌍
void WipeKxKeyPair(KxKeyPair* outPair);

// 서버: 클라 임시 공개 키를 받아 서버 임시 키를 만들고 서명한 ServerHello와 통로 키를 만든다. 클라 공개 키가 잘못이면 false
bool AcceptClientHello(const ClientHello& hello, const uint8_t (&signSecretKey)[SIGN_SECRET_KEY_LEN], ServerHello* outHello, ChannelCipher* outCipher);
// 클라: 서명을 미리 가진 서버 공개 키로 확인하고 통로 키를 만든다. 서명이 틀리면(가짜 서버) false
bool FinishClientHandshake(const KxKeyPair& mine, const ServerHello& hello, const uint8_t (&serverSignPublicKey)[SIGN_PUBLIC_KEY_LEN], ChannelCipher* outCipher);

// 16진수 문자열 → 바이트(설정 파일의 키). 길이가 정확히 맞아야 true
bool HexToBytes(const std::string& hex, uint8_t* outBytes, const size_t size);
