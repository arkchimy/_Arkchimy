#pragma once
#include <cstdint>

// 서버와 클라이언트가 같이 쓰는 패킷 헤더 - 라이브러리는 길이로 TCP 스트림을 패킷으로 자르기만 한다
// 패킷 종류(type)는 라이브러리가 몰라도 되도록 페이로드 안에 컨텐츠가 넣는다 (예: ChatProtocol의 첫 바이트)
// 예전에는 서버 핑 때문에 종류 1바이트(Data/Ping/Pong)가 있었으나, 지연 측정을 클라에 맡기면서 없앴다(2026-11-01)
#pragma pack(push, 1)
struct PacketHeader
{
	uint16_t len;	// 헤더를 뺀 페이로드 길이
};
#pragma pack(pop)
static_assert(sizeof(PacketHeader) == 2, "헤더는 패딩 없이 2바이트");

constexpr int32_t PACKET_HEADER_SIZE = sizeof(PacketHeader);
constexpr int32_t MAX_PAYLOAD_SIZE = 4096;	// 넘는 길이가 오면 비정상 클라이언트로 보고 끊는다
constexpr int32_t MAX_PACKET_SIZE = PACKET_HEADER_SIZE + MAX_PAYLOAD_SIZE;
