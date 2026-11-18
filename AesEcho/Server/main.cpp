#include <atomic>
#include <conio.h>
#include <cstdio>
#include <iostream>
#include <string>
#include <thread>
#include "AesEchoServer.h"
#include "Profiler.h"       // AesEchoServer.h(WinSock2.h) 뒤에
#include "CrushDump_lib.h"  // AesEchoServer.h(WinSock2.h) 뒤에 - 이 헤더가 Windows.h를 먼저 끌어오면 WinSock2.h와 재정의 오류가 난다

enum : int32_t
{
	PROFILE_SAVE_SEC = 5, // 이 간격마다 측정 파일을 덮어쓴다 - 창을 그냥 닫아도 마지막 값이 남게
};

// 1초마다 서버 상태 한 줄, PROFILE_SAVE_SEC마다 측정 파일 저장
static void runMonitorLoop(const AesEchoServer& server, const std::atomic<bool>& bMonitorRunning)
{
	int32_t sec = 0;
	while (bMonitorRunning)
	{
		Sleep(1000);
		std::cout << server << std::endl;
		++sec;
		if (sec % PROFILE_SAVE_SEC == 0)
		{
			Profile::Save();
		}
	}
}

// 서버 고정 키는 RSA 에코의 것을 그대로 쓴다(--genkey는 RsaEchoServer.exe에만)
int main()
{
	CDump dump; // 처리되지 않은 예외(Assert의 __debugbreak 포함)면 작업 폴더에 덤프
	const WsadataRAII wsa;
	NetConfig config;
	NetError configError;
	if (!LoadNetConfig("server.cfg", &config, &configError))
	{
		fprintf(stderr, "[main] server.cfg를 읽지 못함 - 단계=%s 코드=%d\n", configError.Step != nullptr ? configError.Step : "?", configError.Code);
		const volatile NetError error = configError;
		__debugbreak();
		return 1;
	}
	AesEchoServer server(config.MaxSession);
	printf("[main] AES 에코 - 가입·로그인은 RSA, 에코는 로그인 응답으로 받은 세션 키로 AES-256-GCM\n");
	printf("[main] 서버 고정 키를 %s에서 읽는다(RSA 에코와 같은 키). 서버 임시 키는 접속할 때 워커가 만들고 %d세션이 쓰면 버린다\n", AES_ECHO_SECRET_CFG, static_cast<int>(RSA_ECHO_KEY_MAX_USE));
	if (!server.LoadSignKey(AES_ECHO_SECRET_CFG))
	{
		fprintf(stderr, "[main] RSA 키를 준비하지 못함 - %s에 signSecretKey(RSA 2048 비밀 키 16진수)가 있어야 한다. RsaEcho\\Server에서 RsaEchoServer.exe --genkey로 만든다\n", AES_ECHO_SECRET_CFG);
		__debugbreak();
		return 1;
	}
	Profile::Reset(); // 접속부터 잰다
	if (!server.Start(config))
	{
		const NetError startError = server.GetNetError();
		fprintf(stderr, "[main] 서버 시작 실패 - 단계=%s 코드=%d\n", startError.Step != nullptr ? startError.Step : "?", startError.Code);
		const volatile NetError error = startError;
		__debugbreak();
		return 1;
	}
	printf("[main] 포트 %d (클라는 RsaEcho\\Server\\%s의 서버 고정 공개 키로 확인한다)\n", static_cast<int>(config.Port), RSA_ECHO_PUBLIC_CFG);
	printf("[main] Q = 종료, R = 측정값 초기화\n");

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
			if (key == 'r' || key == 'R')
			{
				Profile::Reset();
				printf("[main] 측정값 초기화\n");
			}
		}
		Sleep(100);
	}

	bMonitorRunning = false;
	monitorThread.join();
	std::cout << "[main] 종료 전 " << server << std::endl;
	server.Stop();
	Profile::Save();
	printf("[main] 측정 결과 = 작업 폴더의 AesEchoServer_<시각>_Profiler.txt\n");
	printf("[main] 종료 완료\n");
	return 0;
}
