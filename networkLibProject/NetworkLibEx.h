#pragma once
#include "NetworkLib.h"	// Session, NetStat, WorkerCounter 등 공통 타입은 그대로 쓴다

// NetworkLib과 같은 라이브러리 - 워커가 GetQueuedCompletionStatusEx로 완료를 여러 개씩 꺼내는 것만 다르다
// 호출 1번에 완료 여러 개 → 커널 전환과 IOCP 포트 내부 락을 잡는 횟수가 줄어든다
// (비교 실험용: 바뀐 곳은 workerLoop, setGqcsBatch, mGqcsBatch뿐이고 나머지는 NetworkLib.cpp와 같다)
//
// IOCP 네트워크 라이브러리 - 컨텐츠는 상속해서 On* 콜백을 구현하고, 세션은 ID로만 다룬다
// 콜백 규칙
// - 모든 콜백은 IOCP 워커 스레드에서 불린다. 서로 다른 세션의 콜백은 동시에 불릴 수 있다 (공유 데이터는 컨텐츠가 보호)
// - 한 세션 안에서는 OnAccept → OnRecv(0번 이상, 순서대로, 동시에 안 불림) → OnRelease(정확히 1번) 순서가 보장된다
// - OnRelease는 다른 콜백이나 SendPacket/Disconnect 안에서 재진입하지 않는다. 끝나면 그 ID는 무효
// 파생 클래스 소멸자에서 반드시 stop()을 먼저 부를 것 - 기반 소멸자에서는 파생 부분이 이미 없어 OnRelease를 부를 수 없다
class NetworkLibEx
{
public:
	virtual ~NetworkLibEx();
	// workerCnt: IOCP 워커 수, slotCnt: 걸어두는 AcceptEx 수, maxSession: 최대 동시 접속
	bool start(USHORT port, int backlog, int workerCnt, int slotCnt, int maxSession);
	void stop();	// AcceptEx 중단 → 세션 전부 끊기 → I/O 정리 대기 → 워커 종료. 워커/콜백 안에서 부르면 데드락
	bool isRunning() const { return mRunning; }	// false면 종료 요청(치명적 오류 포함) - main이 보고 stop을 부른다
	NetStat getStat() const;	// 누적 카운터 읽기 (통계 출력용)

	// RTT 측정용 핑 - start 전에 부른다. intervalMs: 세션당 핑 간격(0이면 끔), sampleMod: 슬롯 N개 중 1개만 측정
	void setPing(int intervalMs, int sampleMod);
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
	std::vector<uint64_t> getRttCounts() const;	// 누적 RTT 히스토그램(워커별 합, µs) - 1초 차분은 부르는 쪽이 만든다

	// GQCSEx 한 번에 꺼낼 최대 완료 수 - start 전에 부른다. 크면 호출 수는 줄지만 한 워커가 몰아 처리하는 동안 다른 워커가 놀 수 있다
	void setGqcsBatch(int count) { mGqcsBatch = (count < 1) ? 1 : count; }

	// 아무 스레드에서나 호출 가능. 헤더는 라이브러리가 붙인다
	bool SendPacket(uint64_t id, const char* payload, int32_t len);	// false: 이미 끊김/끊는 중, 길이 초과, 송신 링 넘침(이 경우 세션을 끊는다)
	void Disconnect(uint64_t id);	// 끊기 요청 - 걸린 I/O를 취소하고, 모두 끝나면 OnRelease
	// 세션 ID의 슬롯 번호(0 ~ maxSession-1) - 컨텐츠가 세션별 데이터를 배열로 둘 때 쓴다. 슬롯은 재사용되므로 OnAccept에서 초기화할 것
	static uint32_t SlotOf(uint64_t id) { return static_cast<uint32_t>(id); }

protected:
	virtual void OnAccept(uint64_t id) = 0;	// 연결 수락 직후, 첫 수신을 걸기 전. 여기서 SendPacket 가능
	virtual void OnRecv(uint64_t id, const char* payload, int32_t len) = 0;	// 완성된 패킷 1개 (헤더 제외). payload는 콜백 안에서만 유효
	virtual void OnRelease(uint64_t id) = 0;	// 모든 I/O가 끝나 세션이 해제됨

private:
	bool initIocp(int workerCnt); 	// IOCP 생성 → 워커 스레드 workerCnt개 시작
	void stopWorkers(); 	// 워커 수만큼 종료 신호를 넣고 모두 join
	void workerLoop(int workerIdx);	// GQCSEx로 완료를 최대 mGqcsBatch개씩 꺼내 처리. workerIdx: 워커별 RTT 히스토그램 번호
	void pingLoop();	// 핑 전용 타이머 스레드 - 틱마다 세션 일부에 핑
	void sendPingSlice(int slice, int sliceCnt);	// 조각 slice에 속한 표본 세션에 핑 1개씩
	void onPong(Session* session, const char* payload, int32_t len, int64_t recvQpc);	// 순번 확인 후 RTT 기록
	bool postAccept(AcceptOv* ov);	// 기존 ov 슬롯을 재사용해 AcceptEx 하나 등록
	void onAcceptComplete(AcceptOv* ov, BOOL ok);	// 세션 생성 + 첫 수신, 슬롯 재등록
	Session* allocSession();	// 빈 슬롯 꺼내기 + 새 ID 발급, 가득 차면 nullptr
	void freeSession(Session* session);	// 슬롯 반환
	bool acquireSession(Session* session);	// 살아 있을 때만 I/O 카운트 +1 (해제를 막고 세션 사용)
	Session* findSession(uint64_t id);	// ID로 세션을 찾아 acquire. 이미 끊겼거나 슬롯이 다른 세션에 재사용됐으면 nullptr. 다 쓰면 decIoCount
	// postRecv/postSend는 호출하는 쪽이 카운트를 잡고 있어야 한다 - 걸고 난 뒤 세션을 다시 읽기 때문
	void postRecv(Session* session);	// 수신 링의 빈 공간에 수신 1건 등록 (끊는 중이면 안 건다). 즉시 완료(skip)면 그 자리에서 처리하고 다시 건다
	void onRecvComplete(Session* session, BOOL ok, DWORD transferred);	// 완성된 패킷을 하나씩 OnRecv로 넘기고 다음 수신 등록
	bool processRecv(Session* session, DWORD transferred);	// 받은 바이트에서 완성된 패킷을 꺼내 처리. 0바이트(상대가 끊음)면 false
	void postSend(Session* session);	// 쌓인 데이터를 송신 1건으로 등록 (송신 중이거나 끊는 중이면 무시). 즉시 완료(skip)면 그 자리에서 처리하고 이어서 보낸다
	void onSendComplete(Session* session, BOOL ok, DWORD transferred);	// 보낸 만큼 pop, 이어서 송신
	void finishSend(Session* session, DWORD transferred);	// 성공한 송신 1건 마무리 - 보낸 만큼 pop, 송신 플래그 내림
	bool sendRaw(Session* session, ePacketKind kind, const char* payload, int32_t len);	// 헤더를 붙여 송신 링에 넣고 송신. 호출하는 쪽이 카운트를 잡고 있어야 한다
	void queueFlush(Session* session);	// 수신 처리 중의 송신 - 보낼 목록에 올리기만 한다 (처음 올릴 때 카운트 +1)
	void flushSends();	// 수신 처리 끝 - 목록의 세션마다 송신 1건, 목록 몫 카운트 반납
	void requestDisconnect(Session* session);	// 끊기 플래그 + 걸린 I/O 취소. 카운트는 건드리지 않는다
	void decIoCount(Session* session);	// I/O 카운트 -1, 0이면 해제 작업을 IOCP에 넣는다
	void releaseSession(Session* session);	// (워커에서만) 소켓 닫기 + OnRelease + 버퍼 정리 + 슬롯 반환

	SOCKET mListenSock = INVALID_SOCKET;
	HANDLE mIocp = nullptr;
	std::vector<std::thread> mWorkers;
	int mGqcsBatch = 64;	// GQCSEx 한 번에 꺼낼 최대 완료 수

	std::vector<AcceptOv> mAcceptOvs;	// 고정 크기 - 완료되면 같은 슬롯 재사용
	std::vector<Session> mSessions;	// 고정 크기 - 시작할 때 maxSession개 생성
	std::vector<int32_t> mFreeIdx;	// 비어 있는 세션 슬롯 번호 - 끝에서만 넣고 뺀다(스택)
	std::mutex mFreeLock;
	std::mutex mStartStopLock;	// start/stop 전체를 감싼다 - 여러 스레드가 동시에 불러도 한 번에 하나만 진행
	std::atomic<bool> mRunning = false;
	std::atomic<int32_t> mPendingAccept = 0;	// 걸려 있는 AcceptEx 수 - 종료 시 0이 될 때까지 기다린다

	// RTT 측정 - 핑은 워커와 독립된 스레드가 보낸다 (워커가 멈춰도 핑이 나가야 멈춘 시간이 RTT에 잡힌다)
	std::thread mPingThread;
	int mPingIntervalMs = 0;
	int mPingSampleMod = 1;
	bool mSendBufZero = false;	// 수락한 소켓에 SO_SNDBUF = 0을 걸지
	int mTestSendCompleteDelayMs = 0;	// [테스트 전용] 송신 완료 처리 전 지연
	bool mSkipOnSuccess = false;	// 수락한 소켓에 FILE_SKIP_COMPLETION_PORT_ON_SUCCESS를 걸지 - 모든 세션이 같은 값이어야 한다(즉시 완료 처리 경로가 이 값을 본다)
	bool mFlushPerBatch = false;	// 수신 처리 중 송신을 완료 묶음 끝에서 보낼지 (false면 수신 완료 1건 끝)
	int64_t mQpcFreq = 0;
	std::vector<std::unique_ptr<LatencyHistogram>> mRttHists;	// 워커별 1개 - 퐁을 처리한 워커만 쓴다
	std::vector<std::unique_ptr<WorkerCounter>> mWorkerCounters;	// 워커별 1개
	std::atomic<int64_t> mPingSentCnt = 0;
	std::atomic<int64_t> mPingSkipCnt = 0;
	std::atomic<int64_t> mPingFlagBlockedCnt = 0;	// 핑 대기 측정 - 핑 스레드와 워커가 쓰므로 atomic (초당 수천 건이라 경합 무시)
	std::atomic<int64_t> mPingSendWaitCnt = 0;
	std::atomic<int64_t> mPingSendWaitTicks = 0;

	// 통계 카운터 - 연결 단위 사건만 센다 (바이트 단위는 워커 간 캐시 라인 경합이 핫패스에 붙어서 제외)
	std::atomic<int64_t> mAcceptCnt = 0;
	std::atomic<int64_t> mReleaseCnt = 0;
	std::atomic<int64_t> mOverflowCnt = 0;
	std::atomic<int64_t> mSessionFullCnt = 0;
	std::atomic<int64_t> mBadPacketCnt = 0;
};
