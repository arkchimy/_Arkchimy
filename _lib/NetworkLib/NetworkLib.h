#pragma once
#include <WinSock2.h>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>
#include "JobOverlapped.h"
#include "Memory.h"
#include "NetConfig.h"
#include "NetStat.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "Mswsock.lib")

class WsadataRAII
{
public:
	WsadataRAII();
	~WsadataRAII();
private:
	int32_t mErr = 0;
};

// 정의는 라이브러리 내부 헤더에 있다 - 서버는 세션을 ID로만 다루므로 이름만 알면 된다
class Session;
struct AcceptOv;
struct WorkerCounter;

// IOCP 네트워크 라이브러리 - 컨텐츠는 상속해서 On* 콜백을 구현하고, 세션은 ID로만 다룬다
// 콜백 규칙
// - 모든 콜백은 IOCP 워커 스레드에서 불린다.
// - 한 세션의 OnRecv는 순서대로, 동시에 불리지 않는다. 단 OnJobComplete는 같은 세션의 다른 콜백(OnAccept·OnRecv, 다른 OnJobComplete)과 동시에 불릴 수 있다 - 함께 쓰는 데이터는 컨텐츠가 보호한다
// - OnRelease는 그 세션의 마지막 콜백이다(걸린 작업이 있으면 OnJobComplete가 모두 끝난 뒤에 온다)
class NetworkLib
{
public:
	// 정의는 DLL에 둔다 
	// 암시적 생성자는 서버 쪽에서 만들어져 Session 정의까지 요구한다
	NetworkLib(); 
	virtual ~NetworkLib();
	bool Start(const NetConfig& config);
	void Stop();

	bool IsRunning() const // false면 종료 요청(치명적 오류 포함) - main이 보고 Stop을 부른다
	{
		return mbRunning != 0;
	}
	NetStat GetStat() const; // 누적 카운터 읽기 (통계 출력용)
	NetError GetNetError() const;

	const NetConfig& GetConfig() const 
	{
		// 실제 적용된 값(Start가 고친 값 포함) - Start가 끝난 뒤에 읽는다.
		// IFS가 아닌 LSP가 있으면 bSkipOnSuccess가 꺼져 있다
		return mConfig;
	}

	// 아무 스레드에서나 호출 가능 - 링에 쌓고, 워커가 받는 사람마다 WSASend 한 번으로 모아 보낸다. 
	// 헤더는 라이브러리가 붙인다
	// false: 이미 끊김/끊는 중, 길이 초과, 송신 링이 가득 참(이 경우 세션을 끊는다)
	bool SendPacket(const uint64_t id, const char* payload, const int32_t len);
	void Disconnect(const uint64_t id); // 끊기 요청 - 아무 스레드에서나 호출 가능. 걸린 I/O를 취소하고, 모두 끝나면 OnRelease

	// 워커를 막는 작업(DB 등)을 다른 스레드로 넘길 때 - 아무 스레드에서나 호출 가능
	// AcquireSession: ID를 확인하고 IOCount +1. 성공하면 그 작업의 OnJobComplete가 끝날 때까지 세션이 해제되지 않는다(끊겨도 OnRelease가 미뤄진다)
	//   false: 이미 해제됐거나 다른 세션으로 바뀐 ID, 또는 그 세션에 걸린 작업이 JobPerSession개 (같은 세션에 시도가 겹치면 한도 아래에서도 잠깐 false가 날 수 있다)
	//   끊기 요청은 받았지만 아직 해제되지 않은 세션에는 성공한다(해제 작업이 들어간 세션은 실패)
	//   그래서 OnJobComplete에서 작업을 계속 이어 걸면 해제와 Stop이 끝나지 않는다 - 이어 걸기는 컨텐츠가 하지 않는다
	// PostJobComplete: 성공한 AcquireSession마다 정확히 한 번 - 빠뜨리면 그 세션은 해제되지 않고 Stop이 끝나지 않는다
	//   ov는 컨텐츠의 작업 객체(JobOverlapped 상속)이고, 워커가 OnJobComplete로 돌려준다. 그때까지 지우면 안 된다
	//   Acquire보다 많이 부르거나 큐에 있는 ov를 다시 넣으면 IOCount가 어긋나므로 로그를 남기고 프로세스를 멈춘다(많이 부른 것은 그 세션의 다음 Post에서 드러날 수 있다)
	bool AcquireSession(const uint64_t id);
	void PostJobComplete(const uint64_t id, JobOverlapped* ov);

protected:
	// 연결 수락 직후, 첫 수신을 걸기 전. SendPacket 가능
	virtual void OnAccept(const uint64_t id) = 0;
	// 완성된 패킷 1개 (헤더 제외). payload는 콜백 안에서만 유효
	virtual void OnRecv(const uint64_t id, const char* payload, const int32_t len) = 0;
	// 모든 I/O가 끝나 세션이 해제됨
	virtual void OnRelease(const uint64_t id) = 0;
	// PostJobComplete로 넘긴 작업 객체 - static_cast로 컨텐츠의 작업 객체로 되돌린다. 이 안에서 지우거나 풀에 돌려줘도 된다
	// 걸린 작업 수는 PostJobComplete가 이미 내렸으므로 이 안에서 AcquireSession을 다시 걸 수 있다. 작업을 쓰지 않는 컨텐츠는 구현하지 않아도 된다
	virtual void OnJobComplete(const uint64_t id, JobOverlapped* ov);

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
	void postReleasePQCS(Session& session);
	bool sessionLock(Session& session);          														// 카운트 +1, 해제 표시가 서 있으면 실패(되돌리지 않음) 
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
	void requestSendToWorker(Session& session);                                      // (외부 스레드) 송신 권리를 잡고 워커에게 0바이트 송신 완료로 넘긴다
	void requestDisconnect(Session& session);                                        // 끊기 플래그 + 걸린 I/O 취소. 카운트는 건드리지 않는다 - 호출하는 쪽이 카운트를 잡고 있어야 한다
	void sessionUnLock(Session& session);                                               // I/O 카운트 -1, 0이면 해제 작업을 IOCP에 넣는다. 넣지 못하면 로그를 남기고 프로세스를 멈춘다
	void releaseSession(Session& session);                                           // (워커에서만) 소켓 닫기 + OnRelease + 버퍼 정리 + 슬롯 반환
	void onJobComplete(Session& session, JobOverlapped& ov);                         // OnJobComplete → AcquireSession 몫 IOCount 반납 (걸린 작업 수는 PostJobComplete가 내림)
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
