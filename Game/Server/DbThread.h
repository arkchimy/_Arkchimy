#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct MYSQL;

struct DbConfig
{
	std::string Host;
	int32_t Port = 3306;
	std::string User;
	std::string Password; // db.cfg에만 둔다(.gitignore) - 출력하지 않는다
	std::string Database;
};

// DB 연결 하나 - 스레드를 띄우기 전에는 main 스레드가(테이블 준비), 그 뒤로는 DB 스레드만 쓴다
// 연결이 끊기면(DB 재시작, wait_timeout, 망 단절 - 2006/2013) 표시해 두고 다음 문장 전에 다시 붙는다
class DbConn final
{
public:
	~DbConn();

	// 연결·읽기·쓰기 타임아웃을 건다 - DB가 응답하지 않아도 DB 스레드가 영영 멈추지 않게
	// bCreateDatabase: 데이터베이스가 없으면 만든다(서버 시작 때만 - CREATE 권한이 필요하다)
	bool Connect(const DbConfig& config, const bool bCreateDatabase, std::string* outError);
	void Close();
	bool Exec(const std::string& sql); // 실패하면 stderr에 오류와 SQL을 남기고 false
	// 결과 행을 문자열로(NULL은 빈 문자열). 실패하면 false. 연결이 끊겨 실패하면 다시 붙어 한 번 더 - 읽기 전용, 트랜잭션 안에서 쓰지 않는다
	bool Query(const std::string& sql, std::vector<std::vector<std::string>>* outRows);
	bool IsLost() const;         // 마지막 실패가 연결 끊김
	bool Reconnect();            // 끊겼으면 같은 설정으로 다시 붙는다. 성공하면 true

private:
	bool ensureConnected();      // 문장 전에 - 끊긴 연결이면 다시 붙는다
	bool queryOnce(const std::string& sql, std::vector<std::vector<std::string>>* outRows);
	void recordError(const std::string& sql);

	MYSQL* mConn = nullptr;
	DbConfig mConfig;            // 다시 붙을 때 쓴다
	bool mbLost = false;
};

// DB 전용 스레드 하나 + 요청 대기열(설계 3-2) - 워커는 요청을 넣고 바로 돌아간다
// 요청은 넣은 순서대로 하나씩 처리된다. 같은 데이터를 바꾼 락 안에서 넣으면 DB에도 바꾼 순서대로 쓰인다(설계 3-3)
// 대기열 락은 가장 안쪽 - 플레이어 락·경매장 락 안에서 넣어도 되고, 대기열 락을 잡은 채 다른 락을 잡지 않는다
class DbThread final
{
public:
	using Job = std::function<void(DbConn& conn)>;

	~DbThread();

	bool Connect(const DbConfig& config, std::string* outError);
	DbConn& GetStartupConn(); // 스레드를 띄우기 전에만 - 테이블 만들기·재시작 정리·번호 시작값
	void StartThread();
	// 남은 요청을 모두 처리한 뒤 끝낸다. 두 번 불러도 된다
	// 부르기 전에 요청을 넣는 쪽(워커의 OnRecv, 타이머)을 모두 멈춰야 한다 - 멈춘 뒤에 넣으면 코드 실수로 보고 멈춘다
	void Stop();

	void Post(Job job);
	// SQL 여러 개를 트랜잭션 하나로 - 하나라도 실패하면 모두 되돌리고 stderr에 남긴다. what은 로그에 쓸 문자열 상수
	void PostTransaction(std::vector<std::string> statements, const char* what);

	int64_t GetFailCnt() const;    // 실패한 트랜잭션 수(누적)
	int64_t GetPendingCnt() const; // 아직 처리하지 않은 요청 수

	// DB 스레드 안에서(또는 시작 전 main 스레드에서) 직접 쓸 때
	// COMMIT 전에 연결이 끊겨 실패하면(서버가 이미 되돌림) 다시 붙어 한 번 더 실행한다
	// COMMIT 중에 끊기면 반영됐는지 알 수 없어 다시 하지 않는다 - 골드 증감이 두 번 들어가면 안 되므로
	static bool RunTransaction(DbConn& conn, const std::vector<std::string>& statements, const char* what);

private:
	void run();

	DbConn mConn;
	mutable std::mutex mLock;         // 대기열 락
	std::condition_variable mWake;
	std::deque<Job> mJobs;
	bool mbStopping = false;
	bool mbExited = false;
	bool mbStarted = false;
	std::thread mThread;
	static std::atomic<int64_t> sFailCnt; // 실패 수 - 모니터 스레드가 읽는다
};
