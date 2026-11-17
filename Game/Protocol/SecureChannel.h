#pragma once
#include <cstdint>
#include <string>
#include "LoginProtocol.h"

// 게임 구간 암호 통로 - 게임 서버와 클라·봇이 같이 쓴다(설계 login_server.docx 5-2). 키는 로그인 서버가 준 세션 키에서 만든다(SessionAuth)
// 로그인 구간은 RSA만 쓴다(LoginCrypto)
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

// 16진수 문자열 → 바이트(설정 파일의 키). 길이가 정확히 맞아야 true
bool HexToBytes(const std::string& hex, uint8_t* outBytes, const size_t size);
