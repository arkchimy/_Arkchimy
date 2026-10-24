#pragma once
#include <cstdint>

// 서버와 클라이언트가 같이 쓰는 패킷 헤더 - 라이브러리는 길이만 보고 TCP 스트림을 패킷으로 자른다
// 패킷 종류(type) 같은 컨텐츠 정보는 라이브러리가 몰라도 되도록 페이로드 안에 컨텐츠가 넣는다
struct PacketHeader
{
	uint16_t len;	// 헤더를 뺀 페이로드 길이
};
static_assert(sizeof(PacketHeader) == 2, "헤더는 패딩 없이 2바이트");

constexpr int32_t PACKET_HEADER_SIZE = sizeof(PacketHeader);
constexpr int32_t MAX_PAYLOAD_SIZE = 4096;	// 넘는 길이가 오면 비정상 클라이언트로 보고 끊는다
constexpr int32_t MAX_PACKET_SIZE = PACKET_HEADER_SIZE + MAX_PAYLOAD_SIZE;
