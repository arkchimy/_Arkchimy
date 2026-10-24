#pragma once
#include "NetworkLib.h"

// 라이브러리 사용 예제 겸 검증용 컨텐츠 - 받은 패킷을 그대로 돌려보낸다 (EchoClient가 바이트 단위로 검증)
class EchoServer : public NetworkLib
{
public:
	~EchoServer() override;

protected:
	void OnAccept(uint64_t id) override;
	void OnRecv(uint64_t id, const char* payload, int32_t len) override;
	void OnRelease(uint64_t id) override;
};
