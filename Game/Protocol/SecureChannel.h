#pragma once
#include <cstdint>
#include <string>
#include "LoginProtocol.h" // CHANNEL_KEY_LEN, GameProtocol.h(PacketHead·eGameType)

// 게임 서버 ↔ 클라 통일 모양 패킷(설계 게임서버_입장_변경.docx 2판 3-1) - 게임 서버와 클라·봇이 같이 쓴다. 로그인 구간은 RSA만 쓴다(LoginCrypto)
// 패킷 = [PacketHead = type | SeqNumber](평문, AD) + AES-256-GCM 암호문(본문) + 태그 16바이트
// 논스 = [방향 값 4바이트 | SeqNumber 8바이트](클라 → 서버 1, 서버 → 클라 2). 키는 로그인 서버가 준 세션 키 그대로
// SeqNumber는 방향마다 따로 센다 - 둘 다 서버가 Challenge로 알려 준 SeqNumber 시작값부터, 메시지 하나에 1씩. 패킷에 적어 보내고(서버도, 사용자 결정)
// 받는 쪽은 적힌 값이 기대한 값이 아니면 풀지 않는다 - 다시 보내기·순서 바꾸기·다른 연결의 패킷(시작값이 다름)을 막는다
// 방향 값이 달라 두 방향이 같은 SeqNumber를 써도 (키, 논스)가 겹치지 않는다(같은 키로 같은 논스를 두 번 쓰지 않는다 - NIST SP 800-38D)
// 한 객체를 두 스레드가 동시에 쓰지 않는다 - 보내는 쪽(SealPacket)은 부르는 쪽이 락으로 묶는다. 받는 쪽(OpenPacket)은 한 스레드만
class ChannelCipher final
{
public:
	ChannelCipher() = default;
	ChannelCipher(const ChannelCipher&) = delete;            // 복사하면 키·순번이 둘이 되어 같은 (키, 논스)로 두 번 암호화한다
	ChannelCipher& operator=(const ChannelCipher&) = delete;
	~ChannelCipher();

	// bServer: 내가 서버면 보내는 방향 = 서버 → 클라, 받는 방향 = 클라 → 서버. 두 방향 모두 startSeq(SeqNumber 시작값)부터
	void SetKey(const uint8_t (&key)[CHANNEL_KEY_LEN], const bool bServer, const uint64_t startSeq);
	void Clear();
	bool IsReady() const;

	// 본문 → 통일 모양 패킷(PACKET_HEAD_LEN + bodyLen + CHANNEL_TAG_LEN). 보낼 때마다 보내는 SeqNumber +1
	void SealPacket(const eGameType type, const void* body, const size_t bodyLen, std::string* outPacket);
	// 통일 모양 패킷 → type·본문. ① 길이 ② SeqNumber = 받을 값 ③ 태그(AD = PacketHead) 중 하나라도 틀리면 false(받을 SeqNumber는 그대로) - 그 연결은 끊는다
	bool OpenPacket(const void* packet, const size_t packetLen, eGameType* outType, std::string* outBody);

private:
	void* mKey = nullptr;       // CNG 키 핸들(BCRYPT_KEY_HANDLE) - 헤더에 Windows.h를 넣지 않으려고 void*
	uint32_t mTxDirection = 0;  // 보내는 방향 값(논스 앞 4바이트)
	uint32_t mRxDirection = 0;  // 받는 방향 값
	uint64_t mTxSeq = 0;        // 다음에 보낼 SeqNumber
	uint64_t mRxSeq = 0;        // 다음에 받을 SeqNumber
};

// SeqNumber 시작값 - CNG BCryptGenRandom 무작위 8바이트(서버가 Hello를 받을 때 연결마다)
uint64_t MakeStartSeq();

// 16진수 문자열 → 바이트(설정 파일의 키). 길이가 정확히 맞아야 true
bool HexToBytes(const std::string& hex, uint8_t* outBytes, const size_t size);
