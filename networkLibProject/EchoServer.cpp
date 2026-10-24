#include "EchoServer.h"

// 기반 소멸자에서는 OnRelease(파생 함수)를 부를 수 없으므로 세션 정리를 여기서 끝낸다
EchoServer::~EchoServer()
{
	stop();
}

void EchoServer::OnAccept(uint64_t id)
{
	// 에코는 연결별 상태가 없다
}

// SendPacket이 헤더를 다시 붙이므로 클라이언트에는 받은 패킷과 같은 바이트가 돌아간다
// 송신 링이 넘치면 SendPacket이 세션을 끊는다 - 여기서 따로 할 일은 없다
void EchoServer::OnRecv(uint64_t id, const char* payload, int32_t len)
{
	SendPacket(id, payload, len);
}

void EchoServer::OnRelease(uint64_t id)
{
}
