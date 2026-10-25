#pragma once
#include <cstdint>

// 서버와 클라이언트가 같이 쓰는 패킷 헤더 - 라이브러리는 길이로 TCP 스트림을 패킷으로 자른다
// kind는 라이브러리 자신의 제어 메시지(핑/퐁) 구분용이다
// 컨텐츠의 패킷 종류(type)는 라이브러리가 몰라도 되도록 Data 페이로드 안에 컨텐츠가 넣는다
enum class ePacketKind : uint8_t
{
	Data = 0,	// 컨텐츠 패킷 - OnRecv로 넘어간다
	Ping = 1,	// RTT 측정 - 받은 쪽은 페이로드를 그대로 Pong으로 돌려준다
	Pong = 2,
};

#pragma pack(push, 1)
struct PacketHeader
{
	uint16_t len;	// 헤더를 뺀 페이로드 길이
	ePacketKind kind;
};

// 핑 페이로드 - t0를 패킷에 실어 보내므로 보낸 쪽은 핑마다 상태를 둘 필요가 없다
struct PingPayload
{
	uint32_t seq;	// 세션마다 1씩 증가 - 오래되거나 조작된 퐁을 거른다
	int64_t t0;	// 보낸 쪽 QPC 값 - 같은 시계로 빼므로 상대와 시계 동기화가 필요 없다
};
#pragma pack(pop)
static_assert(sizeof(PacketHeader) == 3, "헤더는 패딩 없이 3바이트");
static_assert(sizeof(PingPayload) == 12, "핑 페이로드는 패딩 없이 12바이트");

constexpr int32_t PACKET_HEADER_SIZE = sizeof(PacketHeader);
constexpr int32_t MAX_PAYLOAD_SIZE = 4096;	// 넘는 길이가 오면 비정상 클라이언트로 보고 끊는다
constexpr int32_t MAX_PACKET_SIZE = PACKET_HEADER_SIZE + MAX_PAYLOAD_SIZE;
