#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "Memory.h"

class Account;
class DbConn;
class DbThread;

// 계정 행(accounts)을 읽고 쓰는 문장을 만들어 DB 스레드에 넘긴다(2026-11-20 사용자 결정 - DB에는 계정 행만)
// 아이디는 클라가 보낸 바이트라 X'16진수'로 넣는다(이스케이프 없이 어떤 바이트든 안전하고, 열은 VARBINARY라 그대로 비교된다)
class GameDB final
{
public:
	explicit GameDB(DbThread* dbThread);
	DbThread& GetThread();

	// 서버 시작 전(main 스레드): 테이블을 만든다. 컨텐츠를 지우기 전의 옛 accounts(골드 열이 있음)면 실패 - 옮기거나 지운 뒤 다시
	static bool Prepare(DbConn& conn, std::string* outError);
	// DB 스레드에서(입장): 계정 행을 읽고, 없으면 만든다. DB 오류면 nullptr
	static UniquePtr<Account> LoadOrCreate(DbConn& conn, const std::string& id);

	// 메모리를 바꾼 계정 락 안에서 - 같은 계정의 변경이 DB에도 바꾼 순서대로 들어간다
	void SaveBan(const std::string& id, const int64_t banUntil); // banUntil: 유닉스 시각(초), 0이면 해제

	// 이번 실행에서 읽지 않은 계정은 메모리가 없어 DB 스레드가 바로 실행한다(대기열 뒤로 넘기면 그사이 그 계정의 입장이 옛 값을 읽는다)
	static std::string MakeBanSQL(const std::string& id, const int64_t banUntil);

	static std::string ToHexLiteral(const std::string& bytes); // X'...' 리터럴

private:
	DbThread* mDbThread;
};
