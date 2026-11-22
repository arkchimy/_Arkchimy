#pragma once
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "NetworkLib.h"
#include "IntervalThread.h"
#include "MonitorDB.h"
#include "MonitorProtocol.h"

class DbThread;
class DbConn;

// 모니터링 서버(설계 portfolio/design/모니터링서버_설계.docx) - 같은 라이브러리 위의 세 번째 서버
// 한 포트에 보내는 쪽(로그인·게임 서버의 MonitorAgent)과 보는 쪽(MonitorClient)이 붙고, 첫 패킷의 키로 역할을 정한다
// ① 워커: 표본 → 1초 값 계산 → 서버별 저장 대기에 넣고 보는 쪽 전원에게 보냄
// ② 감시 스레드(1초): 콘솔 출력, 응답 없음 판정(3초), 인증 기한(5초), 분이 바뀌면 지난 분의 행을 DB 스레드로(1분에 한 번 묶어서)
// ③ DB 스레드: 묶음 저장(트랜잭션 하나, 실패하면 행을 돌려받아 다음 분에 다시), 지난 기록 조회
// 락은 mLock 하나 - 칸·서버 상태·이벤트를 함께 지키고, 보는 쪽으로 보내기(SendPacket)도 그 안에서 해 순서를 지킨다. DB 대기열 락은 그 안쪽
class MonitorServer final : public NetworkLib
{
public:
	MonitorServer(const std::string& key, const int32_t maxSession, DbThread* db);
	~MonitorServer() override;

	bool StartMonitor(const NetConfig& config); // Start + 감시 스레드
	// 감시 스레드 → 라이브러리 Stop → 남은 행을 DB 스레드에 넘긴다(DB 스레드 Stop은 main이 그 뒤에). 두 번 불러도 된다
	void Shutdown();

protected:
	void OnAccept(const uint64_t id, const SOCKADDR_IN& remoteAddr) override;
	void OnRecv(const uint64_t id, const char* payload, const int32_t len) override;
	void OnRelease(const uint64_t id) override;

private:
	enum class eRole : uint8_t
	{
		None,   // 아직 키를 받지 않음
		Agent,
		Viewer,
	};

	struct Slot
	{
		uint64_t SessionID; // 0이면 빈 칸
		ULONGLONG AcceptTick;
		eRole Role;
		int32_t AgentIdx;   // Agent일 때 mAgents의 번호
		bool bHistoryBusy;  // Viewer가 보낸 기록 요청을 아직 처리 중 - 그사이 또 오면 끊는다(송신 링을 넘기지 않게)
		char IP[16];
	};

	// 서버 하나의 상태 - 이름으로 찾는다. 연결이 끊겨도 지우지 않는다(다시 붙으면 이어서)
	struct AgentState
	{
		std::string Name;
		eServerKind Kind;
		eServerState State;
		uint64_t SessionID;        // 0이면 연결 없음
		ULONGLONG LastSampleTick;
		bool bHasPrev;             // 이전 표본이 있어야 1초 값을 계산한다
		MonSample Prev;
		bool bHasLast;
		MonRateRow Last;           // 마지막 1초 값 - 콘솔 출력과 새로 붙은 보는 쪽
		std::deque<MonRateRow> Unsaved; // 아직 DB에 넣지 않은 행(시각 순)
		bool bDropping;            // 저장 대기가 상한을 넘어 버리는 중 - 이벤트를 한 번만 남기려고
	};

	struct HistoryJob
	{
		uint64_t SessionID;
		MonHistoryReq Req;
	};

	void onAgentHello(const uint64_t id, const char* payload, const int32_t len);
	void onSample(const uint64_t id, const char* payload, const int32_t len);
	void onBye(const uint64_t id);
	void onViewerHello(const uint64_t id, const char* payload, const int32_t len);
	void onHistoryReq(const uint64_t id, const char* payload, const int32_t len);
	void onTick();
	void saveOnDbThread(DbConn& conn, std::shared_ptr<MonSaveBatch> batch);
	void historyOnDbThread(DbConn& conn, const HistoryJob job);

	// 아래는 mLock 안에서
	Slot& getSlot(const uint64_t id);
	int32_t findAgent(const std::string& name) const;
	void addEventLocked(const std::string& name, const std::string& text);
	void broadcastLocked(const char* data, const int32_t len);
	void sendStateLocked(const uint64_t viewerID, const AgentState& agent); // viewerID가 0이면 보는 쪽 전원
	void pushRowLocked(AgentState* agent, const MonRateRow& row);
	std::shared_ptr<MonSaveBatch> takeBatchLocked(const int64_t beforeSec);
	void returnBatchLocked(const MonSaveBatch& batch); // 저장 실패 - 행·이벤트를 앞으로 되돌린다

	const std::string mKey;
	DbThread* const mDb;
	std::mutex mLock;
	std::vector<Slot> mSlots; // 세션과 같은 인덱스
	std::vector<UniquePtr<AgentState>> mAgents;
	std::deque<MonEvent> mRecentEvents;        // 새로 붙은 보는 쪽에 보낼 최근 이벤트
	std::vector<MonEventRow> mUnsavedEvents;
	std::shared_ptr<MonSaveBatch> mInFlight;   // DB 스레드가 넣는 중인 묶음 - 하나씩(순서를 지키려고)
	int64_t mLastFlushMinute = 0;
	int64_t mLastRetentionHour = 0;
	int64_t mSaveCnt = 0;
	int64_t mSaveFailCnt = 0;
	int64_t mDroppedRowCnt = 0;
	bool mbSaveFailing = false;
	bool mbShutdown = false;
	IntervalThread mTick;
};
