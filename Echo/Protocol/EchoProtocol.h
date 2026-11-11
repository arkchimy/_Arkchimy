#pragma once
#include <cstdint>

// 에코 서버 프로토콜 - 서버와 시험 클라가 같이 쓴다
// 종류는 라이브러리 헤더가 아니라 payload 맨 앞에 컨텐츠가 넣는다(라이브러리는 길이로 자르기만 한다)
enum class eEchoType : uint16_t
{
	EchoReq = 1, // 클라 → 서버
	EchoRes = 2, // 서버 → 클라
};

// 구조체를 그대로 보내므로 서버·클라의 배치가 같도록 패딩을 없앤다
#pragma pack(push, 1)
struct EchoReq
{
	eEchoType Type; // EchoReq
	uint64_t Value; // 클라가 정한 값 - 서버는 그대로 돌려준다
};

struct EchoRes
{
	eEchoType Type; // EchoRes
	uint64_t Value; // 요청의 Value
};
#pragma pack(pop)

static_assert(sizeof(EchoReq) == 10, "종류 2 + 값 8, 패딩 없음");
static_assert(sizeof(EchoRes) == 10, "종류 2 + 값 8, 패딩 없음");
