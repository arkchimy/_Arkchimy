#pragma once
#include <WinSock2.h> // MSWSock.h보다 먼저 와야 한다
#include <MSWSock.h>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>
#include "Assert.h"
#include "Memory.h"
#include "PacketHeader.h"
#include "RingBuffer.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "Mswsock.lib")

// WSAStartup / WSACleanup RAII
class WsadataRAII
{
public:
	WsadataRAII();
	~WsadataRAII();

	bool IsOk() const
	{
		return mbOk;
	}

	int32_t GetErr() const // IsOk가 false일 때 WSAStartup의 에러 코드
	{
		return mErr;
	}

private:
	bool mbOk = false;
	int32_t mErr = 0;
};

enum class eIoType
{
	Accept,
	Recv,
	Send,
	Release, // PQCS로 넣는 세션 끊는 작업
};

// GQCS에서 받은 OVERLAPPED*를 static_cast로 되돌려 Type을 읽는다
struct MyOverlapped : OVERLAPPED
{
	eIoType Type;
};

// AcceptEx 슬롯 - 완료되면 같은 슬롯으로 다시 건다
struct AcceptOv : MyOverlapped
{
	SOCKET AcceptSocket = INVALID_SOCKET;
	char Buffer[64] = {}; // 로컬/원격 주소가 써지는 곳 (수신 데이터는 안 받음)
};

// 세션 버퍼 크기 - 상수
enum : int32_t
{
	RECV_RING_SIZE = 16384, // 수신 링 - 최대 패킷보다 커야 한다(static_assert). 완성된 패킷을 매번 다 꺼내므로 가득 차지 않는다
	SEND_RING_SIZE = 32768, // 송신 링 크기 = 송신 넘침 한도. 가득 차면 끊는다 - 받는 속도가 느린 클라, 또는 주변 세션이 너무 많이 보낼 때
};

// 세션 하나의 상태
// 라이브러리 안에서만 다룬다(컨텐츠는 세션 ID로만). 멤버는 NetworkLib이 friend로 직접 쓴다
class Session
{
	friend class NetworkLib;

private:
	SOCKET mSock = INVALID_SOCKET;
	int32_t mIdx = 0;   // 슬롯 번호 - 해제할 때 빈 슬롯 스택에 돌려준다
	uint64_t mID = 0;   // 세션 ID = (세대 << 32) | 슬롯 번호. 슬롯을 다시 쓸 때마다 세대가 올라 이전 ID와 겹치지 않는다
	uint32_t mIP = 0;   // 상대 IP (네트워크 바이트 순서 그대로)
	uint16_t mPort = 0; // 상대 포트 (호스트 바이트 순서)

	// 여러 스레드가 쓰는 변수는 volatile + Interlocked 함수로 쓴다(일반 대입·++ 금지) - CODING_STANDARD "스레드 공유 변수"
	volatile LONG mbDisconnect = 0; // 릴리즈 Flag 해제할 때 0으로 돌아감
	volatile LONG mbSendTarget = 0; // 모아보내는 Flag 1이면 어느 워커의 보낼 목록(tSendTargets)에 올라 있음
	bool mbSendFlag = false;        // WSASend Flag - 세션당 송신 1개.
	int64_t mWsaSendQPC = 0;        // 지금 걸린 송신을 건 시각 - 송신 완료까지 걸린 시간 측정과 송신 정체 판정용.

	MyOverlapped mRecvOv;
	MyOverlapped mSendOv;
	MyOverlapped mReleaseOv;

	RingBuffer<RECV_RING_SIZE> mRecvBuf; // 받은 바이트. 완성된 패킷만 꺼내고, 잘린 나머지는 다음 수신까지 남는다 (수신은 세션당 1건이라 락 없음)
	std::mutex mSendLock;                // mSendBuf, mbSendFlag 보호
	RingBuffer<SEND_RING_SIZE> mSendBuf; // 보낼 데이터(완료를 기다리는 몫 포함). 넣을 자리가 없으면 세션을 끊는다

	alignas(64) volatile LONG mIoCount = 0;
};

// 누적 카운터 스냅샷 - 1초 차분으로 초당 값을 만든다
struct NetStat
{
	int64_t AcceptCnt = 0;      // 세션을 만든 수
	int64_t ReleaseCnt = 0;     // 세션을 해제한 수 (현재 세션 수 = accept - release)
	int64_t OverflowCnt = 0;    // 송신 링이 가득 차 끊은 수
	int64_t SendStallCnt = 0;   // 걸린 WSASend가 기준 시간 넘게 안 끝나 끊은 수 - 받지 않거나 너무 느리게 받는 클라
	int64_t SessionFullCnt = 0; // 빈 세션 슬롯이 없어 받자마자 끊은 수
	int64_t BadPacketCnt = 0;   // 헤더 길이가 최대를 넘어 끊은 수
	int64_t RecvPacketCnt = 0;  // 받은 컨텐츠 패킷 수 - 처리량(패킷/초) 교차 확인용
	int64_t SendPacketCnt = 0;  // 송신 링에 넣은 컨텐츠 패킷 수. WSASend 1번당 메시지 수 = 이 값 / 송신 완료 수
	// 시스템 콜 효율 - 완료 1건(= GQCS 1번 + WSARecv/WSASend 1번)에 얼마나 많은 일이 실렸나
	int64_t RecvCompleteCnt = 0; // 데이터가 있는 수신 완료 수
	int64_t SendCompleteCnt = 0; // 성공한 송신 완료 수
	int64_t SendBytes = 0;       // 송신 완료로 보낸 바이트
	int64_t GqcsCallCnt = 0;     // GQCS(또는 GQCSEx) 호출 수
	int64_t IocpCompleteCnt = 0; // IOCP 큐에서 꺼낸 완료 수(해제 작업 포함) - GQCS 호출 1번에 꺼낸 완료 수 = 이 값 / GqcsCallCnt
	// WSASend/WSARecv 반환 결과 - 즉시 성공(0)인지 대기(WSA_IO_PENDING)인지, 각각 반환까지 걸린 시간(QPC 틱 합)
	int64_t SendSyncCnt = 0;
	int64_t SendPendingCnt = 0;
	int64_t SendSyncTicks = 0;
	int64_t SendPendingTicks = 0;
	int64_t RecvSyncCnt = 0;
	int64_t RecvPendingCnt = 0;
	int64_t RecvSyncTicks = 0;
	int64_t RecvPendingTicks = 0;
	int64_t SendWaitTicks = 0; // WSASend를 건 순간부터 그 송신 완료를 처리할 때까지(QPC 틱 합) - 성공한 송신 완료 수로 나눈다
};

// 워커별 카운터 - 핫패스에서 매 완료마다 올리므로 워커끼리 같은 캐시 라인을 쓰지 않게 64바이트 정렬
// 쓰는 워커는 하나라 Interlocked 대신 읽고 쓰기로 올린다(AddSingleWriter, lock 접두 명령 없음). 이름은 NetStat의 같은 항목과 맞춘다
struct alignas(64) WorkerCounter
{
	volatile LONG64 RecvPacketCnt = 0;
	volatile LONG64 SendPacketCnt = 0;
	volatile LONG64 RecvCompleteCnt = 0;
	volatile LONG64 SendCompleteCnt = 0;
	volatile LONG64 SendBytes = 0;
	volatile LONG64 GqcsCallCnt = 0;
	volatile LONG64 IocpCompleteCnt = 0;
	volatile LONG64 SendSyncCnt = 0;
	volatile LONG64 SendPendingCnt = 0;
	volatile LONG64 SendSyncTicks = 0;
	volatile LONG64 SendPendingTicks = 0;
	volatile LONG64 RecvSyncCnt = 0;
	volatile LONG64 RecvPendingCnt = 0;
	volatile LONG64 RecvSyncTicks = 0;
	volatile LONG64 RecvPendingTicks = 0;
	volatile LONG64 SendWaitTicks = 0;
};

// 단일 쓰기 카운터 증가 - 쓰는 스레드가 하나뿐이라 원자적 RMW(InterlockedAdd64) 대신 읽고 쓰기. 다른 스레드는 읽기만 한다
// x64에서 정렬된 8바이트 읽기·쓰기는 한 번에 일어나므로 읽는 쪽이 반쯤 바뀐 값을 보지 않는다
inline void AddSingleWriter(volatile LONG64& counter, const int64_t value)
{
	counter = counter + value;
}

// WSARecv/WSASend 반환 결과를 즉시 성공 / 대기로 나눠 센다. 즉시 실패(끊긴 소켓 등)는 세지 않는다
inline void CountIoResult(const int ret, const int err, const int64_t ticks,
	volatile LONG64* outSyncCnt, volatile LONG64* outSyncTicks,
	volatile LONG64* outPendingCnt, volatile LONG64* outPendingTicks)
{
	Assert(outSyncCnt != nullptr);
	Assert(outSyncTicks != nullptr);
	Assert(outPendingCnt != nullptr);
	Assert(outPendingTicks != nullptr);
	if (ret == 0)
	{
		AddSingleWriter(*outSyncCnt, 1);
		AddSingleWriter(*outSyncTicks, ticks);
	}
	else if (err == WSA_IO_PENDING)
	{
		AddSingleWriter(*outPendingCnt, 1);
		AddSingleWriter(*outPendingTicks, ticks);
	}
}

// 라이브러리 설정 - Start에 넘긴다. 기본값은 여기 한곳에 둔다
struct NetConfig
{
	USHORT Port = 0;
	int Backlog = 512;
	int WorkerCnt = 2;     // IOCP 워커 수
	int SlotCnt = 2;       // 걸어두는 AcceptEx 수
	int MaxSession = 1000; // 최대 동시 접속

	// 워커가 완료를 꺼내는 방식. true면 GQCSEx로 최대 GqcsBatch개씩, false면 GQCS로 1개씩
	// GQCS는 기본값을 정할 때 비교한 대상이라 같은 코드로 다시 잴 수 있게 남겨 둔다.
	bool bGqcsEx = true;
	int GqcsBatch = 64; // 크면 호출 수는 줄지만 한 워커가 몰아 처리하는 동안 다른 워커가 놀 수 있다. 1보다 작으면 Start가 1로 올린다

	// 완료 통지 skip 방식 (FILE_SKIP_COMPLETION_PORT_ON_SUCCESS)
	// 켜면 WSASend/WSARecv가 즉시 성공했을 때 완료 통지를 큐에 넣지 않고 건 스레드가 그 자리에서 처리한다. IFS가 아닌 LSP가 있으면 Start가 끈다
	bool bSkipOnSuccess = true;

	// 송신 합치기 범위 - 모아 보내기는 항상 켜져 있고, 이 설정은 "어디까지 모았다가 보내나"를 정한다
	// false: 수신 완료 하나를 처리할 때마다 보낸다.
	// true: 워커가 한 번에 꺼낸 완료(GQCSEx면 최대 GqcsBatch개)를 다 처리한 뒤 보낸다 
	bool bMergeSends = true;

	// 송신 정체 판정 기준(초). 0이면 끔(음수는 Start가 0으로). 켜면 감시 스레드가 1초마다 세션을 돌며, 
	// WSASend가 이 시간 넘게 안 끝난 세션을 끊는다
	int SendStallSec = 0;

	// 송신 버퍼 0 실험. 켜면 수락한 소켓마다 SO_SNDBUF = 0 (커널이 송신 버퍼로 복사하지 않고 송신이 비동기로 넘어간다)
	bool bSendBufZero = false;

	// [테스트 전용] 송신 완료를 처리하기 전에 ms만큼 잠든다 - 완료 처리 지연을 일부러 만들어 넘침 판정을 결정적으로 시험한다. 0이면 끔
	int TestSendCompleteDelayMs = 0;
};

// 라이브러리 오류 - 라이브러리는 출력하지 않는다. 실패한 단계와 코드만 남기고 출력은 쓰는 쪽이 정한다
struct NetError
{
	const char* Step = nullptr; // 실패한 단계("bind", "GQCS" 등 문자열 상수). nullptr이면 오류 없음
	int32_t Code = 0;           // WSAGetLastError 또는 GetLastError 값. 코드가 없는 오류면 0
};

// IOCP 네트워크 라이브러리 - 컨텐츠는 상속해서 On* 콜백을 구현하고, 세션은 ID로만 다룬다
// 콜백 규칙
// - 모든 콜백은 IOCP 워커 스레드에서 불린다. 서로 다른 세션의 콜백은 동시에 불릴 수 있다 (공유 데이터는 컨텐츠가 보호)
// - 한 세션 안에서는 OnAccept → OnRecv(0번 이상, 순서대로, 동시에 안 불림) → OnRelease(정확히 1번) 순서가 보장된다
// - OnRelease는 다른 콜백이나 SendPacket/Disconnect 안에서 재진입하지 않는다. 끝나면 그 ID는 무효
// 파생 클래스 소멸자에서 반드시 Stop()을 먼저 부를 것 - 기반 소멸자에서는 파생 부분이 이미 없어 OnRelease를 부를 수 없다
class NetworkLib
{
public:
	virtual ~NetworkLib();
	bool Start(const NetConfig& config); // 설정은 시작할 때 복사해 두고 실행 중에는 바꿀 수 없다
	void Stop();                         // AcceptEx 중단 → 세션 전부 끊기 → I/O 정리 대기 → 워커 종료. 워커/콜백 안에서 부르면 데드락

	bool IsRunning() const // false면 종료 요청(치명적 오류 포함) - main이 보고 Stop을 부른다
	{
		return mbRunning != 0;
	}

	NetStat GetStat() const; // 누적 카운터 읽기 (통계 출력용)

	// 처음 난 치명적 오류 - Start가 false를 돌려주거나 IsRunning이 false가 된 뒤 원인을 볼 때 쓴다. 뒤따르는 실패가 원인을 덮어쓰지 않게 첫 오류만 남긴다
	NetError GetNetError() const;

	const NetConfig& GetConfig() const // 실제 적용된 값(Start가 고친 값 포함) - Start가 끝난 뒤에 읽는다. IFS가 아닌 LSP가 있으면 bSkipOnSuccess가 꺼져 있다
	{
		return mConfig;
	}

	// 워커 스레드(OnAccept, OnRecv, OnRelease 안)에서만 호출 - 링에 쌓고, 처리가 끝날 때 받는 사람마다 WSASend 한 번으로 모아 보낸다. 헤더는 라이브러리가 붙인다
	// false: 워커가 아닌 스레드에서 부름(보내지 않는다), 이미 끊김/끊는 중, 길이 초과, 송신 링이 가득 참(이 경우 세션을 끊는다)
	bool SendPacket(const uint64_t id, const char* payload, const int32_t len);
	void Disconnect(const uint64_t id); // 끊기 요청 - 아무 스레드에서나 호출 가능. 걸린 I/O를 취소하고, 모두 끝나면 OnRelease

	// 세션 ID의 슬롯 번호(0 – maxSession-1) - 컨텐츠가 세션별 데이터를 배열로 둘 때 쓴다. 슬롯은 재사용되므로 OnAccept에서 초기화할 것
	static uint32_t GetIdx(const uint64_t id)
	{
		return static_cast<uint32_t>(id);
	}

protected:
	virtual void OnAccept(const uint64_t id) = 0;                                       // 연결 수락 직후, 첫 수신을 걸기 전. SendPacket 가능
	virtual void OnRecv(const uint64_t id, const char* payload, const int32_t len) = 0; // 완성된 패킷 1개 (헤더 제외). payload는 콜백 안에서만 유효
	virtual void OnRelease(const uint64_t id) = 0;                                      // 모든 I/O가 끝나 세션이 해제됨

private:
	bool initIOCP(const int workerCnt);                                                                 // IOCP 생성 → 워커 스레드 workerCnt개 시작
	void stopWorkers();                                                                                 // 워커 수만큼 종료 신호를 넣고 모두 join
	void runWorkerLoopGQCS(const int workerIdx);                                                        // GQCS로 완료를 1개씩 꺼내 처리. workerIdx: 워커별 카운터 번호
	void runWorkerLoopGqcsEx(const int workerIdx);                                                      // GQCSEx로 완료를 최대 GqcsBatch개씩 꺼내 처리
	void handleCompletion(MyOverlapped& ov, Session* session, const BOOL bOk, const DWORD transferred); // 완료 1건을 종류별 처리 함수로 넘긴다. session은 Accept면 nullptr
	bool postAccept(AcceptOv& ov);                                                                      // 기존 ov 슬롯을 재사용해 AcceptEx 하나 등록
	void onAcceptComplete(AcceptOv& ov, const BOOL bOk);                                                // 슬롯 재등록 → 세션 생성 → 수락 대기 수 -1
	void acceptSession(const SOCKET sock, const BOOL bOk, const uint32_t ip, const uint16_t port);      // 세션 생성 + OnAccept + 첫 수신. 실패하면 소켓을 닫는다
	Session* allocSession();                                                                            // 빈 슬롯 꺼내기 + 새 ID 발급, 가득 차면 nullptr
	void freeSession(const Session& session);                                                           // 슬롯 반환
	bool tryIncIoCount(Session& session);                                                               // 살아 있을 때만 I/O 카운트 +1 (해제를 막고 세션 사용)
	Session* findSession(const uint64_t id);                                                            // ID로 세션을 찾아 I/O 카운트 +1. 이미 끊겼거나 슬롯이 다른 세션에 재사용됐으면 nullptr. 다 쓰면 decIoCount
	// postRecv/postSend는 호출하는 쪽이 카운트를 잡고 있어야 한다 - 걸고 난 뒤 세션을 다시 읽기 때문
	void postRecv(Session& session);                                                 // 수신 링의 빈 공간에 수신 1건 등록 (끊는 중이면 안 건다). 즉시 완료(skip)면 그 자리에서 처리하고 다시 건다
	void onRecvComplete(Session& session, const BOOL bOk, const DWORD transferred);  // 완성된 패킷을 하나씩 OnRecv로 넘기고 다음 수신 등록
	bool completeRecv(Session& session, const DWORD transferred);                    // 받은 바이트에서 완성된 패킷을 꺼내 처리. 0바이트(상대가 끊음)면 false
	void postSend(Session& session);                                                 // 쌓인 데이터를 송신 1건으로 등록 (송신 중이거나 끊는 중이면 무시). 즉시 완료(skip)면 그 자리에서 처리하고 이어서 보낸다
	void onSendComplete(Session& session, const BOOL bOk, const DWORD transferred);  // 보낸 만큼 Pop, 이어서 송신
	void completeSend(Session& session, const DWORD transferred);                    // 성공한 송신 1건 마무리 - 보낸 만큼 Pop, 송신 플래그 내림
	bool marshalToSendBuf(Session& session, const char* payload, const int32_t len); // 헤더(길이)를 붙여 송신 링에 넣고 보낼 목록에 올림. 호출하는 쪽이 카운트를 잡고 있어야 한다
	void addSendTarget(Session& session);                                            // 보낼 목록에 올리기만 한다 (처음 올릴 때 카운트 +1)
	void flushSends();                                                               // 목록의 세션마다 송신 1건, 목록 몫 카운트 반납
	void requestDisconnect(Session& session);                                        // 끊기 플래그 + 걸린 I/O 취소. 카운트는 건드리지 않는다 - 호출하는 쪽이 카운트를 잡고 있어야 한다
	void decIoCount(Session& session);                                               // I/O 카운트 -1, 0이면 해제 작업을 IOCP에 넣는다
	void releaseSession(Session& session);                                           // (워커에서만) 소켓 닫기 + OnRelease + 버퍼 정리 + 슬롯 반환
	void runSendStallWatch();                                                        // 감시 스레드 - 1초마다 세션을 돌며 송신 정체 세션에 끊기 요청만 한다(해제는 워커)
	void stopSendStallWatch();                                                       // 감시 스레드를 깨워 멈추고 join, 이벤트 닫기
	void setNetError(const char* step, const int32_t code);                          // 첫 오류만 남긴다. step은 문자열 상수

	SOCKET mListenSock = INVALID_SOCKET;
	HANDLE mIOCP = nullptr;
	std::vector<std::thread> mWorkers;
	NetConfig mConfig; // Start에서만 쓴다 - 워커가 락 없이 읽는다
	NetError mNetError;
	mutable std::mutex mNetErrorLock; // 단계와 코드를 함께 읽고 쓴다 - 워커도 남기므로 필요하고, 오류 때만 잡아 핫패스 비용은 없다

	std::vector<AcceptOv> mAcceptOvs; // 고정 크기 - 완료되면 같은 슬롯 재사용
	std::vector<Session> mSessions;   // 고정 크기 - 시작할 때 maxSession개 생성
	std::vector<int32_t> mFreeIdx;    // 비어 있는 세션 슬롯 번호 - 끝에서만 넣고 뺀다(스택)
	std::mutex mFreeLock;
	std::mutex mStartStopLock;           // Start/Stop 전체를 감싼다 - 여러 스레드가 동시에 불러도 한 번에 하나만 진행
	volatile LONG mbRunning = 0;         // 1이면 실행 중. 워커가 치명적 오류로 0으로 내리면 main이 보고 Stop을 부른다
	volatile LONG mPendingAcceptCnt = 0; // 걸려 있는 AcceptEx 수 - 종료 시 0이 될 때까지 기다린다

	std::vector<UniquePtr<WorkerCounter>> mWorkerCounters; // 워커별 1개

	// 송신 정체 감시 - 워커와 독립된 스레드(워커가 밀려도 판정이 밀리지 않게)
	int64_t mQpcFreq = 0; // 기준 시간을 QPC 틱으로 바꿀 때 쓴다
	std::thread mWatchThread;
	HANDLE mWatchStopEvent = nullptr; // 멈춤 신호 - 감시 스레드가 1초씩 기다리다 이것이 켜지면 바로 끝난다(수동 리셋 이벤트)

	// 통계 카운터 - 연결 단위 사건만 센다 (바이트 단위는 워커 간 캐시 라인 경합이 핫패스에 붙어서 제외)
	volatile LONG64 mAcceptCnt = 0; // 여러 워커가 올리므로 InterlockedIncrement64
	volatile LONG64 mReleaseCnt = 0;
	volatile LONG64 mOverflowCnt = 0;
	volatile LONG64 mSendStallCnt = 0; // 감시 스레드만 올린다
	volatile LONG64 mSessionFullCnt = 0;
	volatile LONG64 mBadPacketCnt = 0;
};
