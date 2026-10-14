#pragma once
#include <WinSock2.h>
#include <thread>
#include <vector>
#pragma comment(lib, "ws2_32.lib")

// WSAStartup / WSACleanup RAII

struct WsadataRAII
{
	WsadataRAII();
	~WsadataRAII();
	bool mOk = false;
};

enum class eIoType
{
	Accept,
	Recv,
	Send,
};

// GQCS에서 받은 OVERLAPPED*를 static_cast로 되돌려 type을 읽는다
// WSARecv/WSASend에는 항상 MyOverlapped만 넘길 것
struct MyOverlapped : OVERLAPPED
{
	eIoType type;
};

class NetworkLib
{
public:
	~NetworkLib();
	bool initListenSocket(USHORT port, int backlog); 	// 리슨 소켓 생성 → bind → listen
	bool initIocp(int workerCnt); 	// IOCP 생성 → 워커 스레드 workerCnt개 시작

	void stopWorkers(); 	// 워커 수만큼 종료 신호를 넣고 모두 join
private:
	void workerLoop();

	SOCKET mListenSock = INVALID_SOCKET;
	HANDLE mIocp = nullptr;
	std::vector<std::thread> mWorkers;
};
