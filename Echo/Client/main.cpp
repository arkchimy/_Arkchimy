#include <WinSock2.h> // Windows.h보다 먼저 - 아래 헤더들이 Windows.h를 끌어오기 전에
#include <atomic>
#include <conio.h>
#include <cstdio>
#include <functional>
#include <memory>
#include <thread>
#include <timeapi.h>
#include "ClientConfig.h"
#include "EchoClient.h"
#include "ParserLib.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winmm.lib") // timeGetTime

// client.cfg를 읽는다. 필수 키: port와 역할별 세션 수 중 하나 이상
static bool loadConfig(const char* path, ClientConfig* outConfig)
{
	utility::stDebug debug;
	const std::unique_ptr<utility::Parser> parser = utility::Parser::CreateParser(path, debug);
	if (parser == nullptr)
	{
		printf("[main] 설정 파일 오류 err=%d key=%s\n", debug.err, debug.lastKey.c_str());
		return false;
	}

	ClientConfig& config = *outConfig;
	parser->Get("ip", config.IP);
	if (!parser->Get("port", config.Port))
	{
		printf("[main] port 누락\n");
		return false;
	}
	parser->Get("normalCnt", config.NormalCnt);
	parser->Get("badHeaderCnt", config.BadHeaderCnt);
	parser->Get("lifeMinSec", config.LifeMinSec);
	parser->Get("lifeMaxSec", config.LifeMaxSec);

	if (config.NormalCnt + config.BadHeaderCnt <= 0 || config.LifeMinSec > config.LifeMaxSec)
	{
		printf("[main] 세션 수가 0이거나 lifeMinSec > lifeMaxSec\n");
		return false;
	}
	printf("[main] %s:%u 정상=%d 비정상헤더=%d 수명=%d-%d초\n", config.IP.c_str(), config.Port, config.NormalCnt, config.BadHeaderCnt,
		config.LifeMinSec, config.LifeMaxSec);
	return true;
}

// 1초마다 진행 상황 출력 - 누적 값이라 마지막 줄로 판정한다
// 다음 출력 시각을 1초씩 더해 가며 그때까지 잔다 - Sleep(1000)을 반복하면 출력에 걸린 시간만큼 조금씩 밀린다
static void runStatLoop(const ClientStat& stat, const std::atomic<bool>& bStatRunning)
{
	int64_t lastBatch = 0;
	DWORD nextTime = timeGetTime() + 1000;

	while (bStatRunning)
	{
		const DWORD now = timeGetTime();
		if (static_cast<int32_t>(nextTime - now) > 0) // 넘어감(49.7일)에도 맞도록 부호 있는 차이로 비교
		{
			Sleep(nextTime - now);
		}
		nextTime += 1000;

		const int64_t batch = stat.BatchCnt;
		printf("[stat] 연결=%lld 묶음/s=%lld | 접속=%lld 접속실패=%lld | FIN=%lld RST=%lld | 비정상헤더 보냄=%lld 끊김확인=%lld | 예상밖끊김=%lld\n",
			stat.ConnectedCnt.load(),
			batch - lastBatch,
			stat.ConnectCnt.load(),
			stat.ConnectFailCnt.load(),
			stat.FinCloseCnt.load(),
			stat.RstCloseCnt.load(),
			stat.BadHeaderSentCnt.load(),
			stat.BadHeaderClosedCnt.load(),
			stat.UnexpectedCloseCnt.load());
		lastBatch = batch;
	}
}

int main()
{
	WSADATA wsa;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
	{
		__debugbreak();
		return 1;
	}

	ClientConfig config;
	if (!loadConfig("client.cfg", &config))
	{
		WSACleanup();
		return 1;
	}

	EchoClient client;
	if (!client.Start(config))
	{
		printf("[main] 클라 시작 실패 - IP 형식(%s) 확인\n", config.IP.c_str()); // 시작하지 못한 채 Q를 누르면 통계가 모두 0이라 PASS로 보이므로 여기서 끝낸다
		WSACleanup();
		return 1;
	}
	const ClientStat& stat = client.GetStat();

	std::atomic<bool> bStatRunning = true;
	std::thread statThread(runStatLoop, std::cref(stat), std::cref(bStatRunning));
	printf("[main] 시작 - Q로 종료\n");

	while (true)
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

	client.Stop(); // 정상 세션은 진행 중인 묶음의 응답까지 받은 뒤 닫고 select 스레드가 끝난다
	bStatRunning = false;
	statThread.join();

	// 실패: 정상 세션이 서버에게 먼저 끊김, 종료할 때 응답이 끝까지 안 옴, 정상 세션이 있는데 묶음을 하나도 못 끝냄(아무 일도 안 한 것을 통과로 보지 않게)
	// 불일치는 select 스레드가 그 자리에서 멈춘다
	const bool bDidWork = (config.NormalCnt == 0 || stat.BatchCnt > 0);
	const bool bPass = (stat.UnexpectedCloseCnt == 0 && stat.ShutdownTimeoutCnt == 0 && bDidWork);
	printf("[result] %s 접속=%lld 묶음=%lld FIN=%lld RST=%lld 비정상헤더 보냄=%lld 끊김확인=%lld 보내기실패=%lld 예상밖끊김=%lld 종료대기초과=%lld\n",
		bPass ? "PASS" : "FAIL",
		stat.ConnectCnt.load(),
		stat.BatchCnt.load(),
		stat.FinCloseCnt.load(),
		stat.RstCloseCnt.load(),
		stat.BadHeaderSentCnt.load(),
		stat.BadHeaderClosedCnt.load(),
		stat.BadHeaderSendFailCnt.load(),
		stat.UnexpectedCloseCnt.load(),
		stat.ShutdownTimeoutCnt.load());

	WSACleanup();
	return bPass ? 0 : 1;
}
