#pragma once
#include "NetworkLib.h"
#include "NetworkLibEx.h"

// 라이브러리 사용 예제 겸 검증용 컨텐츠 - 받은 패킷을 그대로 돌려보낸다 (EchoClient가 바이트 단위로 검증)
// 템플릿인 이유: 같은 에코 로직을 NetworkLib(GQCS)과 NetworkLibEx(GQCSEx) 위에 얹어 라이브러리만 바꿔 비교한다
template <typename Lib>
class EchoServerT : public Lib
{
public:
	// 기반 소멸자에서는 OnRelease(파생 함수)를 부를 수 없으므로 세션 정리를 여기서 끝낸다
	~EchoServerT() override
	{
		this->stop();
	}

protected:
	void OnAccept(uint64_t id) override
	{
		// 에코는 연결별 상태가 없다
	}

	// SendPacket이 헤더를 다시 붙이므로 클라이언트에는 받은 패킷과 같은 바이트가 돌아간다
	// 송신 링이 넘치면 SendPacket이 세션을 끊는다 - 여기서 따로 할 일은 없다
	void OnRecv(uint64_t id, const char* payload, int32_t len) override
	{
		this->SendPacket(id, payload, len);	// 템플릿 기반 클래스의 멤버는 this->로 불러야 찾는다
	}

	void OnRelease(uint64_t id) override
	{
	}
};

using EchoServer = EchoServerT<NetworkLib>;
using EchoServerEx = EchoServerT<NetworkLibEx>;
