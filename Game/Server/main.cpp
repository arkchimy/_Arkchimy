#include <atomic>
#include <conio.h>
#include <cstdio>
#include <iostream>
#include <thread>
#include "GameServer.h"
#include "CrushDump_lib.h" // GameServer.h(WinSock2.h) 뒤에 - 이 헤더가 Windows.h를 먼저 끌어오면 WinSock2.h와 재정의 오류가 난다

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
	GameServer server(config.MaxSession);
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
	server.Stop();
	printf("[main] 종료 완료\n");
	return 0;
}
