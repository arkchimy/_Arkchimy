#pragma once
#include <WinSock2.h>
#include <MSWSock.h>
#include "PacketHeader.h"
#include "RingBuffer.h"
#include <atomic>
#include <cstdint>
#include <memory>
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
	Release,	// 실제 I/O가 아니라 PQCS로 넣는 해제 작업 - OnRelease를 워커에서 부르기 위해
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

// 송신 넘침 기준 - "아직 WSASend에 넘기지 않은 바이트"가 이것을 넘으면 상대가 받는 속도보다 빨리 쌓인 것
// 이미 넘긴 바이트는 세지 않는다: 대부분 상대에게 도착했고 완료 통지만 기다리는 중이라, 세면 서버의 완료 처리 지연까지 상대 탓이 된다
static constexpr int32_t SEND_PENDING_LIMIT = 16384;
static constexpr int32_t RECV_RING_SIZE = 16384;
// 넘긴 몫은 넘길 때의 안 넘긴 몫(≤ 한도)이었으므로 링은 한도의 2배면 넘긴 몫 + 안 넘긴 몫이 항상 들어간다
static constexpr int32_t SEND_RING_SIZE = SEND_PENDING_LIMIT * 2;

struct Session
{
	SOCKET mSock = INVALID_SOCKET;
	int32_t mIdx = 0;	// 슬롯 번호 - 해제할 때 빈 슬롯 스택에 돌려준다
	uint64_t mId = 0;	// 세션 ID = (세대 << 32) | 슬롯 번호. 슬롯을 다시 쓸 때마다 세대가 올라 이전 ID와 겹치지 않는다
	uint32_t mIp = 0;	// 상대 IP (네트워크 바이트 순서 그대로)
	uint16_t mPort = 0;	// 상대 포트 (호스트 바이트 순서)
	std::atomic<int32_t> mIoCount = 0;	// 걸려 있는 I/O 수 + 사용 중인 스레드 수. 0이 되는 순간 해제 작업을 넣는다
	std::atomic<bool> mDisconnect = false;	// 끊기 요청됨 - 새 I/O를 걸지 않는다. 해제할 때 false로 돌아감
	std::atomic<bool> mIsSendTarget = false;	// 어느 워커의 보낼 목록(tSendTargets)에 올라 있음 - 목록에 최대 한 번만 들어간다. 목록 몫의 카운트를 반납하기 전에 내리므로 해제 시점엔 항상 false
	bool mSendFlag = false;	// WSASend가 걸려 있으면 true - 세션당 송신 1개. mSendLock 안에서만 읽고 쓴다
	int64_t mWsaSendQpc = 0;	// 지금 걸린 송신을 건 시각 - 송신 완료를 처리할 때까지 걸린 시간 측정용 (송신은 세션당 1개라 하나로 충분)
	int32_t mSendingBytes = 0;	// 지금 걸린 WSASend에 넘긴 바이트 - 링 사용량 중 이미 커널에 넘긴 몫. mSendLock 안에서만 읽고 쓴다
	MyOverlapped mRecvOv;
	MyOverlapped mSendOv;
	MyOverlapped mReleaseOv;	// 해제 작업용 - 카운트가 0이 된 뒤에만 쓰므로 하나로 충분
	RingBuffer mRecvBuf{ RECV_RING_SIZE };	// 받은 바이트. 완성된 패킷만 꺼내고, 잘린 나머지는 다음 수신까지 남는다 (수신은 세션당 1건이라 락 없음)
	std::mutex mSendLock;	// mSendBuf, mSendFlag, mSendingBytes 보호
	RingBuffer mSendBuf{ SEND_RING_SIZE };	// 보낼 데이터. 안 넘긴 몫이 SEND_PENDING_LIMIT를 넘으면 세션을 끊는다
};

// 누적 카운터 스냅샷 - 1초 차분으로 초당 값을 만든다
struct NetStat
{
	int64_t acceptCnt = 0;	// 세션을 만든 수
	int64_t releaseCnt = 0;	// 세션을 해제한 수 (현재 세션 수 = accept - release)
	int64_t overflowCnt = 0;	// 송신 링이 가득 차서 끊은 수
	int64_t sessionFullCnt = 0;	// 빈 세션 슬롯이 없어 받자마자 끊은 수
	int64_t badPacketCnt = 0;	// 헤더 길이가 최대를 넘어 끊은 수
	int64_t recvPacketCnt = 0;	// 받은 컨텐츠 패킷 수 - 처리량(패킷/초) 교차 확인용
	int64_t sendPacketCnt = 0;	// 송신 링에 넣은 컨텐츠 패킷 수. WSASend 1번당 메시지 수 = 이 값 / 송신 완료 수
	// 시스템 콜 효율 - 완료 1건(= GQCS 1번 + WSARecv/WSASend 1번)에 얼마나 많은 일이 실렸나
	int64_t recvCompleteCnt = 0;	// 데이터가 있는 수신 완료 수
	int64_t sendCompleteCnt = 0;	// 성공한 송신 완료 수
	int64_t sendBytes = 0;	// 송신 완료로 보낸 바이트
	int64_t gqcsCallCnt = 0;	// GQCS(또는 GQCSEx) 호출 수
	int64_t iocpCompleteCnt = 0;	// IOCP 큐에서 꺼낸 완료 수(해제 작업 포함) - GQCS 호출 1번에 꺼낸 완료 수 = 이 값 / gqcsCallCnt
	// WSASend/WSARecv 반환 결과 - 즉시 성공(0)인지 대기(WSA_IO_PENDING)인지, 각각 반환까지 걸린 시간(QPC 틱 합)
	int64_t sendSyncCnt = 0;
	int64_t sendPendingCnt = 0;
	int64_t sendSyncTicks = 0;
	int64_t sendPendingTicks = 0;
	int64_t recvSyncCnt = 0;
	int64_t recvPendingCnt = 0;
	int64_t recvSyncTicks = 0;
	int64_t recvPendingTicks = 0;
	int64_t sendWaitTicks = 0;	// WSASend를 건 순간부터 그 송신 완료를 처리할 때까지(QPC 틱 합) - 성공한 송신 완료 수로 나눈다
};

// 워커별 카운터 - 핫패스에서 매 완료마다 올리므로 워커끼리 같은 캐시 라인을 쓰지 않게 64바이트 정렬
// 쓰는 워커는 하나라 load + store로 올린다 (lock 접두 명령 없음)
struct alignas(64) WorkerCounter
{
	std::atomic<int64_t> recvPacket = 0;
	std::atomic<int64_t> sendPacket = 0;
	std::atomic<int64_t> recvComplete = 0;
	std::atomic<int64_t> sendComplete = 0;
	std::atomic<int64_t> sendBytes = 0;
	std::atomic<int64_t> gqcsCall = 0;
	std::atomic<int64_t> iocpComplete = 0;
	std::atomic<int64_t> sendSync = 0;
	std::atomic<int64_t> sendPending = 0;
	std::atomic<int64_t> sendSyncTicks = 0;
	std::atomic<int64_t> sendPendingTicks = 0;
	std::atomic<int64_t> recvSync = 0;
	std::atomic<int64_t> recvPending = 0;
	std::atomic<int64_t> recvSyncTicks = 0;
	std::atomic<int64_t> recvPendingTicks = 0;
	std::atomic<int64_t> sendWaitTicks = 0;
};

// 단일 쓰기 카운터 증가 - 원자적 RMW(lock xadd) 대신 읽고 쓰기. 다른 스레드는 읽기만 한다
inline void addSingleWriter(std::atomic<int64_t>& counter, int64_t value)
{
	counter.store(counter.load(std::memory_order_relaxed) + value, std::memory_order_relaxed);
}

// WSARecv/WSASend 반환 결과를 즉시 성공 / 대기로 나눠 센다. 즉시 실패(끊긴 소켓 등)는 세지 않는다
inline void countIoResult(int ret, int err, int64_t ticks,
	std::atomic<int64_t>& syncCnt, std::atomic<int64_t>& syncTicks,
	std::atomic<int64_t>& pendingCnt, std::atomic<int64_t>& pendingTicks)
{
	if (ret == 0)
	{
		addSingleWriter(syncCnt, 1);
		addSingleWriter(syncTicks, ticks);
	}
	else if (err == WSA_IO_PENDING)
	{
		addSingleWriter(pendingCnt, 1);
		addSingleWriter(pendingTicks, ticks);
	}
}

// IOCP 네트워크 라이브러리 - 컨텐츠는 상속해서 On* 콜백을 구현하고, 세션은 ID로만 다룬다
// 콜백 규칙
// - 모든 콜백은 IOCP 워커 스레드에서 불린다. 서로 다른 세션의 콜백은 동시에 불릴 수 있다 (공유 데이터는 컨텐츠가 보호)
// - 한 세션 안에서는 OnAccept → OnRecv(0번 이상, 순서대로, 동시에 안 불림) → OnRelease(정확히 1번) 순서가 보장된다
// - OnRelease는 다른 콜백이나 SendPacket/Disconnect 안에서 재진입하지 않는다. 끝나면 그 ID는 무효
// 파생 클래스 소멸자에서 반드시 stop()을 먼저 부를 것 - 기반 소멸자에서는 파생 부분이 이미 없어 OnRelease를 부를 수 없다
class NetworkLib
{
public:
	virtual ~NetworkLib();
	// workerCnt: IOCP 워커 수, slotCnt: 걸어두는 AcceptEx 수, maxSession: 최대 동시 접속
	bool start(USHORT port, int backlog, int workerCnt, int slotCnt, int maxSession);
	void stop();	// AcceptEx 중단 → 세션 전부 끊기 → I/O 정리 대기 → 워커 종료. 워커/콜백 안에서 부르면 데드락
	bool isRunning() const { return mRunning; }	// false면 종료 요청(치명적 오류 포함) - main이 보고 stop을 부른다
	NetStat getStat() const;	// 누적 카운터 읽기 (통계 출력용)

	// 송신 버퍼 0 실험 - start 전에 부른다. 켜면 수락한 소켓마다 SO_SNDBUF = 0 (커널이 송신 버퍼로 복사하지 않고 송신이 비동기로 넘어간다)
	void setSendBufZero(bool on) { mSendBufZero = on; }
	// [테스트 전용] 송신 완료를 처리하기 전에 ms만큼 잠든다 - 완료 처리 지연을 일부러 만들어 넘침 판정을 결정적으로 시험한다. 0이면 끔
	void setTestSendCompleteDelayMs(int ms) { mTestSendCompleteDelayMs = ms; }
	// 완료 통지 skip 방식 - start 전에 부른다 (FILE_SKIP_COMPLETION_PORT_ON_SUCCESS)
	// 켜면 WSASend/WSARecv가 즉시 성공했을 때 완료 통지를 큐에 넣지 않고 건 스레드가 그 자리에서 처리한다. IFS가 아닌 LSP가 있으면 start가 끈다
	void setSkipOnSuccess(bool on) { mSkipOnSuccess = on; }
	bool getSkipOnSuccess() const { return mSkipOnSuccess; }
	// 수신 처리 중 송신을 보내는 경계 - start 전에 부른다. false면 수신 완료 1건 끝, true면 워커가 꺼낸 완료 묶음 끝(GQCSEx면 최대 gqcsBatch개)
	// 묶음이 길수록 여러 보낸 사람의 메시지가 받는 사람마다 한 번에 합쳐지지만, 받는 쪽의 안 넘긴 바이트가 더 오래 쌓인다
	void setFlushPerBatch(bool on) { mFlushPerBatch = on; }
	// 워커가 완료를 꺼내는 방식 - start 전에 부른다. true(기본)면 GQCSEx로 최대 batch개씩, false면 GQCS로 1개씩
	// GQCS는 기본값을 정할 때 비교한 대상이라 같은 코드로 다시 잴 수 있게 남겨 둔다. 워커 함수를 시작할 때 골라 완료마다 분기하지 않는다
	void setGqcsEx(bool on, int batch) { mGqcsEx = on; mGqcsBatch = (batch < 1) ? 1 : batch; }
	bool getGqcsEx() const { return mGqcsEx; }

	// OnRecv 안에서만 호출 - 그 수신 처리가 끝날 때 받는 사람마다 WSASend 한 번으로 모아 보낸다. 헤더는 라이브러리가 붙인다
	// false: OnRecv 밖에서 부름, 이미 끊김/끊는 중, 길이 초과, 송신 링 넘침(이 경우 세션을 끊는다)
	bool SendPacket(uint64_t id, const char* payload, int32_t len);
	void Disconnect(uint64_t id);	// 끊기 요청 - 아무 스레드에서나 호출 가능. 걸린 I/O를 취소하고, 모두 끝나면 OnRelease
	// 세션 ID의 슬롯 번호(0 ~ maxSession-1) - 컨텐츠가 세션별 데이터를 배열로 둘 때 쓴다. 슬롯은 재사용되므로 OnAccept에서 초기화할 것
	static uint32_t IdxOf(uint64_t id) { return static_cast<uint32_t>(id); }

protected:
	virtual void OnAccept(uint64_t id) = 0;	// 연결 수락 직후, 첫 수신을 걸기 전. SendPacket은 쓸 수 없다(OnRecv 안에서만)
	virtual void OnRecv(uint64_t id, const char* payload, int32_t len) = 0;	// 완성된 패킷 1개 (헤더 제외). payload는 콜백 안에서만 유효
	virtual void OnRelease(uint64_t id) = 0;	// 모든 I/O가 끝나 세션이 해제됨

private:
	bool initIocp(int workerCnt); 	// IOCP 생성 → 워커 스레드 workerCnt개 시작
	void stopWorkers(); 	// 워커 수만큼 종료 신호를 넣고 모두 join
	void workerLoopGqcs(int workerIdx);	// GQCS로 완료를 1개씩 꺼내 처리. workerIdx: 워커별 카운터 번호
	void workerLoopGqcsEx(int workerIdx);	// GQCSEx로 완료를 최대 mGqcsBatch개씩 꺼내 처리
	void handleCompletion(MyOverlapped* ov, Session* session, BOOL ok, DWORD transferred);	// 완료 1건을 종류별 처리 함수로 넘긴다
	bool postAccept(AcceptOv* ov);	// 기존 ov 슬롯을 재사용해 AcceptEx 하나 등록
	void onAcceptComplete(AcceptOv* ov, BOOL ok);	// 세션 생성 + 첫 수신, 슬롯 재등록
	Session* allocSession();	// 빈 슬롯 꺼내기 + 새 ID 발급, 가득 차면 nullptr
	void freeSession(Session* session);	// 슬롯 반환
	bool tryIncIoCount(Session* session);	// 살아 있을 때만 I/O 카운트 +1 (해제를 막고 세션 사용)
	Session* findSession(uint64_t id);	// ID로 세션을 찾아 I/O 카운트 +1. 이미 끊겼거나 슬롯이 다른 세션에 재사용됐으면 nullptr. 다 쓰면 decIoCount
	// postRecv/postSend는 호출하는 쪽이 카운트를 잡고 있어야 한다 - 걸고 난 뒤 세션을 다시 읽기 때문
	void postRecv(Session* session);	// 수신 링의 빈 공간에 수신 1건 등록 (끊는 중이면 안 건다). 즉시 완료(skip)면 그 자리에서 처리하고 다시 건다
	void onRecvComplete(Session* session, BOOL ok, DWORD transferred);	// 완성된 패킷을 하나씩 OnRecv로 넘기고 다음 수신 등록
	bool completeRecv(Session* session, DWORD transferred);	// 받은 바이트에서 완성된 패킷을 꺼내 처리. 0바이트(상대가 끊음)면 false
	void postSend(Session* session);	// 쌓인 데이터를 송신 1건으로 등록 (송신 중이거나 끊는 중이면 무시). 즉시 완료(skip)면 그 자리에서 처리하고 이어서 보낸다
	void onSendComplete(Session* session, BOOL ok, DWORD transferred);	// 보낸 만큼 pop, 이어서 송신
	void completeSend(Session* session, DWORD transferred);	// 성공한 송신 1건 마무리 - 보낸 만큼 pop, 송신 플래그 내림
	bool marshalToSendBuf(Session* session, const char* payload, int32_t len);	// 헤더(길이)를 붙여 송신 링에 넣고 송신. 호출하는 쪽이 카운트를 잡고 있어야 한다
	void addSendTarget(Session* session);	// 수신 처리 중의 송신 - 보낼 목록에 올리기만 한다 (처음 올릴 때 카운트 +1)
	void flushSends();	// 수신 처리 끝 - 목록의 세션마다 송신 1건, 목록 몫 카운트 반납
	void requestDisconnect(Session* session);	// 끊기 플래그 + 걸린 I/O 취소. 카운트는 건드리지 않는다
	void decIoCount(Session* session);	// I/O 카운트 -1, 0이면 해제 작업을 IOCP에 넣는다
	void releaseSession(Session* session);	// (워커에서만) 소켓 닫기 + OnRelease + 버퍼 정리 + 슬롯 반환

	SOCKET mListenSock = INVALID_SOCKET;
	HANDLE mIocp = nullptr;
	std::vector<std::thread> mWorkers;
	bool mGqcsEx = true;	// 워커가 GQCSEx로 꺼낼지 (false면 GQCS)
	int mGqcsBatch = 64;	// GQCSEx 한 번에 꺼낼 최대 완료 수 - 크면 호출 수는 줄지만 한 워커가 몰아 처리하는 동안 다른 워커가 놀 수 있다

	std::vector<AcceptOv> mAcceptOvs;	// 고정 크기 - 완료되면 같은 슬롯 재사용
	std::vector<Session> mSessions;	// 고정 크기 - 시작할 때 maxSession개 생성
	std::vector<int32_t> mFreeIdx;	// 비어 있는 세션 슬롯 번호 - 끝에서만 넣고 뺀다(스택)
	std::mutex mFreeLock;
	std::mutex mStartStopLock;	// start/stop 전체를 감싼다 - 여러 스레드가 동시에 불러도 한 번에 하나만 진행
	std::atomic<bool> mRunning = false;
	std::atomic<int32_t> mPendingAcceptCnt = 0;	// 걸려 있는 AcceptEx 수 - 종료 시 0이 될 때까지 기다린다

	bool mSendBufZero = false;	// 수락한 소켓에 SO_SNDBUF = 0을 걸지
	int mTestSendCompleteDelayMs = 0;	// [테스트 전용] 송신 완료 처리 전 지연
	bool mSkipOnSuccess = false;	// 수락한 소켓에 FILE_SKIP_COMPLETION_PORT_ON_SUCCESS를 걸지 - 모든 세션이 같은 값이어야 한다(즉시 완료 처리 경로가 이 값을 본다)
	bool mFlushPerBatch = false;	// 수신 처리 중 송신을 완료 묶음 끝에서 보낼지 (false면 수신 완료 1건 끝)
	std::vector<std::unique_ptr<WorkerCounter>> mWorkerCounters;	// 워커별 1개

	// 통계 카운터 - 연결 단위 사건만 센다 (바이트 단위는 워커 간 캐시 라인 경합이 핫패스에 붙어서 제외)
	std::atomic<int64_t> mAcceptCnt = 0;
	std::atomic<int64_t> mReleaseCnt = 0;
	std::atomic<int64_t> mOverflowCnt = 0;
	std::atomic<int64_t> mSessionFullCnt = 0;
	std::atomic<int64_t> mBadPacketCnt = 0;
};
