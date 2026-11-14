#include <atomic>
#include <conio.h>
#include <cstdio>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include "GameServer.h"
#include "CrushDump_lib.h" // GameServer.h(WinSock2.h) 뒤에 - 이 헤더가 Windows.h를 먼저 끌어오면 WinSock2.h와 재정의 오류가 난다
#include "DbThread.h"
#include "GameData.h"
#include "GameDb.h"
#include "GameRule.h"
#include "ItemUID.h"
#include "ParserLib.h"

// server.cfg에서 게임 규칙을 읽는다 - 키가 없으면 기본값. 밖에서 오는 값이라 범위가 틀리면 알리고 false
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
	parser->Get("playerHp", rule.PlayerHp);
	parser->Get("playerAttack", rule.PlayerAttack);
	parser->Get("playerDefense", rule.PlayerDefense);
	parser->Get("playerInvestigate", rule.PlayerInvestigate);
	parser->Get("turnTimeMs", rule.TurnTimeMs);
	parser->Get("dungeonGold", rule.DungeonGold);
	parser->Get("auctionDurationSec", rule.AuctionDurationSec);
	// 판정 값이 프로토콜 필드(int8 보너스, uint8 기준값, int16 체력)를 넘지 않게 - 장비 4칸(각 최대 20)이 더해져도 안에 들어가는 범위
	if (rule.PlayerHp <= 0 || rule.PlayerHp > 1000 || rule.TurnTimeMs < 1000 || rule.TurnTimeMs > UINT16_MAX || rule.DungeonGold < 0
		|| rule.PlayerAttack < 0 || rule.PlayerAttack > 20 || rule.PlayerDefense < 0 || rule.PlayerDefense > 40 || rule.PlayerInvestigate < 0
		|| rule.PlayerInvestigate > 20 || rule.AuctionDurationSec < 5 || rule.AuctionDurationSec > 3600)
	{
		fprintf(stderr, "[main] 게임 규칙 범위 오류 - playerHp 1~1000, turnTimeMs 1000~%d, dungeonGold 0 이상, playerAttack·playerInvestigate 0~20, playerDefense 0~40, auctionDurationSec 5~3600\n",
			UINT16_MAX);
		return false;
	}
	printf("[main] 규칙: 체력 %d, 공격 +%d, 방어 %d, 조사 +%d, 턴 %dms, 던전 골드 %d, 경매 %d초\n", rule.PlayerHp, rule.PlayerAttack, rule.PlayerDefense,
		rule.PlayerInvestigate, rule.TurnTimeMs, rule.DungeonGold, rule.AuctionDurationSec);
	return true;
}

// db.cfg(.gitignore - 비밀번호가 있어 저장소에 올리지 않는다)에서 DB 접속 설정을 읽는다. 비밀번호는 출력하지 않는다
static bool loadDbConfig(const char* path, DbConfig* outConfig)
{
	Assert(outConfig != nullptr, "loadDbConfig: outConfig가 null");
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
	printf("[main] DB: %s@%s:%d/%s\n", config.User.c_str(), config.Host.c_str(), config.Port, config.Database.c_str());
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

// 모니터링 스레드 - 1초마다 서버 상태를 한 줄 출력한다
// std::endl은 줄바꿈과 함께 출력을 바로 내보낸다 - 출력을 파일로 받을 때 강제 종료돼도 줄이 남게
static void runMonitorLoop(const GameServer& server, const std::atomic<bool>& bMonitorRunning)
{
	while (bMonitorRunning)
	{
		Sleep(1000);
		printProcessInfo(std::cout);
		std::cout << server << std::endl;
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
	GameData gameData; // 시나리오 데이터 - 서버가 끝날 때까지 읽기만 한다(GameServer보다 먼저 만들고 늦게 사라진다)
	std::string dataError;
	if (!gameData.Load("Data", &dataError))
	{
		fprintf(stderr, "[main] 시나리오 데이터를 읽지 못함 - %s\n", dataError.c_str());
		return 1;
	}
	printf("[main] 시나리오 데이터: 아이템 %zu개, 방 %zu개\n", gameData.GetItems().size(), gameData.GetRooms().size());

	// DB - 연결하고 테이블·재시작 정리·번호 시작값을 끝낸 뒤 DB 스레드를 띄운다(서버보다 먼저 만들고 늦게 사라진다)
	DbConfig dbConfig;
	if (!loadDbConfig("db.cfg", &dbConfig))
	{
		return 1;
	}
	DbThread dbThread;
	std::string dbError;
	uint64_t lastItemUID = 0;
	uint64_t lastAuctionID = 0;
	if (!dbThread.Connect(dbConfig, &dbError) || !GameDb::Prepare(dbThread.GetStartupConn(), &lastItemUID, &lastAuctionID, &dbError))
	{
		fprintf(stderr, "[main] DB 준비 실패 - %s\n", dbError.c_str());
		return 1;
	}
	SetLastItemUID(lastItemUID);
	printf("[main] DB 준비 완료 - 다음 아이템 번호 %llu, 다음 경매 번호 %llu\n", static_cast<unsigned long long>(lastItemUID + 1),
		static_cast<unsigned long long>(lastAuctionID + 1));
	dbThread.StartThread();
	GameDb gameDb(&dbThread);

	GameServer server(config.MaxSession, &gameData, rule, &gameDb, lastAuctionID);
	if (!server.Start(config))
	{
		const volatile NetError error = server.GetNetError(); // 덤프에서 실패한 단계·코드를 본다 - volatile은 쓰지 않는 값을 릴리즈 최적화가 지우지 않게
		__debugbreak();
		return 1;
	}

	std::atomic<bool> bMonitorRunning = true;
	std::thread monitorThread(runMonitorLoop, std::cref(server), std::cref(bMonitorRunning));

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

	// Stop이 남은 세션을 모두 끊고 해제하므로, "클라 종료 후 세션 0" 판정은 Stop 전의 값으로 한다
	std::cout << "[main] 종료 전 ";
	printProcessInfo(std::cout);
	std::cout << server << std::endl;
	server.Shutdown(); // 타이머 → 새 패킷 막기 → DB 남은 저장 → 라이브러리 Stop
	printf("[main] 종료 완료\n");
	return 0;
}
