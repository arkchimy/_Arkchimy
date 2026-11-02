#pragma once
#include <WinSock2.h>

// SO_LINGER(0)로 즉시 RST를 보내고 닫는다 - TIME_WAIT 없이 바로 정리
void CloseReset(const SOCKET sock);

// TCP 소켓이 IFS 핸들인지 - 아니면(IFS가 아닌 LSP가 끼어 있음) FILE_SKIP_COMPLETION_PORT_ON_SUCCESS를 걸면 완료를 놓칠 수 있다
bool IsIfsTcpSocket();
