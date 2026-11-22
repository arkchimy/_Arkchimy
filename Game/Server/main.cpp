#include <atomic>
#include <conio.h>
#include <cstdio>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include "GameServer.h"
#include "AdminServer.h"
#include "CrushDump_lib.h" // GameServer.h(WinSock2.h) 뒤에 - 이 헤더가 Windows.h를 먼저 끌어오면 WinSock2.h와 재정의 오류가 난다
#include "DbThread.h"
#include "GameDB.h"
#include "GameRule.h"
#include "MonitorAgent.h"
#include "ParserLib.h"
#include "RedisClient.h"
#include "SessionAuth.h"

// server.cfg에서 게임 서버 규칙을 읽는다 - 키가 없으면 기본값. 밖에서 오는 값이라 범위가 틀리면 알리고 false
static bool loadGameRule(const char* path, GameRule* outRule)
{
	utility::stDebug debug;
	const std::unique_ptr<utility::Parser> parser = utility::Parser::CreateParser(path, debug);
	if (parser == nullptr)
	{
		fprintf(stderr, "[main] %s를 읽지 못함 err=%d key=%s\n", path, debug.err, debug.lastKey.c_str());
		return false;
	}
	GameRule& rule = *outRule;
	parser->Get("entryLimitSec", rule.EntryLimitSec);
	if (rule.EntryLimitSec < 0 || rule.EntryLimitSec > 3600)
	{
		fprintf(stderr, "[main] 규칙 범위 오류 - entryLimitSec 0~3600\n");
		return false;
	}
	printf("[main] 규칙: 입장 기한 %d초\n", rule.EntryLimitSec);
	return true;
}
// db.cfg(.gitignore - 비밀번호가 있어 저장소에 올리지 않는다)에서 DB 접속 설정을 읽는다. 비밀번호는 출력하지 않는다
// adminKey(선택): 운영 툴이 운영자 포트에 처음 보내는 키. 없으면 운영자 포트를 열지 않는다
static bool loadDbConfig(const char* path, DbConfig* outConfig, RedisConfig* outRedis, std::string* outAdminKey)
{
	Assert(outConfig != nullptr && outRedis != nullptr && outAdminKey != nullptr, "loadDbConfig: out 매개변수가 null");
	utility::stDebug debug;
	const std::unique_ptr<utility::Parser> parser = utility::Parser::CreateParser(path, debug);
	if (parser == nullptr)
	{
		fprintf(stderr, "[main] %s를 읽지 못함 err=%d - db.cfg.example을 복사해 값을 채운다\n", path, debug.err);
		return false;
	}
	DbConfig& config = *outConfig;
	if (!parser->Get("host", config.Host) || !parser->Get("port", config.Port) || !parser->Get("user", config.User)
		|| !parser->Get("password", config.Password) || !parser->Get("database", config.Database))
	{
		fprintf(stderr, "[main] %s에 host·port·user·password·database가 모두 있어야 한다\n", path);
		return false;
	}
	if (!LoadRedisConfig(*parser, outRedis))
	{
		fprintf(stderr, "[main] %s에 redisHost·redisPort·redisPassword가 있어야 한다(db.cfg.example 참고)\n", path);
		return false;
	}
	printf("[main] DB: %s@%s:%d/%s, Redis: %s:%d\n", config.User.c_str(), config.Host.c_str(), config.Port, config.Database.c_str(),
		outRedis->Host.c_str(), outRedis->Port);
	parser->Get("adminKey", *outAdminKey);
	if (outAdminKey->size() >= ADMIN_KEY_LEN)
	{
		fprintf(stderr, "[main] adminKey는 %d바이트 미만이어야 한다\n", ADMIN_KEY_LEN);
		return false;
	}
	return true;
}

// 시각과 프로세스 자원 - 장시간 시험에서 언제 무엇이 바뀌었는지, 메모리·핸들이 조금씩 새는지 본다
static void printProcessInfo(std::ostream& out)
{
	SYSTEMTIME now;
	GetLocalTime(&now);
	PROCESS_MEMORY_COUNTERS_EX memory{};
	GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory));
	DWORD handleCnt = 0;
	GetProcessHandleCount(GetCurrentProcess(), &handleCnt);

	char buf[96];
	snprintf(buf, sizeof(buf), "[%02u:%02u:%02u] 메모리=%lluMB 핸들=%lu ", now.wHour, now.wMinute, now.wSecond,
		static_cast<unsigned long long>(memory.PrivateUsage / (1024 * 1024)), handleCnt);
	out << buf;
}

// 모니터링 서버로 보낼 표본 - 컨텐츠 칸 순서는 MON_GAME_CONTENT_LABELS와 같다
static void fillSample(const GameServer& server, MonSample* outSample)
{
	Assert(outSample != nullptr, "fillSample: outSample이 nullptr");
	MonSample& sample = *outSample;
	MonitorAgent::FillProcessValues(&sample);
	MonitorAgent::FillNetValues(server.GetStat(), server.GetSessionCnt(), &sample);
	sample.Raw[RAW_BAD_PACKET] = server.GetBadPacketCnt();
	int64_t* const content = &sample.Raw[RAW_CONTENT];
	content[0] = server.GetOnlineCnt();
	content[1] = server.GetRoomMemberCnt();
	content[2] = server.GetChatCnt();
	content[3] = server.GetDbPendingCnt();
	content[4] = server.GetDbFailCnt();
	content[5] = server.GetEntryTimeoutCnt();
	content[6] = server.GetEntryAuthFailCnt();
	content[7] = server.GetEntryRejectCnt();
}

// 모니터링 스레드 - 1초마다 서버 상태를 한 줄 출력하고 모니터링 서버로 표본을 넘긴다(넘기기만 - 보내기는 MonitorAgent 스레드)
// std::endl은 줄바꿈과 함께 출력을 바로 내보낸다 - 출력을 파일로 받을 때 강제 종료돼도 줄이 남게
static void runMonitorLoop(const GameServer& server, MonitorAgent* agent, const std::atomic<bool>& bMonitorRunning)
{
	while (bMonitorRunning)
	{
		Sleep(1000);
		printProcessInfo(std::cout);
		std::cout << server << std::endl;
		MonSample sample{};
		fillSample(server, &sample);
		agent->Post(sample);
	}
}

int main()
{
	// 처리되지 않은 예외(Assert·crashOn*의 __debugbreak 포함)가 나면 작업 폴더에 덤프를 쓰고 끝낸다 - 프로세스 전체에 걸리므로 DLL 워커 스레드도 잡는다
	CDump dump;

	const WsadataRAII wsa; // WSAStartup / WSACleanup
	NetConfig config;
	NetError configError;
	if (!LoadNetConfig("server.cfg", &config, &configError))
	{
		// 설정 파일은 밖에서 오는 값이라 멈추지 않고 알리고 끝낸다(디버거 선택 창이 떠서 기다리지 않게)
		fprintf(stderr, "[main] server.cfg를 읽지 못함 - 단계=%s 코드=%d (실행 파일 옆이나 작업 폴더에 server.cfg가 있는지 확인)\n",
			configError.Step != nullptr ? configError.Step : "?", configError.Code);
		return 1;
	}
	GameRule rule;
	if (!loadGameRule("server.cfg", &rule))
	{
		return 1;
	}
	// DB - 연결하고 테이블을 확인한 뒤 DB 스레드를 띄운다(서버보다 먼저 만들고 늦게 사라진다)
	if (!InitCrypto())
	{
		fprintf(stderr, "[main] libsodium 초기화 실패\n");
		return 1;
	}
	DbConfig dbConfig;
	RedisConfig redisConfig;
	std::string adminKey;
	if (!loadDbConfig("db.cfg", &dbConfig, &redisConfig, &adminKey))
	{
		return 1;
	}
	MonitorAgentConfig agentConfig;
	agentConfig.Name = "game";
	agentConfig.Kind = eServerKind::Game;
	if (!MonitorAgent::LoadConfig("server.cfg", "db.cfg", &agentConfig))
	{
		return 1;
	}
	int32_t adminPort = 0; // server.cfg의 adminPort - 0이거나 운영자 키가 없으면 운영자 포트를 열지 않는다
	{
		utility::stDebug debug;
		const std::unique_ptr<utility::Parser> parser = utility::Parser::CreateParser("server.cfg", debug);
		if (parser != nullptr)
		{
			parser->Get("adminPort", adminPort);
		}
	}
	DbThread dbThread;
	std::string dbError;
	if (!dbThread.Connect(dbConfig, &dbError) || !GameDB::Prepare(dbThread.GetStartupConn(), &dbError))
	{
		fprintf(stderr, "[main] DB 준비 실패 - %s\n", dbError.c_str());
		return 1;
	}
	printf("[main] DB 준비 완료\n");
	RedisClient redis; // 연결은 여기서, 쓰는 것은 DB 스레드만(스레드를 띄우기 전에 넘긴다)
	if (!redis.Connect(redisConfig, &dbError))
	{
		fprintf(stderr, "[main] Redis 연결 실패 - %s\n", dbError.c_str());
		return 1;
	}
	dbThread.StartThread();
	GameDB gameDB(&dbThread);

	GameServer server(config.MaxSession, rule, &gameDB, &redis);
	if (!server.Start(config))
	{
		// 멈추기 전에 이유를 남긴다 - 포트 사용 중(bind 10048) 같은 환경 문제를 덤프를 열지 않아도 알 수 있게(README_ERRORS 1-33)
		const NetError startError = server.GetNetError();
		fprintf(stderr, "[main] 서버 시작 실패 - 단계=%s 코드=%d\n", startError.Step != nullptr ? startError.Step : "?", startError.Code);
		const volatile NetError error = startError; // 덤프에서 실패한 단계·코드를 본다 - volatile은 쓰지 않는 값을 릴리즈 최적화가 지우지 않게
		__debugbreak();
		return 1;
	}

	// 운영자 포트 - 같은 라이브러리로 두 번째 서버 객체(설계 3-5). 접속은 운영 툴 몇 개뿐이라 작게
	UniquePtr<AdminServer> admin;
	if (adminPort > 0 && adminPort <= UINT16_MAX && !adminKey.empty())
	{
		NetConfig adminConfig = config;
		adminConfig.Port = static_cast<USHORT>(adminPort);
		adminConfig.bLoopbackOnly = true; // 같은 PC의 운영 툴만
		adminConfig.WorkerCnt = 1;
		adminConfig.SlotCnt = 1;
		adminConfig.MaxSession = 4;
		admin = MakeUnique<AdminServer>(&server, adminKey, adminConfig.MaxSession);
		if (!admin->StartAdmin(adminConfig))
		{
			const NetError error = admin->GetNetError();
			fprintf(stderr, "[main] 운영자 포트 %d 시작 실패 - 단계=%s 코드=%d\n", adminPort, error.Step != nullptr ? error.Step : "?", error.Code);
			admin.reset();
		}
		else
		{
			printf("[main] 운영자 포트 %d\n", adminPort);
		}
	}
	else
	{
		printf("[main] 운영자 포트 꺼짐(server.cfg adminPort, db.cfg adminKey)\n");
	}

	MonitorAgent agent;
	agent.Start(agentConfig);

	std::atomic<bool> bMonitorRunning = true;
	std::thread monitorThread(runMonitorLoop, std::cref(server), &agent, std::cref(bMonitorRunning));

	while (server.IsRunning())
	{
		if (_kbhit())
		{
			const int key = _getch();
			if (key == 'q' || key == 'Q')
			{
				break;
			}
		}
		Sleep(100);
	}

	bMonitorRunning = false;
	monitorThread.join(); // 진행 중인 1초 대기가 끝날 때까지 최대 1초
	agent.Stop();         // Bye - 모니터가 응답 없음이 아니라 정상 종료로 기록한다

	// Stop이 남은 세션을 모두 끊고 해제하므로, "클라 종료 후 세션 0" 판정은 Stop 전의 값으로 한다
	std::cout << "[main] 종료 전 ";
	printProcessInfo(std::cout);
	std::cout << server << std::endl;
	server.Shutdown(); // 입장 기한 감시 → 새 패킷·운영 명령 막기 → DB 남은 일(운영 명령 결과는 아직 도는 운영자 서버로 나간다) → 라이브러리 Stop
	if (admin != nullptr)
	{
		admin->Shutdown(); // 인증 기한 감시 스레드 → 라이브러리 Stop
	}
	printf("[main] 종료 완료\n");
	return 0;
}
