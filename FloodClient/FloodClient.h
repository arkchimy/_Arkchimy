#pragma once
#include <WinSock2.h>
#include <ws2tcpip.h>
#include <MSWSock.h>
#include <string>
#include <thread>
#include <vector>
#include <atomic>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "Mswsock.lib")

// ConnectEx(IOCP 비동기 connect)로 연결을 쏟아붓는 부하 클라이언트
// 완료통지로 성공/거절(10061)/기타에러를 나눠 세어 서버 backlog 넘침 지점을 찾는다
// SLOT_CNT개의 소켓을 동시에 걸어두고, 완료되면 즉시 closeReset 후 같은 슬롯으로 재등록

struct ConnectOv : OVERLAPPED
{
	SOCKET mSocket = INVALID_SOCKET;
};

class FloodClient
{
public:
	~FloodClient();
	bool init(const std::string& ip, USHORT port, int slotCnt, int workerCnt);
	void stop();

private:
	bool loadConnectEx();	// WSAIoctl로 ConnectEx 함수 포인터 획득 (1회)
	bool postConnect(ConnectOv* ov);	// 소켓 생성 → bind → IOCP 등록 → ConnectEx. 실패 시 running 동안 재시도
	void workerLoop();	// GQCS 루프, 완료통지 성공/실패 집계
	void statLoop();	// 1초마다 콘솔 출력

	sockaddr_in mTargetAddr{};
	LPFN_CONNECTEX mConnectEx = nullptr;
	HANDLE mIocp = nullptr;

	std::vector<ConnectOv> mConnectOvs;
	std::vector<std::thread> mWorkers;
	std::thread mStatThread;
	std::atomic<bool> mRunning = false;

	int mSlotCnt = 0;	// 동시에 걸어두는 ConnectEx 수 = 포트 점유 수
	long long mWorkerCnt = 0;
	std::atomic<long long> mCntIdx = 0;
	// 워커별 분리 집계 (false sharing은 신경 안 씀 - 측정값이라 핫패스 아님)
	std::vector<long long> mSuccess;	// ConnectEx 성공 완료
	std::vector<long long> mRefused;	// 10061 (backlog 넘침 또는 리슨 없음)
	std::vector<long long> mOther;	// 그 외 에러 (포트 고갈 등 클라 쪽 한계 포함)
	std::atomic<long> mLastOtherErr = 0;	// 기타 버킷에 마지막으로 들어온 에러코드 (진단용)
	std::atomic<long long> mSyncFail = 0;	// postConnect에서 ConnectEx 동기 즉시실패 횟수 (재시도로 숨던 것)
	std::atomic<long> mLastSyncErr = 0;	// 그 동기 실패의 마지막 에러코드
};
