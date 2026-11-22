#include <conio.h>
#include <cstdio>
#include <memory>
#include <string>
#include "MonitorServer.h"
#include "CrushDump_lib.h" // MonitorServer.h(WinSock2.h) 뒤에 - Windows.h를 먼저 끌어오면 WinSock2.h와 재정의 오류가 난다
#include "DbThread.h"
#include "ParserLib.h"

// db.cfg(.gitignore)에서 DB 접속과 monitorKey를 읽는다 - 게임 서버와 같은 파일. 비밀번호·키는 출력하지 않는다
static bool loadDbConfig(const char* path, DbConfig* outConfig, std::string* outKey)
{
	Assert(outConfig != nullptr && outKey != nullptr, "loadDbConfig: out 매개변수가 null");
	utility::stDebug debug;
	const std::unique_ptr<utility::Parser> parser = utility::Parser::CreateParser(path, debug);
	if (parser == nullptr)
	{
		fprintf(stderr, "[main] %s를 읽지 못함 err=%d - Game/Server/db.cfg.example을 복사해 값을 채운다\n", path, debug.err);
		return false;
	}
	DbConfig& config = *outConfig;
	if (!parser->Get("host", config.Host) || !parser->Get("port", config.Port) || !parser->Get("user", config.User)
		|| !parser->Get("password", config.Password) || !parser->Get("database", config.Database))
	{
		fprintf(stderr, "[main] %s에 host·port·user·password·database가 모두 있어야 한다\n", path);
		return false;
	}
	if (!parser->Get("monitorKey", *outKey) || outKey->empty() || outKey->size() >= MON_KEY_LEN)
	{
		fprintf(stderr, "[main] %s에 monitorKey(1-%d바이트)가 있어야 한다\n", path, MON_KEY_LEN - 1);
		return false;
	}
	printf("[main] DB: %s@%s:%d/%s\n", config.User.c_str(), config.Host.c_str(), config.Port, config.Database.c_str());
	return true;
}

int main()
{
	CDump dump; // 처리되지 않은 예외(Assert 포함)면 작업 폴더에 덤프
	setvbuf(stdout, nullptr, _IONBF, 0); // 출력을 파일로 받을 때 강제 종료돼도 이벤트 줄이 남게

	const WsadataRAII wsa;
	NetConfig config;
	NetError configError;
	if (!LoadNetConfig("monitor.cfg", &config, &configError))
	{
		fprintf(stderr, "[main] monitor.cfg를 읽지 못함 - 단계=%s 코드=%d\n", configError.Step != nullptr ? configError.Step : "?", configError.Code);
		return 1;
	}
	DbConfig dbConfig;
	std::string key;
	if (!loadDbConfig("db.cfg", &dbConfig, &key))
	{
		return 1;
	}
	DbThread dbThread; // 모니터보다 먼저 만들고 늦게 사라진다
	std::string dbError;
	if (!dbThread.Connect(dbConfig, &dbError) || !MonitorDB::Prepare(dbThread.GetStartupConn(), &dbError))
	{
		fprintf(stderr, "[main] DB 준비 실패 - %s\n", dbError.c_str());
		return 1;
	}
	dbThread.StartThread();

	MonitorServer server(key, config.MaxSession, &dbThread);
	if (!server.StartMonitor(config))
	{
		const NetError error = server.GetNetError();
		fprintf(stderr, "[main] 모니터 서버 시작 실패 - 단계=%s 코드=%d\n", error.Step != nullptr ? error.Step : "?", error.Code);
		return 1;
	}
	printf("[main] 모니터 서버 포트 %d - 1초 값을 콘솔·보는 쪽에, 1분마다 DB에(q로 종료)\n", config.Port);

	while (server.IsRunning())
	{
		if (_kbhit())
		{
			const int pressed = _getch();
			if (pressed == 'q' || pressed == 'Q')
			{
				break;
			}
		}
		Sleep(100);
	}
	server.Shutdown(); // 감시 스레드 → 라이브러리 Stop → 남은 행을 DB 대기열에
	dbThread.Stop();   // 남은 저장을 마치고 끝낸다
	printf("[main] 종료 완료\n");
	return 0;
}
