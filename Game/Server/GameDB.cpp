#include <cstdlib>
#include "GameDB.h"
#include "Account.h"
#include "Assert.h"
#include "DbThread.h"

// 계정 행 하나 = 아이디 + 차단 시각. 아이디 열은 VARBINARY(클라가 보낸 바이트 그대로, 대소문자 구분 = 메모리 계정 표와 같은 비교)
// 로그인 정보(비밀번호 해시, AccountNo)는 로그인 서버의 credentials 표에 있다 - 이 행은 게임 서버가 처음 입장할 때 만든다
static const char* const SCHEMA =
	"CREATE TABLE IF NOT EXISTS accounts ("
	" account_id VARBINARY(15) NOT NULL PRIMARY KEY,"
	" ban_until DATETIME NULL," // 이 시각까지 입장 차단(운영 툴 제재)
	" created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP)";

GameDB::GameDB(DbThread* dbThread)
	: mDbThread(dbThread)
{
	Assert(dbThread != nullptr, "GameDB: dbThread가 null");
}

DbThread& GameDB::GetThread()
{
	return *mDbThread;
}

std::string GameDB::ToHexLiteral(const std::string& bytes)
{
	constexpr char DIGITS[] = "0123456789ABCDEF";
	std::string out = "X'";
	for (const char c : bytes)
	{
		const uint8_t byte = static_cast<uint8_t>(c);
		out.push_back(DIGITS[byte >> 4]);
		out.push_back(DIGITS[byte & 0x0F]);
	}
	out.push_back('\'');
	return out;
}

bool GameDB::Prepare(DbConn& conn, std::string* outError)
{
	Assert(outError != nullptr, "Prepare: outError가 null");
	if (!conn.Exec(SCHEMA))
	{
		*outError = "accounts 테이블 만들기 실패";
		return false;
	}
	// CREATE TABLE IF NOT EXISTS는 이미 있는 표를 바꾸지 않는다 - 옛 표(gold NOT NULL)에는 새 계정 INSERT가 실패하므로 시작할 때 알린다
	std::vector<std::vector<std::string>> rows;
	if (!conn.Query("SHOW COLUMNS FROM accounts LIKE 'gold'", &rows))
	{
		*outError = "accounts 열 확인 실패";
		return false;
	}
	if (!rows.empty())
	{
		*outError = "옛 accounts 표(gold 열)가 남아 있음 - 컨텐츠를 지운 뒤의 표가 아니다. 시험 DB면 옛 표(accounts·items·auctions·logs)를 옮기거나 지운 뒤 다시 시작";
		return false;
	}
	return true;
}

UniquePtr<Account> GameDB::LoadOrCreate(DbConn& conn, const std::string& id)
{
	UniquePtr<Account> account = MakeUnique<Account>(id);
	std::vector<std::vector<std::string>> rows;
	bool bOk = conn.Query("SELECT IFNULL(UNIX_TIMESTAMP(ban_until), 0) FROM accounts WHERE account_id = " + ToHexLiteral(id), &rows);
	if (bOk && rows.empty())
	{
		// 처음 보는 아이디 - 트랜잭션으로 넣어 끊긴 연결이면 다시 붙어 한 번 더 한다(DbThread::RunTransaction)
		bOk = DbThread::RunTransaction(conn, { "INSERT INTO accounts (account_id) VALUES (" + ToHexLiteral(id) + ")" }, "계정 만들기");
	}
	else if (bOk)
	{
		std::lock_guard<std::mutex> accountLock(account->GetLock()); // 아직 아무도 모르는 계정이지만 Account의 약속(락을 잡고 고친다)을 지킨다
		account->SetBanUntil(std::strtoll(rows[0][0].c_str(), nullptr, 10));
	}
	if (!bOk)
	{
		account.reset(); // DB 오류 - 반쯤 읽은 계정을 쓰지 않는다
	}
	return account;
}

std::string GameDB::MakeBanSQL(const std::string& id, const int64_t banUntil)
{
	return "UPDATE accounts SET ban_until = " + (banUntil > 0 ? "FROM_UNIXTIME(" + std::to_string(banUntil) + ")" : std::string("NULL"))
		+ " WHERE account_id = " + ToHexLiteral(id);
}

void GameDB::SaveBan(const std::string& id, const int64_t banUntil)
{
	mDbThread->PostTransaction({ MakeBanSQL(id, banUntil) }, "제재");
}
