#pragma once
#include <WinSock2.h>
#include <MSWSock.h>
#include "SendRingBuffer.h"
#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "Mswsock.lib")

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

// AcceptEx 슬롯 - 완료되면 같은 슬롯으로 다시 건다
struct AcceptOv : MyOverlapped
{
	SOCKET mAcceptSocket = INVALID_SOCKET;
	char mBuffer[64] = {};	// 로컬/원격 주소가 써지는 곳 (수신 데이터는 안 받음)
};

struct Session
{
	SOCKET mSock = INVALID_SOCKET;
	int32_t mIdx = 0;	// 슬롯 번호 - 해제할 때 빈 슬롯 스택에 돌려준다
	uint32_t mIp = 0;	// 상대 IP (네트워크 바이트 순서 그대로)
	uint16_t mPort = 0;	// 상대 포트 (호스트 바이트 순서)
	std::atomic<int32_t> mIoCount = 0;	// 걸려 있는 I/O 수. 0이 되는 순간 해제
	bool mSendFlag = false;	// WSASend가 걸려 있으면 true - 세션당 송신 1개. mSendLock 안에서만 읽고 쓴다
	MyOverlapped mRecvOv;
	MyOverlapped mSendOv;
	char mRecvBuf[4096];
	std::mutex mSendLock;	// mSendBuf 보호
	SendRingBuffer mSendBuf;	// 보낼 데이터. 가득 차면 세션을 끊는다
};

// 누적 카운터 스냅샷 - 1초 차분으로 초당 값을 만든다
struct NetStat
{
	int64_t acceptCnt = 0;	// 세션을 만든 수
	int64_t releaseCnt = 0;	// 세션을 해제한 수 (현재 세션 수 = accept - release)
	int64_t overflowCnt = 0;	// 송신 링이 가득 차서 끊은 수
	int64_t sessionFullCnt = 0;	// 빈 세션 슬롯이 없어 받자마자 끊은 수
};

class NetworkLib
{
public:
	~NetworkLib();
	// workerCnt: IOCP 워커 수, slotCnt: 걸어두는 AcceptEx 수, maxSession: 최대 동시 접속
	bool start(USHORT port, int backlog, int workerCnt, int slotCnt, int maxSession);
	void stop();	// AcceptEx 중단 → 세션 전부 끊기 → I/O 정리 대기 → 워커 종료
	bool isRunning() const { return mRunning; }	// false면 종료 요청(치명적 오류 포함) - main이 보고 stop을 부른다
	NetStat getStat() const;	// 누적 카운터 읽기 (통계 출력용)

private:
	bool initIocp(int workerCnt); 	// IOCP 생성 → 워커 스레드 workerCnt개 시작
	void stopWorkers(); 	// 워커 수만큼 종료 신호를 넣고 모두 join
	void workerLoop();
	bool postAccept(AcceptOv* ov);	// 기존 ov 슬롯을 재사용해 AcceptEx 하나 등록
	void onAcceptComplete(AcceptOv* ov, BOOL ok);	// 세션 생성 + 첫 수신, 슬롯 재등록
	Session* allocSession();	// 빈 슬롯 꺼내기, 가득 차면 nullptr
	void freeSession(Session* session);	// 슬롯 반환
	bool acquireSession(Session* session);	// 살아 있을 때만 I/O 카운트 +1 (해제를 막고 세션 사용)
	void postRecv(Session* session);	// 수신 1건 등록
	void onRecvComplete(Session* session, BOOL ok, DWORD transferred);	// 에코: 송신 링에 넣고 다음 수신 등록
	void postSend(Session* session);	// 쌓인 데이터를 송신 1건으로 등록 (송신 중이면 무시)
	void onSendComplete(Session* session, BOOL ok, DWORD transferred);	// 보낸 만큼 pop, 이어서 송신
	void decIoCount(Session* session);	// I/O 카운트 -1, 0이면 해제
	void releaseSession(Session* session);	// 소켓 닫기 + 송신 데이터 정리 + 슬롯 반환

	SOCKET mListenSock = INVALID_SOCKET;
	HANDLE mIocp = nullptr;
	std::vector<std::thread> mWorkers;

	std::vector<AcceptOv> mAcceptOvs;	// 고정 크기 - 완료되면 같은 슬롯 재사용
	std::vector<Session> mSessions;	// 고정 크기 - 시작할 때 maxSession개 생성
	std::vector<int32_t> mFreeIdx;	// 비어 있는 세션 슬롯 번호 - 끝에서만 넣고 뺀다(스택)
	std::mutex mFreeLock;
	std::atomic<bool> mRunning = false;
	std::atomic<int32_t> mPendingAccept = 0;	// 걸려 있는 AcceptEx 수 - 종료 시 0이 될 때까지 기다린다

	// 통계 카운터 - 연결 단위 사건만 센다 (바이트 단위는 워커 간 캐시 라인 경합이 핫패스에 붙어서 제외)
	std::atomic<int64_t> mAcceptCnt = 0;
	std::atomic<int64_t> mReleaseCnt = 0;
	std::atomic<int64_t> mOverflowCnt = 0;
	std::atomic<int64_t> mSessionFullCnt = 0;
};
