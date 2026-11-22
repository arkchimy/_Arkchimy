#include <WinSock2.h>
#include <WS2tcpip.h>
#include <Psapi.h>
#include <chrono>
#include <cstdio>
#include <memory>
#include "MonitorAgent.h"
#include "Assert.h"
#include "Memory.h"
#include "ParserLib.h"

enum : int32_t
{
	AGENT_RECONNECT_MS = 5000,     // 연결 실패·끊김 뒤 다시 붙기까지
	AGENT_CONNECT_TIMEOUT_MS = 1000,
};

static int64_t toInt64(const FILETIME& time)
{
	return (static_cast<int64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
}

// 고정 길이 칸에 넣는다 - 넘치면 잘라 널로 끝나게
template <size_t N>
static void copyText(char (&outText)[N], const std::string& text)
{
	MemSet(outText, 0, N);
	MemCpy(outText, text.data(), (text.size() < N) ? text.size() : N - 1);
}

static int64_t getUnixMs()
{
	FILETIME now;
	GetSystemTimeAsFileTime(&now);
	constexpr int64_t UNIX_EPOCH_100NS = 116444736000000000LL; // 1601-01-01 → 1970-01-01
	return (toInt64(now) - UNIX_EPOCH_100NS) / 10000;
}

MonitorAgent::~MonitorAgent()
{
	Stop();
}

bool MonitorAgent::LoadConfig(const char* serverCfgPath, const char* dbCfgPath, MonitorAgentConfig* outConfig)
{
	Assert(outConfig != nullptr, "MonitorAgent::LoadConfig: outConfig가 nullptr");
	MonitorAgentConfig& config = *outConfig;
	utility::stDebug debug;
	const std::unique_ptr<utility::Parser> serverCfg = utility::Parser::CreateParser(serverCfgPath, debug);
	if (serverCfg != nullptr)
	{
		serverCfg->Get("monitorHost", config.Host);
		serverCfg->Get("monitorPort", config.Port);
	}
	if (config.Port < 0 || config.Port > UINT16_MAX)
	{
		fprintf(stderr, "[main] server.cfg의 monitorPort는 0-65535\n");
		return false;
	}
	const std::unique_ptr<utility::Parser> dbCfg = utility::Parser::CreateParser(dbCfgPath, debug);
	if (dbCfg != nullptr)
	{
		dbCfg->Get("monitorKey", config.Key);
	}
	if (config.Key.size() >= MON_KEY_LEN)
	{
		fprintf(stderr, "[main] db.cfg의 monitorKey는 %d바이트 미만이어야 한다\n", MON_KEY_LEN);
		return false;
	}
	if (config.Port == 0 || config.Host.empty() || config.Key.empty())
	{
		config.Port = 0;
		printf("[main] 모니터 보내기 꺼짐(server.cfg monitorHost·monitorPort, db.cfg monitorKey)\n");
	}
	else
	{
		printf("[main] 모니터 %s:%d로 1초 표본을 보낸다\n", config.Host.c_str(), config.Port);
	}
	return true;
}

void MonitorAgent::FillProcessValues(MonSample* outSample)
{
	Assert(outSample != nullptr, "FillProcessValues: outSample이 nullptr");
	MonSample& sample = *outSample;
	FILETIME created;
	FILETIME exited;
	FILETIME kernel;
	FILETIME user;
	GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
	sample.Raw[RAW_PROC_CPU_100NS] = toInt64(kernel) + toInt64(user);

	PROCESS_MEMORY_COUNTERS_EX memory{};
	GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory));
	sample.Raw[RAW_PRIVATE_BYTES] = static_cast<int64_t>(memory.PrivateUsage);
	DWORD handleCnt = 0;
	GetProcessHandleCount(GetCurrentProcess(), &handleCnt);
	sample.Raw[RAW_HANDLE_CNT] = handleCnt;

	FILETIME sysIdle;
	FILETIME sysKernel;
	FILETIME sysUser;
	GetSystemTimes(&sysIdle, &sysKernel, &sysUser);
	const int64_t total = toInt64(sysKernel) + toInt64(sysUser); // 커널 시간은 유휴를 포함한다
	sample.Raw[RAW_SYS_TOTAL_100NS] = total;
	sample.Raw[RAW_SYS_BUSY_100NS] = total - toInt64(sysIdle);

	MEMORYSTATUSEX status{};
	status.dwLength = sizeof(status);
	GlobalMemoryStatusEx(&status);
	sample.Raw[RAW_SYS_AVAIL_MB] = static_cast<int64_t>(status.ullAvailPhys / (1024 * 1024));
}

void MonitorAgent::FillNetValues(const NetStat& stat, const int64_t sessionCnt, MonSample* outSample)
{
	Assert(outSample != nullptr, "FillNetValues: outSample이 nullptr");
	MonSample& sample = *outSample;
	sample.Raw[RAW_SESSION] = sessionCnt;
	sample.Raw[RAW_ACCEPT] = stat.AcceptCnt;
	sample.Raw[RAW_RECV_PACKET] = stat.RecvPacketCnt;
	sample.Raw[RAW_SEND_PACKET] = stat.SendPacketCnt;
	sample.Raw[RAW_RECV_BYTES] = stat.RecvBytes;
	sample.Raw[RAW_SEND_BYTES] = stat.SendBytes;
	sample.Raw[RAW_OVERFLOW] = stat.OverflowCnt;
	sample.Raw[RAW_SEND_STALL] = stat.SendStallCnt;
	sample.Raw[RAW_SESSION_FULL] = stat.SessionFullCnt;
	sample.Raw[RAW_BAD_HEADER] = stat.BadPacketCnt;
}

void MonitorAgent::Start(const MonitorAgentConfig& config)
{
	Assert(!mThread.joinable(), "MonitorAgent::Start: 이미 시작함");
	if (config.Port == 0)
	{
		return;
	}
	mConfig = config;
	mbStopping = false;
	mThread = std::thread(&MonitorAgent::run, this);
}

void MonitorAgent::Stop()
{
	{
		std::lock_guard<std::mutex> lock(mLock);
		mbStopping = true;
	}
	mWake.notify_one();
	if (mThread.joinable())
	{
		mThread.join();
	}
}

void MonitorAgent::Post(const MonSample& sample)
{
	if (mConfig.Port == 0)
	{
		return;
	}
	{
		std::lock_guard<std::mutex> lock(mLock);
		mPending = sample;
		mPending.Type = eMonType::Sample;
		mPending.UnixMs = getUnixMs();
		mbHasPending = true;
	}
	mWake.notify_one();
}

void MonitorAgent::run()
{
	std::unique_lock<std::mutex> lock(mLock);
	while (true)
	{
		while (!mbStopping && !mbHasPending)
		{
			mWake.wait(lock);
		}
		if (mbStopping)
		{
			break;
		}
		const MonSample sample = mPending;
		mbHasPending = false;
		lock.unlock(); // 연결·보내기는 락 밖에서 - Post가 기다리지 않게

		const ULONGLONG now = GetTickCount64();
		if (mSock == INVALID_SOCKET && (mLastConnectTick == 0 || now - mLastConnectTick >= AGENT_RECONNECT_MS))
		{
			mLastConnectTick = now;
			connectOnce();
		}
		if (mSock != INVALID_SOCKET && !sendPacket(sample))
		{
			printf("[monitor] 모니터로 보내기 실패 err=%d - 끊고 %d초 뒤 다시 붙는다\n", WSAGetLastError(), AGENT_RECONNECT_MS / 1000);
			closeSocket();
		}
		lock.lock();
	}
	lock.unlock();
	if (mSock != INVALID_SOCKET)
	{
		const MonBye bye{ eMonType::Bye };
		sendPacket(bye);
		shutdown(mSock, SD_SEND); // FIN - 모니터가 Bye를 읽은 뒤 끊김을 보게(바로 RST면 Bye가 버려질 수 있다)
		closeSocket();
	}
}

bool MonitorAgent::connectOnce()
{
	Assert(mSock == INVALID_SOCKET, "connectOnce: 이미 연결됨");
	SOCKADDR_IN addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(static_cast<u_short>(mConfig.Port));
	if (inet_pton(AF_INET, mConfig.Host.c_str(), &addr.sin_addr) != 1)
	{
		printf("[monitor] monitorHost가 IPv4 주소가 아님 - %s\n", mConfig.Host.c_str());
		return false;
	}
	mSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (mSock == INVALID_SOCKET)
	{
		return false;
	}
	u_long bNonBlocking = 1;
	ioctlsocket(mSock, FIONBIO, &bNonBlocking);
	bool bOk = (connect(mSock, reinterpret_cast<const SOCKADDR*>(&addr), sizeof(addr)) == 0);
	if (!bOk && WSAGetLastError() == WSAEWOULDBLOCK)
	{
		fd_set writeSet;
		FD_ZERO(&writeSet);
		FD_SET(mSock, &writeSet);
		fd_set errorSet = writeSet;
		const timeval timeout{ 0, AGENT_CONNECT_TIMEOUT_MS * 1000 };
		bOk = (select(0, nullptr, &writeSet, &errorSet, &timeout) == 1 && FD_ISSET(mSock, &writeSet));
	}
	MonAgentHello hello{};
	hello.Type = eMonType::AgentHello;
	copyText(hello.Key, mConfig.Key);
	copyText(hello.Name, mConfig.Name);
	hello.Kind = mConfig.Kind;
	if (!bOk || !sendPacket(hello))
	{
		if (!mbEverConnected)
		{
			printf("[monitor] 모니터 %s:%d 연결 실패 - %d초마다 다시 시도\n", mConfig.Host.c_str(), mConfig.Port, AGENT_RECONNECT_MS / 1000);
			mbEverConnected = true; // 실패도 처음 한 번만 알린다 - 모니터 없이 도는 서버의 출력을 덮지 않게
		}
		closeSocket();
		return false;
	}
	printf("[monitor] 모니터 %s:%d 연결\n", mConfig.Host.c_str(), mConfig.Port);
	mbEverConnected = true;
	return true;
}

template <typename T>
bool MonitorAgent::sendPacket(const T& body)
{
	char buf[PACKET_HEADER_SIZE + sizeof(T)];
	const PacketHeader header{ static_cast<uint16_t>(sizeof(T)) };
	MemCpy(buf, &header, PACKET_HEADER_SIZE);
	MemCpy(buf + PACKET_HEADER_SIZE, &body, sizeof(T));
	return sendAll(buf, static_cast<int32_t>(sizeof(buf)));
}

bool MonitorAgent::sendAll(const char* data, const int32_t len)
{
	// 비블록 소켓 - 커널 송신 버퍼가 가득 차면(모니터가 오래 읽지 않음) 일부만 나가거나 WSAEWOULDBLOCK. 둘 다 연결을 버린다
	// (일부만 나간 패킷의 나머지를 붙들고 있으면 1초 통계 스레드를 기다리게 하거나 다음 표본과 섞인다)
	const int ret = send(mSock, data, len, 0);
	return ret == len;
}

void MonitorAgent::closeSocket()
{
	if (mSock != INVALID_SOCKET)
	{
		closesocket(mSock);
		mSock = INVALID_SOCKET;
	}
}
