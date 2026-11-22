#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include "MonitorDB.h"
#include "Assert.h"
#include "DbThread.h"

enum : int64_t
{
	SAMPLE_KEEP_SEC = 7 * 24 * 3600,  // 1초 원본 보관(설계 4-3)
	MINUTE_KEEP_SEC = 90 * 24 * 3600, // 1분 요약·이벤트 보관
};

static std::string toNumber(const double value)
{
	char buf[32];
	snprintf(buf, sizeof(buf), "%.3f", value);
	return buf;
}

// 이벤트 글은 모니터가 만든 것뿐이지만 따옴표·역슬래시가 SQL을 깨지 않게
static std::string escapeText(const std::string& text)
{
	std::string out;
	out.reserve(text.size());
	for (const char c : text)
	{
		if (c == '\'' || c == '\\')
		{
			out += '\\';
		}
		out += c;
	}
	return out;
}

bool MonitorDB::IsValidName(const std::string& name)
{
	return IsValidMonName(name.data(), name.size());
}

bool MonitorDB::Prepare(DbConn& conn, std::string* outError)
{
	Assert(outError != nullptr, "MonitorDB::Prepare: outError가 nullptr");
	std::string sample = "CREATE TABLE IF NOT EXISTS monitor_sample (server VARCHAR(15) NOT NULL, ts BIGINT NOT NULL";
	std::string minute = "CREATE TABLE IF NOT EXISTS monitor_minute (server VARCHAR(15) NOT NULL, minute BIGINT NOT NULL, n INT NOT NULL";
	for (const char* column : MON_VALUE_COLUMNS)
	{
		sample += std::string(", ") + column + " FLOAT NOT NULL";
		minute += std::string(", avg_") + column + " FLOAT NOT NULL, min_" + column + " FLOAT NOT NULL, max_" + column + " FLOAT NOT NULL";
	}
	sample += ", PRIMARY KEY (server, ts)) DEFAULT CHARSET = utf8mb4";
	minute += ", PRIMARY KEY (server, minute)) DEFAULT CHARSET = utf8mb4";
	const std::string event = "CREATE TABLE IF NOT EXISTS monitor_event (id BIGINT NOT NULL AUTO_INCREMENT PRIMARY KEY,"
		" server VARCHAR(15) NOT NULL, at_ms BIGINT NOT NULL, text VARCHAR(100) NOT NULL, KEY (server, at_ms)) DEFAULT CHARSET = utf8mb4";
	const bool bOk = conn.Exec(sample) && conn.Exec(minute) && conn.Exec(event);
	if (!bOk)
	{
		*outError = "monitor_sample·monitor_minute·monitor_event 표를 만들지 못함";
	}
	return bOk;
}

void MonitorDB::MakeSaveSQL(const MonSaveBatch& batch, const int64_t nowSec, std::vector<std::string>* outStatements)
{
	Assert(outStatements != nullptr, "MakeSaveSQL: outStatements가 nullptr");
	std::vector<std::string>& statements = *outStatements;
	std::string columns;
	std::string summary;
	for (const char* column : MON_VALUE_COLUMNS)
	{
		columns += std::string(", ") + column;
		summary += std::string(", AVG(") + column + "), MIN(" + column + "), MAX(" + column + ")";
	}
	std::string minuteColumns;
	for (const char* column : MON_VALUE_COLUMNS)
	{
		minuteColumns += std::string(", avg_") + column + ", min_" + column + ", max_" + column;
	}

	for (const MonAgentRows& agent : batch.Agents)
	{
		if (agent.Rows.empty())
		{
			continue;
		}
		Assert(IsValidName(agent.Name), "MakeSaveSQL: 확인하지 않은 서버 이름");
		std::string sql = "REPLACE INTO monitor_sample (server, ts" + columns + ") VALUES ";
		int64_t firstSec = agent.Rows.front().UnixSec;
		int64_t lastSec = firstSec;
		for (size_t i = 0; i < agent.Rows.size(); ++i)
		{
			const MonRateRow& row = agent.Rows[i];
			firstSec = std::min(firstSec, row.UnixSec);
			lastSec = std::max(lastSec, row.UnixSec);
			sql += (i == 0) ? "('" : ",('";
			sql += agent.Name + "'," + std::to_string(row.UnixSec);
			for (const float value : row.Values)
			{
				sql += "," + toNumber(value);
			}
			sql += ")";
		}
		statements.push_back(sql);
		const int64_t fromMinute = firstSec / 60 * 60;
		const int64_t toMinute = lastSec / 60 * 60 + 60;
		statements.push_back("REPLACE INTO monitor_minute (server, minute, n" + minuteColumns + ") SELECT server, ts DIV 60 * 60, COUNT(*)" + summary
			+ " FROM monitor_sample WHERE server = '" + agent.Name + "' AND ts >= " + std::to_string(fromMinute) + " AND ts < " + std::to_string(toMinute)
			+ " GROUP BY server, ts DIV 60");
	}
	for (const MonEventRow& event : batch.Events)
	{
		statements.push_back("INSERT INTO monitor_event (server, at_ms, text) VALUES ('" + escapeText(event.Name) + "'," + std::to_string(event.UnixMs)
			+ ",'" + escapeText(event.Text) + "')");
	}
	if (batch.bRetention)
	{
		// 서버 이름마다 나눠 지운다 - 기본 키가 (server, ts)라 server를 함께 주어야 키의 앞부분으로 범위만 읽는다(ts만 주면 표 전체를 훑는다)
		std::vector<std::string> names;
		for (const MonAgentRows& agent : batch.Agents)
		{
			names.push_back(agent.Name);
		}
		names.push_back("monitor"); // 모니터 자신의 이벤트(DB 저장 실패 등)
		for (const std::string& name : names)
		{
			Assert(IsValidName(name), "MakeSaveSQL: 확인하지 않은 서버 이름");
			const std::string where = " WHERE server = '" + name + "' AND ";
			statements.push_back("DELETE FROM monitor_sample" + where + "ts < " + std::to_string(nowSec - SAMPLE_KEEP_SEC));
			statements.push_back("DELETE FROM monitor_minute" + where + "minute < " + std::to_string(nowSec - MINUTE_KEEP_SEC));
			statements.push_back("DELETE FROM monitor_event" + where + "at_ms < " + std::to_string((nowSec - MINUTE_KEEP_SEC) * 1000));
		}
	}
}

bool MonitorDB::QueryHistory(DbConn& conn, const std::string& name, const int64_t fromSec, const int64_t toSec, const bool bMinute,
	const int32_t limit, std::vector<MonRateRow>* outRows)
{
	Assert(outRows != nullptr, "QueryHistory: outRows가 nullptr");
	Assert(IsValidName(name), "QueryHistory: 확인하지 않은 서버 이름");
	std::string columns;
	for (const char* column : MON_VALUE_COLUMNS)
	{
		columns += bMinute ? (std::string(", avg_") + column) : (std::string(", ") + column);
	}
	const char* const table = bMinute ? "monitor_minute" : "monitor_sample";
	const char* const timeColumn = bMinute ? "minute" : "ts";
	const std::string sql = std::string("SELECT ") + timeColumn + columns + " FROM " + table + " WHERE server = '" + name + "' AND " + timeColumn
		+ " BETWEEN " + std::to_string(fromSec) + " AND " + std::to_string(toSec) + " ORDER BY " + timeColumn + " LIMIT " + std::to_string(limit);
	std::vector<std::vector<std::string>> result;
	if (!conn.Query(sql, &result))
	{
		return false;
	}
	for (const std::vector<std::string>& cells : result)
	{
		if (cells.size() != 1 + MON_VALUE_CNT)
		{
			continue;
		}
		MonRateRow row{};
		row.UnixSec = _strtoi64(cells[0].c_str(), nullptr, 10);
		for (int32_t i = 0; i < MON_VALUE_CNT; ++i)
		{
			row.Values[i] = static_cast<float>(atof(cells[1 + i].c_str()));
		}
		outRows->push_back(row);
	}
	return true;
}
