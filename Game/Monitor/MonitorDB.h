#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "MonitorProtocol.h"

class DbConn;

// 1분에 한 번 넣는 묶음(설계 4-3) - 모니터 워커·감시 스레드가 만들고 DB 스레드가 넣는다
struct MonAgentRows
{
	std::string Name;
	std::vector<MonRateRow> Rows; // 시각 순
};

struct MonEventRow
{
	std::string Name;
	int64_t UnixMs = 0;
	std::string Text; // UTF-8(표가 utf8mb4)
};

struct MonSaveBatch
{
	std::vector<MonAgentRows> Agents;
	std::vector<MonEventRow> Events;
	bool bRetention = false; // 오래된 행 지우기를 함께(1시간에 한 번)
};

// 모니터 표 - 열 이름은 MON_VALUE_COLUMNS. 시각은 유닉스 초(BIGINT) - 볼 때는 FROM_UNIXTIME(ts)
// monitor_sample: 1초 원본(7일) / monitor_minute: 1분 요약 평균·최소·최대(90일) / monitor_event: 이벤트(90일)
class MonitorDB final
{
public:
	static bool Prepare(DbConn& conn, std::string* outError);
	// 1초 원본 여러 행 REPLACE 한 문장(서버마다) + 그 분들의 요약을 monitor_sample에서 다시 계산해 REPLACE + 이벤트 INSERT
	// 요약을 원본에서 다시 계산하므로 늦게 온 표본이 다음 묶음에 들어가도 그 분의 요약이 틀어지지 않는다
	static void MakeSaveSQL(const MonSaveBatch& batch, const int64_t nowSec, std::vector<std::string>* outStatements);
	// [fromSec, toSec]의 행을 시각 순으로 최대 limit개. bMinute면 1분 요약의 평균
	static bool QueryHistory(DbConn& conn, const std::string& name, const int64_t fromSec, const int64_t toSec, const bool bMinute,
		const int32_t limit, std::vector<MonRateRow>* outRows);
	// 서버 이름 - 영문·숫자·_·- 만(SQL에 그대로 넣는다)
	static bool IsValidName(const std::string& name);
};
