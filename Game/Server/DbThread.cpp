#include <WinSock2.h>
#include <cstdio>
#include <utility>
#include <errmsg.h>
#include <mysql.h>
#include "DbThread.h"
#include "Assert.h"

enum : int32_t
{
	DB_CONNECT_TIMEOUT_SEC = 5,
	DB_RW_TIMEOUT_SEC = 30, // 문장 하나가 이보다 오래 응답이 없으면 끊긴 것으로 본다
};

std::atomic<int64_t> DbThread::sFailCnt = 0;

DbConn::~DbConn()
{
	Close();
}

bool DbConn::Connect(const DbConfig& config, const bool bCreateDatabase, std::string* outError)
{
	Assert(outError != nullptr, "Connect: outError가 null");
	Close();
	mConfig = config;
	mConn = mysql_init(nullptr);
	if (mConn == nullptr)
	{
		*outError = "mysql_init 실패";
		return false;
	}
	const unsigned int connectTimeout = DB_CONNECT_TIMEOUT_SEC;
	const unsigned int rwTimeout = DB_RW_TIMEOUT_SEC;
	mysql_options(mConn, MYSQL_OPT_CONNECT_TIMEOUT, &connectTimeout);
	mysql_options(mConn, MYSQL_OPT_READ_TIMEOUT, &rwTimeout);
	mysql_options(mConn, MYSQL_OPT_WRITE_TIMEOUT, &rwTimeout);
	mysql_options(mConn, MYSQL_SET_CHARSET_NAME, "utf8mb4");
	// 데이터베이스 만들기는 서버 시작 때 한 번만(bCreateDatabase) - 다시 연결·운영 툴은 DB 이름으로 바로 붙는다
	// (연결마다 CREATE DATABASE를 하면 CREATE 권한이 없는 계정 - 읽기 전용 운영 툴 계정 등 - 은 DB가 있어도 연결에 실패한다)
	bool bOk = mysql_real_connect(mConn, config.Host.c_str(), config.User.c_str(), config.Password.c_str(),
		bCreateDatabase ? nullptr : config.Database.c_str(), static_cast<unsigned int>(config.Port), nullptr, 0) != nullptr;
	if (bOk && bCreateDatabase)
	{
		const std::string create = "CREATE DATABASE IF NOT EXISTS `" + config.Database + "` CHARACTER SET utf8mb4"; // 이름은 설정 파일 값이다
		bOk = mysql_query(mConn, create.c_str()) == 0 && mysql_select_db(mConn, config.Database.c_str()) == 0;
	}
	if (!bOk)
	{
		*outError = mysql_error(mConn);
		Close();
	}
	mbLost = !bOk;
	return bOk;
}

void DbConn::Close()
{
	if (mConn != nullptr)
	{
		mysql_close(mConn);
		mConn = nullptr;
	}
}

bool DbConn::IsLost() const
{
	return mbLost;
}

bool DbConn::Reconnect()
{
	std::string error;
	const DbConfig config = mConfig; // Connect가 mConfig에 다시 쓴다
	if (!Connect(config, false, &error))
	{
		fprintf(stderr, "[db] 다시 연결 실패 - %s\n", error.c_str());
		return false;
	}
	fprintf(stderr, "[db] 다시 연결함\n");
	return true;
}

bool DbConn::ensureConnected()
{
	return !mbLost || Reconnect();
}

void DbConn::recordError(const std::string& sql)
{
	const unsigned int err = mysql_errno(mConn);
	mbLost = (err == CR_SERVER_GONE_ERROR || err == CR_SERVER_LOST); // 2006, 2013 - 다음 문장 전에 다시 붙는다
	fprintf(stderr, "[db] 실패 err=%u %s\n  SQL: %s\n", err, mysql_error(mConn), sql.c_str());
}

bool DbConn::Exec(const std::string& sql)
{
	if (!ensureConnected())
	{
		return false;
	}
	if (mysql_query(mConn, sql.c_str()) != 0)
	{
		recordError(sql);
		return false;
	}
	MYSQL_RES* result = mysql_store_result(mConn); // 결과가 있는 문장이 섞여도 다음 문장이 막히지 않게 비운다
	if (result != nullptr)
	{
		mysql_free_result(result);
	}
	return true;
}

bool DbConn::Query(const std::string& sql, std::vector<std::vector<std::string>>* outRows)
{
	Assert(outRows != nullptr, "Query: outRows가 null");
	// 읽기는 다시 해도 결과가 같다 - 연결이 끊겨 실패했으면(DB가 밤새 닫은 연결 등) 다시 붙어 한 번 더. Query는 트랜잭션 안에서 쓰지 않는다(트랜잭션은 Exec)
	bool bOk = queryOnce(sql, outRows);
	if (!bOk && mbLost)
	{
		bOk = queryOnce(sql, outRows);
	}
	return bOk;
}

bool DbConn::queryOnce(const std::string& sql, std::vector<std::vector<std::string>>* outRows)
{
	outRows->clear();
	if (!ensureConnected())
	{
		return false;
	}
	if (mysql_query(mConn, sql.c_str()) != 0)
	{
		recordError(sql);
		return false;
	}
	MYSQL_RES* result = mysql_store_result(mConn);
	if (result == nullptr)
	{
		if (mysql_field_count(mConn) == 0)
		{
			return true; // 결과가 없는 문장
		}
		recordError(sql); // 결과를 받는 중에 실패
		return false;
	}
	const unsigned int fieldCnt = mysql_num_fields(result);
	MYSQL_ROW row;
	while ((row = mysql_fetch_row(result)) != nullptr)
	{
		const unsigned long* lengths = mysql_fetch_lengths(result);
		std::vector<std::string> values;
		values.reserve(fieldCnt);
		for (unsigned int i = 0; i < fieldCnt; ++i)
		{
			values.push_back((row[i] != nullptr) ? std::string(row[i], lengths[i]) : std::string());
		}
		outRows->push_back(std::move(values));
	}
	mysql_free_result(result);
	return true;
}

DbThread::~DbThread()
{
	Stop();
}

bool DbThread::Connect(const DbConfig& config, std::string* outError)
{
	return mConn.Connect(config, true, outError); // 서버 시작 - 데이터베이스가 없으면 만든다
}

DbConn& DbThread::GetStartupConn()
{
	Assert(!mbStarted, "GetStartupConn: 스레드를 띄운 뒤에는 DB 스레드만 연결을 쓴다");
	return mConn;
}

void DbThread::StartThread()
{
	Assert(!mbStarted, "StartThread: 두 번 시작");
	mbStarted = true;
	mThread = std::thread(&DbThread::run, this);
}

void DbThread::Stop()
{
	{
		std::lock_guard<std::mutex> lock(mLock);
		mbStopping = true;
		if (!mbStarted)
		{
			mbExited = true; // 띄운 적 없음 - 이후 요청은 처리할 스레드가 없다
		}
	}
	mWake.notify_one();
	if (mThread.joinable())
	{
		mThread.join();
	}
}

void DbThread::Post(Job job)
{
	std::lock_guard<std::mutex> lock(mLock);
	// 멈춘 뒤의 요청은 처리할 스레드가 없다 - 저장이 조용히 사라지지 않게 멈춘다(GameServer::Shutdown이 넣는 쪽을 먼저 멈춘다)
	Assert(!mbExited, "DbThread::Post: DB 스레드가 멈춘 뒤 요청이 들어옴 - 넣는 쪽을 먼저 멈춰야 한다");
	mJobs.push_back(std::move(job));
	mWake.notify_one(); // 락 안에서 깨운다 - 놓은 뒤 깨우면 그사이 Stop·소멸이 끼어들 수 있다
}

void DbThread::PostTransaction(std::vector<std::string> statements, const char* what)
{
	Post([statements = std::move(statements), what](DbConn& conn)
		{
			RunTransaction(conn, statements, what);
		});
}

bool DbThread::RunTransaction(DbConn& conn, const std::vector<std::string>& statements, const char* what)
{
	bool bOk = false;
	bool bRetry = true;
	for (int32_t attempt = 0; attempt < 2 && bRetry; ++attempt)
	{
		bOk = conn.Exec("START TRANSACTION");
		for (size_t i = 0; bOk && i < statements.size(); ++i)
		{
			bOk = conn.Exec(statements[i]);
		}
		// COMMIT 전에 끊겼으면 서버가 이미 되돌렸으니 다시 붙어 한 번 더. 그 밖의 실패(문장 오류)는 다시 해도 같다
		bRetry = !bOk && conn.IsLost();
		if (bOk)
		{
			bOk = conn.Exec("COMMIT"); // 여기서 끊기면 반영 여부를 모른다 - 다시 하지 않는다
			bRetry = false;
		}
		if (!bOk && !conn.IsLost())
		{
			conn.Exec("ROLLBACK");
		}
	}
	if (!bOk)
	{
		++sFailCnt;
		fprintf(stderr, "[db] 트랜잭션 실패: %s - 메모리와 DB가 어긋났을 수 있다\n", what);
	}
	return bOk;
}

int64_t DbThread::GetFailCnt() const
{
	return sFailCnt;
}

int64_t DbThread::GetPendingCnt() const
{
	std::lock_guard<std::mutex> lock(mLock);
	return static_cast<int64_t>(mJobs.size());
}

void DbThread::run()
{
	mysql_thread_init(); // 연결을 만들지 않은 스레드가 C API를 쓰기 전에(스레드별 상태)
	while (true)
	{
		Job job;
		{
			std::unique_lock<std::mutex> lock(mLock);
			mWake.wait(lock, [this]
				{
					return mbStopping || !mJobs.empty();
				});
			if (mJobs.empty())
			{
				mbExited = true; // 멈추라는 요청이 왔고 남은 요청도 없다
				break;
			}
			job = std::move(mJobs.front());
			mJobs.pop_front();
		}
		job(mConn);
	}
	mysql_thread_end();
}
