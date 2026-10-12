#pragma once
#include <WinSock2.h>
#pragma comment(lib, "ws2_32.lib")

// WSAStartup / WSACleanup 짝을 생성자/소멸자로 묶는다
// main 맨 앞에 지역변수로 두면 모든 소켓 코드보다 먼저 시작되고 가장 늦게 정리된다
struct WsadataRAII
{
	WsadataRAII();
	~WsadataRAII();
	bool mOk = false;
};

class NetworkLib
{
public:
	// 리슨 소켓 생성 → bind → listen
	bool initListenSocket(USHORT port, int backlog);

private:
	SOCKET mListenSock = INVALID_SOCKET;
};
