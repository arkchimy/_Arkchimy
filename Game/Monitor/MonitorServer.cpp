#include <WinSock2.h>
#include <WS2tcpip.h>
#include <algorithm>
#include <cstdio>
#include <functional>
#include "MonitorServer.h"
#include "Assert.h"
#include "DbThread.h"

enum : int32_t
{
	AUTH_LIMIT_MS = 5000,          // 접속 후 이 안에 키가 와야 한다
	RECENT_EVENT_CNT = 40,         // 새로 붙은 보는 쪽에 보낼 이벤트 수
	UNSAVED_ROW_MAX = 30 * 60,     // 서버 하나의 저장 대기 상한(30분) - DB가 오래 멈추면 오래된 것부터 버린다(설계 4-3)
	HISTORY_ROW_MAX = MON_HISTORY_ROWS * MON_HISTORY_PACKETS,
};

static int64_t getUnixMs()
{
	FILETIME now;
	GetSystemTimeAsFileTime(&now);
	constexpr int64_t UNIX_EPOCH_100NS = 116444736000000000LL;
	const int64_t value = (static_cast<int64_t>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
	return (value - UNIX_EPOCH_100NS) / 10000;
}

template <size_t N>
static std::string toText(const char (&text)[N])
{
	return std::string(text, strnlen(text, N));
}

template <size_t N>
static void copyText(char (&outText)[N], const std::string& text)
{
	MemSet(outText, 0, N);
	MemCpy(outText, text.data(), (text.size() < N) ? text.size() : N - 1);
}

// 소스의 한글 문자열 상수는 실행 문자 집합(CP949)으로 컴파일된다 - 콘솔에는 그대로 찍고, DB(utf8mb4)와 보는 쪽(ImGui)에는 UTF-8로 바꿔 보낸다
// (CP949 그대로 넣으면 MySQL이 잘못된 UTF-8로 보고 글을 잘라 빈 문자열이 된다)
static std::string toUtf8(const std::string& text)
{
	const int wideLen = MultiByteToWideChar(CP_ACP, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
	std::wstring wide(static_cast<size_t>(wideLen), L'\0');
	MultiByteToWideChar(CP_ACP, 0, text.data(), static_cast<int>(text.size()), wide.data(), wideLen);
	const int utf8Len = WideCharToMultiByte(CP_UTF8, 0, wide.data(), wideLen, nullptr, 0, nullptr, nullptr);
	std::string utf8(static_cast<size_t>(utf8Len), '\0');
	WideCharToMultiByte(CP_UTF8, 0, wide.data(), wideLen, utf8.data(), utf8Len, nullptr, nullptr);
	return utf8;
}

// UTF-8을 고정 칸에 - 넘치면 글자 중간에서 자르지 않게 이어지는 바이트(10xxxxxx) 앞에서 자른다
template <size_t N>
static void copyUtf8(char (&outText)[N], const std::string& utf8)
{
	size_t len = (utf8.size() < N) ? utf8.size() : N - 1;
	while (len > 0 && len < utf8.size() && (static_cast<uint8_t>(utf8[len]) & 0xC0) == 0x80)
	{
		--len;
	}
	MemSet(outText, 0, N);
	MemCpy(outText, utf8.data(), len);
}

static const char* toString(const eServerState state)
{
	switch (state)
	{
	case eServerState::Online:
		return "정상";
	case eServerState::NoResponse:
		return "응답 없음";
	case eServerState::Stopped:
		return "종료";
	default:
		Assert(false, "toString: 모르는 서버 상태");
		return "?";
	}
}

// 이전 표본과 이번 표본으로 1초 값 - 누적치가 줄었으면(서버를 다시 시작) false: 호출하는 쪽이 기준만 바꾼다
static bool computeRate(const eServerKind kind, const MonSample& prev, const MonSample& cur, MonRateRow* outRow)
{
	Assert(outRow != nullptr, "computeRate: outRow가 nullptr");
	const int64_t elapsedMs = cur.UnixMs - prev.UnixMs;
	if (elapsedMs <= 0 || cur.Raw[RAW_ACCEPT] < prev.Raw[RAW_ACCEPT] || cur.Raw[RAW_PROC_CPU_100NS] < prev.Raw[RAW_PROC_CPU_100NS])
	{
		return false;
	}
	const double sec = static_cast<double>(elapsedMs) / 1000.0;
	MonRateRow& row = *outRow;
	row.UnixSec = (cur.UnixMs + 500) / 1000;
	float* const v = row.Values;
	v[MON_PROC_CPU] = static_cast<float>((cur.Raw[RAW_PROC_CPU_100NS] - prev.Raw[RAW_PROC_CPU_100NS]) / (static_cast<double>(elapsedMs) * 10000.0));
	v[MON_MEM_MB] = static_cast<float>(cur.Raw[RAW_PRIVATE_BYTES] / (1024.0 * 1024.0));
	v[MON_HANDLES] = static_cast<float>(cur.Raw[RAW_HANDLE_CNT]);
	const int64_t sysTotal = cur.Raw[RAW_SYS_TOTAL_100NS] - prev.Raw[RAW_SYS_TOTAL_100NS];
	v[MON_SYS_CPU_PCT] = (sysTotal > 0) ? static_cast<float>(100.0 * (cur.Raw[RAW_SYS_BUSY_100NS] - prev.Raw[RAW_SYS_BUSY_100NS]) / sysTotal) : 0.0f;
	v[MON_SYS_AVAIL_MB] = static_cast<float>(cur.Raw[RAW_SYS_AVAIL_MB]);
	v[MON_SESSION] = static_cast<float>(cur.Raw[RAW_SESSION]);

	// 누적 칸 → 초당 칸(같은 순서)
	constexpr int32_t RATE_PAIRS[][2] = {
		{ RAW_ACCEPT, MON_ACCEPT_PS }, { RAW_RECV_PACKET, MON_RECV_PACKET_PS }, { RAW_SEND_PACKET, MON_SEND_PACKET_PS },
		{ RAW_RECV_BYTES, MON_RECV_KB_PS }, { RAW_SEND_BYTES, MON_SEND_KB_PS }, { RAW_OVERFLOW, MON_OVERFLOW_PS },
		{ RAW_SEND_STALL, MON_SEND_STALL_PS }, { RAW_SESSION_FULL, MON_SESSION_FULL_PS }, { RAW_BAD_HEADER, MON_BAD_HEADER_PS },
		{ RAW_BAD_PACKET, MON_BAD_PACKET_PS },
	};
	for (const int32_t (&pair)[2] : RATE_PAIRS)
	{
		double perSec = (cur.Raw[pair[0]] - prev.Raw[pair[0]]) / sec;
		if (pair[1] == MON_RECV_KB_PS || pair[1] == MON_SEND_KB_PS)
		{
			perSec /= 1024.0;
		}
		v[pair[1]] = static_cast<float>(perSec);
	}
	for (int32_t i = 0; i < MON_CONTENT_CNT; ++i)
	{
		const int64_t now = cur.Raw[RAW_CONTENT + i];
		v[MON_CONTENT + i] = IsMonContentGauge(kind, i) ? static_cast<float>(now) : static_cast<float>((now - prev.Raw[RAW_CONTENT + i]) / sec);
	}
	return true;
}

MonitorServer::MonitorServer(const std::string& key, const int32_t maxSession, DbThread* db)
	: mKey(key)
	, mDb(db)
	, mSlots(static_cast<size_t>(maxSession), Slot{})
{
	Assert(db != nullptr && !key.empty(), "MonitorServer: db가 null이거나 키가 비었음");
}

MonitorServer::~MonitorServer()
{
	Shutdown(); // 파생 소멸자에서 - 기반 소멸자 시점엔 OnRelease가 순수 가상 호출이 된다
}

bool MonitorServer::StartMonitor(const NetConfig& config)
{
	Assert(config.MaxSession <= static_cast<int>(mSlots.size()), "StartMonitor: MaxSession이 생성자의 maxSession보다 큼");
	if (!Start(config))
	{
		return false;
	}
	mLastFlushMinute = getUnixMs() / 1000 / 60 * 60;
	mTick.Start(1000, std::bind(&MonitorServer::onTick, this));
	return true;
}

void MonitorServer::Shutdown()
{
	mTick.Stop(); // 워커 밖 스레드는 라이브러리 Stop 전에
	Stop();
	std::shared_ptr<MonSaveBatch> batch;
	{
		std::lock_guard<std::mutex> lock(mLock);
		if (mbShutdown)
		{
			return;
		}
		mbShutdown = true;
		batch = takeBatchLocked(INT64_MAX); // 남은 행 모두 - 앞 묶음이 아직 DB 대기열에 있어도 대기열은 순서대로라 그 뒤에 들어간다
	}
	if (batch != nullptr)
	{
		mDb->Post(std::bind(&MonitorServer::saveOnDbThread, this, std::placeholders::_1, batch));
	}
}

void MonitorServer::OnAccept(const uint64_t id, const SOCKADDR_IN& remoteAddr)
{
	std::lock_guard<std::mutex> lock(mLock);
	Slot& slot = getSlot(id);
	slot = Slot{ id, GetTickCount64(), eRole::None, -1, false, {} };
	inet_ntop(AF_INET, &remoteAddr.sin_addr, slot.IP, sizeof(slot.IP));
}

void MonitorServer::OnRelease(const uint64_t id)
{
	std::lock_guard<std::mutex> lock(mLock);
	Slot& slot = getSlot(id);
	if (slot.Role == eRole::Agent)
	{
		AgentState& agent = *mAgents[slot.AgentIdx];
		if (agent.SessionID == id)
		{
			agent.SessionID = 0;
			if (agent.State == eServerState::Online)
			{
				agent.State = eServerState::NoResponse; // Bye 없이 끊김 - 죽었거나 망이 끊겼다
				addEventLocked(agent.Name, "연결 끊김(Bye 없음)");
				sendStateLocked(0, agent);
			}
		}
	}
	slot = Slot{};
}

void MonitorServer::OnRecv(const uint64_t id, const char* payload, const int32_t len)
{
	if (len < static_cast<int32_t>(sizeof(eMonType)))
	{
		Disconnect(id);
		return;
	}
	eMonType type;
	MemCpy(&type, payload, sizeof(type));
	eRole role;
	{
		std::lock_guard<std::mutex> lock(mLock);
		role = getSlot(id).Role;
	}
	// 역할에 맞지 않는 패킷은 끊는다 - 키를 보내기 전의 다른 패킷, 보는 쪽의 표본 등
	if ((role == eRole::None && type != eMonType::AgentHello && type != eMonType::ViewerHello)
		|| (role == eRole::Agent && type != eMonType::Sample && type != eMonType::Bye)
		|| (role == eRole::Viewer && type != eMonType::HistoryReq))
	{
		Disconnect(id);
		return;
	}
	switch (type)
	{
	case eMonType::AgentHello:
		onAgentHello(id, payload, len);
		break;
	case eMonType::Sample:
		onSample(id, payload, len);
		break;
	case eMonType::Bye:
		onBye(id);
		break;
	case eMonType::ViewerHello:
		onViewerHello(id, payload, len);
		break;
	case eMonType::HistoryReq:
		onHistoryReq(id, payload, len);
		break;
	default:
		Disconnect(id);
		break;
	}
}

void MonitorServer::onAgentHello(const uint64_t id, const char* payload, const int32_t len)
{
	if (len != static_cast<int32_t>(sizeof(MonAgentHello)))
	{
		Disconnect(id);
		return;
	}
	MonAgentHello hello;
	MemCpy(&hello, payload, sizeof(hello));
	const std::string name = toText(hello.Name);
	if (toText(hello.Key) != mKey || !MonitorDB::IsValidName(name) || (hello.Kind != eServerKind::Login && hello.Kind != eServerKind::Game))
	{
		printf("[mon] 서버 접속 거절(키·이름·종류) - 끊음\n");
		Disconnect(id);
		return;
	}
	uint64_t oldSessionID = 0;
	{
		std::lock_guard<std::mutex> lock(mLock);
		int32_t index = findAgent(name);
		if (index < 0)
		{
			index = static_cast<int32_t>(mAgents.size());
			mAgents.push_back(MakeUnique<AgentState>());
			mAgents.back()->Name = name;
		}
		AgentState& agent = *mAgents[index];
		oldSessionID = (agent.SessionID != id) ? agent.SessionID : 0; // 같은 이름이 다시 붙음 - 옛 연결은 끊는다(반쯤 열린 채 남은 연결)
		agent.Kind = hello.Kind;
		agent.State = eServerState::Online;
		agent.SessionID = id;
		agent.LastSampleTick = GetTickCount64();
		agent.bHasPrev = false; // 다시 시작한 서버일 수 있다 - 누적치 기준을 새로
		Slot& slot = getSlot(id);
		slot.Role = eRole::Agent;
		slot.AgentIdx = index;
		addEventLocked(name, std::string("접속 ") + slot.IP);
		sendStateLocked(0, agent);
	}
	if (oldSessionID != 0)
	{
		Disconnect(oldSessionID); // 락 밖에서 - 끊긴 세션의 OnRelease가 이 락을 잡는다(그 OnRelease는 SessionID가 바뀌어 상태를 건드리지 않는다)
	}
}

void MonitorServer::onSample(const uint64_t id, const char* payload, const int32_t len)
{
	if (len != static_cast<int32_t>(sizeof(MonSample)))
	{
		Disconnect(id);
		return;
	}
	MonSample sample;
	MemCpy(&sample, payload, sizeof(sample));
	std::lock_guard<std::mutex> lock(mLock);
	AgentState& agent = *mAgents[getSlot(id).AgentIdx];
	if (agent.SessionID != id)
	{
		return; // 같은 이름의 새 연결이 이어받음 - 옛 연결의 표본은 버린다
	}
	agent.LastSampleTick = GetTickCount64();
	if (agent.State != eServerState::Online)
	{
		const bool bWasNoResponse = (agent.State == eServerState::NoResponse);
		agent.State = eServerState::Online;
		if (bWasNoResponse)
		{
			addEventLocked(agent.Name, "복구(표본 다시 받음)");
		}
		sendStateLocked(0, agent);
	}
	MonRateRow row{};
	if (agent.bHasPrev && computeRate(agent.Kind, agent.Prev, sample, &row))
	{
		pushRowLocked(&agent, row);
		agent.Last = row;
		agent.bHasLast = true;
		MonRate rate{};
		rate.Type = eMonType::Rate;
		copyText(rate.Name, agent.Name);
		rate.Row = row;
		broadcastLocked(reinterpret_cast<const char*>(&rate), sizeof(rate));
	}
	agent.Prev = sample;
	agent.bHasPrev = true;
}

void MonitorServer::onBye(const uint64_t id)
{
	std::lock_guard<std::mutex> lock(mLock);
	AgentState& agent = *mAgents[getSlot(id).AgentIdx];
	if (agent.SessionID == id)
	{
		agent.State = eServerState::Stopped;
		addEventLocked(agent.Name, "정상 종료(Bye)");
		sendStateLocked(0, agent);
	}
}

void MonitorServer::onViewerHello(const uint64_t id, const char* payload, const int32_t len)
{
	if (len != static_cast<int32_t>(sizeof(MonViewerHello)))
	{
		Disconnect(id);
		return;
	}
	MonViewerHello hello;
	MemCpy(&hello, payload, sizeof(hello));
	const bool bOk = (toText(hello.Key) == mKey);
	const MonHelloRes res{ eMonType::HelloRes, static_cast<uint8_t>(bOk ? 1 : 0) };
	SendPacket(id, reinterpret_cast<const char*>(&res), sizeof(res));
	if (!bOk)
	{
		printf("[mon] 보는 쪽 키 틀림 - 끊음\n");
		Disconnect(id);
		return;
	}
	std::lock_guard<std::mutex> lock(mLock);
	Slot& slot = getSlot(id);
	slot.Role = eRole::Viewer;
	printf("[mon] 보는 쪽 접속 %s\n", slot.IP);
	// 지금 상태 - 서버별 상태와 마지막 1초 값, 최근 이벤트(약 4KB라 송신 링에 들어간다)
	for (const UniquePtr<AgentState>& agent : mAgents)
	{
		sendStateLocked(id, *agent);
		if (agent->bHasLast)
		{
			MonRate rate{};
			rate.Type = eMonType::Rate;
			copyText(rate.Name, agent->Name);
			rate.Row = agent->Last;
			SendPacket(id, reinterpret_cast<const char*>(&rate), sizeof(rate));
		}
	}
	for (const MonEvent& event : mRecentEvents)
	{
		SendPacket(id, reinterpret_cast<const char*>(&event), sizeof(event));
	}
}

void MonitorServer::onHistoryReq(const uint64_t id, const char* payload, const int32_t len)
{
	if (len != static_cast<int32_t>(sizeof(MonHistoryReq)))
	{
		Disconnect(id);
		return;
	}
	HistoryJob job{ id, {} };
	MemCpy(&job.Req, payload, sizeof(job.Req));
	if (!MonitorDB::IsValidName(toText(job.Req.Name)) || job.Req.FromSec > job.Req.ToSec)
	{
		Disconnect(id);
		return;
	}
	{
		std::lock_guard<std::mutex> lock(mLock);
		Slot& slot = getSlot(id);
		if (slot.bHistoryBusy)
		{
			Disconnect(id); // 답을 받기 전에 또 요청 - 정상 클라는 하지 않는다
			return;
		}
		slot.bHistoryBusy = true;
	}
	mDb->Post(std::bind(&MonitorServer::historyOnDbThread, this, std::placeholders::_1, job));
}

void MonitorServer::historyOnDbThread(DbConn& conn, const HistoryJob job)
{
	const std::string name = toText(job.Req.Name);
	const bool bMinute = (job.Req.bMinute != 0);
	std::vector<MonRateRow> rows;
	MonitorDB::QueryHistory(conn, name, job.Req.FromSec, job.Req.ToSec, bMinute, HISTORY_ROW_MAX, &rows); // 실패하면 빈 답
	{
		std::lock_guard<std::mutex> lock(mLock);
		// 1초 원본은 아직 DB에 없는 최근 행(넣는 중인 묶음 + 저장 대기)을 메모리에서 채운다
		if (!bMinute && static_cast<int32_t>(rows.size()) < HISTORY_ROW_MAX)
		{
			const int64_t afterSec = rows.empty() ? job.Req.FromSec - 1 : rows.back().UnixSec;
			std::vector<MonRateRow> memoryRows;
			if (mInFlight != nullptr)
			{
				for (const MonAgentRows& agentRows : mInFlight->Agents)
				{
					if (agentRows.Name == name)
					{
						for (const MonRateRow& row : agentRows.Rows)
						{
							if (row.UnixSec > afterSec && row.UnixSec <= job.Req.ToSec)
							{
								memoryRows.push_back(row);
							}
						}
					}
				}
			}
			const int32_t index = findAgent(name);
			if (index >= 0)
			{
				for (const MonRateRow& row : mAgents[index]->Unsaved)
				{
					if (row.UnixSec > afterSec && row.UnixSec <= job.Req.ToSec)
					{
						memoryRows.push_back(row);
					}
				}
			}
			for (const MonRateRow& row : memoryRows)
			{
				if (static_cast<int32_t>(rows.size()) >= HISTORY_ROW_MAX)
				{
					break;
				}
				rows.push_back(row);
			}
		}
		Slot& slot = getSlot(job.SessionID);
		if (slot.SessionID == job.SessionID)
		{
			slot.bHistoryBusy = false; // 보내기 전에 내린다 - 클라가 답을 받자마자 다음 요청을 보내도 끊지 않게
		}
	}

	// 칸이 다른 세션으로 바뀌었으면 SendPacket이 버린다(세션 ID로 확인)
	const int64_t step = bMinute ? 60 : 1;
	const int64_t nextFromSec = (static_cast<int32_t>(rows.size()) >= HISTORY_ROW_MAX) ? rows.back().UnixSec + step : 0;
	size_t sent = 0;
	do
	{
		MonHistoryRes res{};
		res.Type = eMonType::HistoryRes;
		res.ReqID = job.Req.ReqID;
		const size_t cnt = (std::min)(rows.size() - sent, static_cast<size_t>(MON_HISTORY_ROWS)); // 괄호 - Windows.h의 min 매크로를 피한다
		res.RowCnt = static_cast<uint8_t>(cnt);
		for (size_t i = 0; i < cnt; ++i)
		{
			res.Rows[i] = rows[sent + i];
		}
		sent += cnt;
		res.bLast = (sent >= rows.size()) ? 1 : 0;
		res.NextFromSec = res.bLast ? nextFromSec : 0;
		const int32_t size = static_cast<int32_t>(sizeof(res) - sizeof(MonRateRow) * (MON_HISTORY_ROWS - cnt)); // 빈 행은 보내지 않는다
		SendPacket(job.SessionID, reinterpret_cast<const char*>(&res), size);
	} while (sent < rows.size());
}

void MonitorServer::onTick()
{
	std::vector<uint64_t> expired;
	std::vector<std::string> lines;
	std::shared_ptr<MonSaveBatch> batch;
	{
		std::lock_guard<std::mutex> lock(mLock);
		const ULONGLONG now = GetTickCount64();
		for (const Slot& slot : mSlots)
		{
			if (slot.SessionID != 0 && slot.Role == eRole::None && now - slot.AcceptTick > AUTH_LIMIT_MS)
			{
				expired.push_back(slot.SessionID);
			}
		}
		int32_t viewerCnt = 0;
		for (const Slot& slot : mSlots)
		{
			viewerCnt += (slot.SessionID != 0 && slot.Role == eRole::Viewer) ? 1 : 0;
		}
		size_t unsavedCnt = 0;
		for (const UniquePtr<AgentState>& agentPtr : mAgents)
		{
			AgentState& agent = *agentPtr;
			if (agent.State == eServerState::Online && now - agent.LastSampleTick > MON_NO_RESPONSE_SEC * 1000)
			{
				agent.State = eServerState::NoResponse;
				addEventLocked(agent.Name, "응답 없음(표본이 3초 넘게 없음)");
				sendStateLocked(0, agent);
			}
			unsavedCnt += agent.Unsaved.size();
			char line[256];
			const float* const v = agent.Last.Values;
			snprintf(line, sizeof(line), "[mon] %-6s %-8s cpu=%.2f 메모리=%.0fMB 세션=%.0f 수락/s=%.0f 받은KB/s=%.1f 보낸KB/s=%.1f PC cpu=%.0f%%",
				agent.Name.c_str(), toString(agent.State), v[MON_PROC_CPU], v[MON_MEM_MB], v[MON_SESSION], v[MON_ACCEPT_PS], v[MON_RECV_KB_PS],
				v[MON_SEND_KB_PS], v[MON_SYS_CPU_PCT]);
			lines.push_back(line);
		}
		char summary[200];
		snprintf(summary, sizeof(summary), "[mon] 보는쪽=%d 저장대기행=%zu DB대기=%lld 저장=%lld 저장실패=%lld 버린행=%lld", viewerCnt, unsavedCnt,
			static_cast<long long>(mDb->GetPendingCnt()), static_cast<long long>(mSaveCnt), static_cast<long long>(mSaveFailCnt),
			static_cast<long long>(mDroppedRowCnt));
		lines.push_back(summary);

		// 분이 바뀌면 지난 분까지의 행을 한 묶음으로(설계 4-3). 앞 묶음이 아직 DB에 들어가는 중이면 다음 초에 다시 본다
		const int64_t nowSec = getUnixMs() / 1000;
		const int64_t curMinute = nowSec / 60 * 60;
		if (mInFlight == nullptr && curMinute > mLastFlushMinute && !mbShutdown)
		{
			mLastFlushMinute = curMinute;
			batch = takeBatchLocked(curMinute);
			if (batch != nullptr && nowSec / 3600 != mLastRetentionHour)
			{
				mLastRetentionHour = nowSec / 3600;
				batch->bRetention = true;
			}
			mInFlight = batch;
		}
	}
	for (const uint64_t sessionID : expired)
	{
		printf("[mon] 인증 기한 초과 - 끊음\n");
		Disconnect(sessionID);
	}
	for (const std::string& line : lines)
	{
		printf("%s\n", line.c_str());
	}
	if (batch != nullptr)
	{
		mDb->Post(std::bind(&MonitorServer::saveOnDbThread, this, std::placeholders::_1, batch));
	}
}

void MonitorServer::saveOnDbThread(DbConn& conn, std::shared_ptr<MonSaveBatch> batch)
{
	std::vector<std::string> statements;
	MonitorDB::MakeSaveSQL(*batch, getUnixMs() / 1000, &statements);
	const bool bOk = statements.empty() || DbThread::RunTransaction(conn, statements, "모니터 저장");
	std::lock_guard<std::mutex> lock(mLock);
	if (bOk)
	{
		++mSaveCnt;
		if (mbSaveFailing)
		{
			mbSaveFailing = false;
			addEventLocked("monitor", "DB 저장 다시 됨");
		}
	}
	else
	{
		++mSaveFailCnt;
		returnBatchLocked(*batch);
		if (!mbSaveFailing)
		{
			mbSaveFailing = true;
			addEventLocked("monitor", "DB 저장 실패 - 행을 메모리에 두고 다음 분에 다시");
		}
	}
	if (mInFlight == batch)
	{
		mInFlight.reset();
	}
}

std::shared_ptr<MonSaveBatch> MonitorServer::takeBatchLocked(const int64_t beforeSec)
{
	std::shared_ptr<MonSaveBatch> batch(New<MonSaveBatch>(), Deleter());
	bool bEmpty = mUnsavedEvents.empty();
	for (const UniquePtr<AgentState>& agentPtr : mAgents)
	{
		AgentState& agent = *agentPtr;
		MonAgentRows rows;
		rows.Name = agent.Name;
		while (!agent.Unsaved.empty() && agent.Unsaved.front().UnixSec < beforeSec)
		{
			rows.Rows.push_back(agent.Unsaved.front());
			agent.Unsaved.pop_front();
		}
		bEmpty = bEmpty && rows.Rows.empty();
		batch->Agents.push_back(std::move(rows));
	}
	batch->Events.swap(mUnsavedEvents);
	if (bEmpty)
	{
		batch.reset();
	}
	return batch;
}

void MonitorServer::returnBatchLocked(const MonSaveBatch& batch)
{
	for (const MonAgentRows& rows : batch.Agents)
	{
		const int32_t index = findAgent(rows.Name);
		if (index < 0)
		{
			continue;
		}
		AgentState& agent = *mAgents[index];
		agent.Unsaved.insert(agent.Unsaved.begin(), rows.Rows.begin(), rows.Rows.end());
		while (agent.Unsaved.size() > UNSAVED_ROW_MAX)
		{
			agent.Unsaved.pop_front();
			++mDroppedRowCnt;
			if (!agent.bDropping)
			{
				agent.bDropping = true;
				addEventLocked(agent.Name, "저장 대기 30분 넘음 - 오래된 행부터 버림");
			}
		}
	}
	mUnsavedEvents.insert(mUnsavedEvents.begin(), batch.Events.begin(), batch.Events.end());
}

void MonitorServer::pushRowLocked(AgentState* agent, const MonRateRow& row)
{
	Assert(agent != nullptr, "pushRowLocked: agent가 nullptr");
	if (!agent->Unsaved.empty() && agent->Unsaved.back().UnixSec >= row.UnixSec)
	{
		agent->Unsaved.back() = row; // 같은 초에 표본 둘(타이머 흔들림) - 나중 것을 쓴다. 기본 키(서버, 초)라 DB에도 한 행
		return;
	}
	agent->Unsaved.push_back(row);
	if (agent->Unsaved.size() > UNSAVED_ROW_MAX)
	{
		agent->Unsaved.pop_front();
		++mDroppedRowCnt;
	}
	else
	{
		agent->bDropping = false;
	}
}

void MonitorServer::addEventLocked(const std::string& name, const std::string& text)
{
	MonEvent event{};
	event.Type = eMonType::Event;
	copyText(event.Name, name);
	event.UnixMs = getUnixMs();
	const std::string utf8 = toUtf8(text);
	copyUtf8(event.Text, utf8);
	printf("[mon] 이벤트 %s: %s\n", name.c_str(), text.c_str());
	mRecentEvents.push_back(event);
	if (mRecentEvents.size() > RECENT_EVENT_CNT)
	{
		mRecentEvents.pop_front();
	}
	mUnsavedEvents.push_back(MonEventRow{ name, event.UnixMs, utf8 });
	broadcastLocked(reinterpret_cast<const char*>(&event), sizeof(event));
}

void MonitorServer::broadcastLocked(const char* data, const int32_t len)
{
	for (const Slot& slot : mSlots)
	{
		if (slot.SessionID != 0 && slot.Role == eRole::Viewer)
		{
			SendPacket(slot.SessionID, data, len); // 느린 보는 쪽은 송신 링이 차면 라이브러리가 끊는다 - 다시 붙으면 된다
		}
	}
}

void MonitorServer::sendStateLocked(const uint64_t viewerID, const AgentState& agent)
{
	MonServerState state{};
	state.Type = eMonType::ServerState;
	copyText(state.Name, agent.Name);
	state.Kind = agent.Kind;
	state.State = agent.State;
	if (viewerID == 0)
	{
		broadcastLocked(reinterpret_cast<const char*>(&state), sizeof(state));
	}
	else
	{
		SendPacket(viewerID, reinterpret_cast<const char*>(&state), sizeof(state));
	}
}

MonitorServer::Slot& MonitorServer::getSlot(const uint64_t id)
{
	const uint32_t index = static_cast<uint32_t>(id); // 세션 ID의 하위 32비트가 인덱스
	Assert(index < mSlots.size(), "getSlot: 인덱스 범위 밖 - 생성자의 maxSession과 Start의 MaxSession이 다름");
	return mSlots[index];
}

int32_t MonitorServer::findAgent(const std::string& name) const
{
	for (size_t i = 0; i < mAgents.size(); ++i)
	{
		if (mAgents[i]->Name == name)
		{
			return static_cast<int32_t>(i);
		}
	}
	return -1;
}
