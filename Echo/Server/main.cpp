#include <atomic>
#include <conio.h>
#include <cstdio>
#include <iostream>
#include <thread>
#include "EchoServer.h"
#include "CrushDump_lib.h" // EchoServer.h(WinSock2.h) 뒤에 - 이 헤더가 Windows.h를 먼저 끌어오면 WinSock2.h와 재정의 오류가 난다

// 모니터링 스레드 - 1초마다 서버 상태를 한 줄 출력한다
// std::endl은 줄바꿈과 함께 출력을 바로 내보낸다 - 출력을 파일로 받을 때 강제 종료돼도 줄이 남게
static void runMonitorLoop(const EchoServer& server, const std::atomic<bool>& bMonitorRunning)
{
	while (bMonitorRunning)
	{
		Sleep(1000);
		std::cout << server << std::endl;
	}
}

int main(int argc, char* argv[])
{
	// 처리되지 않은 예외(Assert·crashOn*의 __debugbreak 포함)가 나면 작업 폴더에 덤프를 쓰고 끝낸다 - 프로세스 전체에 걸리므로 DLL 워커 스레드도 잡는다
	CDump dump;

	const ULONGLONG runMs = (argc > 1) ? static_cast<ULONGLONG>(atoi(argv[1])) * 1000 : 0; // 실행 시간(초) - 첫 인자로 주면 그 시간 뒤 Q를 누른 것처럼 끝난다(시험 스크립트용). 없으면 Q 키로만
	const ULONGLONG startTick = GetTickCount64();
	const WsadataRAII wsa; // WSAStartup / WSACleanup
	NetConfig config;
	NetError configError;
	if (!LoadNetConfig("server.cfg", &config, &configError))
	{
		const volatile NetError error = configError; // 덤프에서 실패한 키·코드를 본다
		__debugbreak();
		return 1;
	}
	EchoServer server(config.MaxSession);
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
		if (runMs != 0 && GetTickCount64() - startTick >= runMs)
		{
			break;
		}
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
	std::cout << "[main] 종료 전 " << server << std::endl;
	server.Stop();
	printf("[main] 종료 완료\n");
	return 0;
}
