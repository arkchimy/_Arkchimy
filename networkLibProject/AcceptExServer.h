#pragma once
#include <WinSock2.h>
#include <MSWSock.h>
#include <thread>
#include <vector>
#include <atomic>
#pragma comment(lib, "Mswsock.lib")

struct AcceptOv : OVERLAPPED
{
	SOCKET mAcceptSocket = INVALID_SOCKET;
	char mBuffer[64] = {};	// AcceptEx가 로컬/원격 주소를 써넣는 버퍼 (수신 데이터는 안 받음)
};

// AcceptEx(IOCP 비동기 accept)로 연결을 받는 실험용 서버
// 세션/Recv/Send 없음 - accept 처리량(TPS) 측정 전용
class AcceptExServer
{
public:
	~AcceptExServer();
	bool init(USHORT port, int backlog, int workerCnt, int slotCnt);	// workerCnt: IOCP 워커 스레드 수, slotCnt: 동시에 걸어두는 AcceptEx 수
	void stop();

private:
	bool postAccept(AcceptOv* ov);	// 기존 ov 슬롯을 재사용해 AcceptEx 하나 등록
	void workerLoop();	// GQCS 루프, Accept 완료 처리
	void statLoop();	// 1초마다 TPS 콘솔 출력

	SOCKET mListenSock = INVALID_SOCKET;
	HANDLE mIocp = nullptr;
	std::vector<AcceptOv> mAcceptOvs;	// 고정 크기 풀 - 완료되면 같은 슬롯을 즉시 재사용
	std::vector<std::thread> mWorkers;
	std::thread mStatThread;
	std::atomic<bool> mRunning = false;
    std::vector<long long> mAcceptCount;
    std::atomic<long long> mCntIdx = 0;
    long long mWorkerCnt = 0;
};
