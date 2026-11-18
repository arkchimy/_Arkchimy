#include <WinSock2.h> // Windows.h보다 먼저
#include <atomic>
#include <conio.h>
#include <cstdio>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <timeapi.h>
#include "ClientConfig.h"
#include "AesEchoClient.h"
#include "ParserLib.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winmm.lib") // timeGetTime, timeBeginPeriod

// AES 에코 시험 클라 - 서버 쪽 RSA 비용을 재려고 부하를 건다(측정은 서버 ProfilerLib). 설정은 작업 폴더의 client.cfg

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
	if (!parser->Get("port", config.Port) || !parser->Get("sessionCnt", config.SessionCnt))
	{
		printf("[main] port 또는 sessionCnt 누락\n");
		return false;
	}
	parser->Get("threadCnt", config.ThreadCnt);
	parser->Get("echoBatch", config.EchoBatch);
	parser->Get("lifeMinSec", config.LifeMinSec);
	parser->Get("lifeMaxSec", config.LifeMaxSec);
	parser->Get("keyFile", config.KeyFile);
	if (config.SessionCnt <= 0 || config.ThreadCnt <= 0 || config.EchoBatch <= 0 || config.EchoBatch > ECHO_BATCH_MAX ||
		config.LifeMinSec > config.LifeMaxSec)
	{
		printf("[main] sessionCnt·threadCnt는 1 이상, echoBatch는 1-%d, lifeMinSec <= lifeMaxSec\n", static_cast<int>(ECHO_BATCH_MAX));
		return false;
	}
	printf("[main] %s:%u 세션=%d 스레드=%d 에코묶음=%d 수명=%d-%d초 공개키=%s\n", config.IP.c_str(), config.Port, config.SessionCnt, config.ThreadCnt,
		config.EchoBatch, config.LifeMinSec, config.LifeMaxSec, config.KeyFile.c_str());
	return true;
}

// rsa_echo_public.cfg의 serverPublicKey = <16진수 512글자>; - "//" 주석 줄은 건너뛰고 줄 맨 앞이 serverPublicKey인 줄만 본다
static bool loadPublicKey(const std::string& path, uint8_t (&outKey)[RSA_MODULUS_LEN])
{
	std::ifstream file(path);
	std::string line;
	bool bFound = false;
	while (!bFound && std::getline(file, line))
	{
		const size_t start = line.find_first_not_of(" \t\xEF\xBB\xBF");
		if (start == std::string::npos || line.compare(start, 2, "//") == 0 || line.compare(start, 15, "serverPublicKey") != 0)
		{
			continue;
		}
		const size_t eqPos = line.find('=', start);
		const size_t endPos = (eqPos == std::string::npos) ? std::string::npos : line.find(';', eqPos);
		if (endPos == std::string::npos)
		{
			break;
		}
		std::string hex = line.substr(eqPos + 1, endPos - eqPos - 1);
		hex.erase(0, hex.find_first_not_of(" \t"));
		hex.erase(hex.find_last_not_of(" \t") + 1);
		bFound = FromHex(hex, outKey, sizeof(outKey));
		break;
	}
	return bFound;
}

// 1초마다 진행 상황 - 초당 값과 누적 값
static void runStatLoop(const AesEchoClient& client, const std::atomic<bool>& bStatRunning)
{
	const ClientStat& stat = client.GetStat();
	int64_t lastHello = 0;
	int64_t lastLogin = 0;
	int64_t lastEcho = 0;
	DWORD nextTime = timeGetTime() + 1000;
	while (bStatRunning)
	{
		const DWORD now = timeGetTime();
		if (static_cast<int32_t>(nextTime - now) > 0)
		{
			Sleep(nextTime - now);
		}
		nextTime += 1000;

		const int64_t hello = stat.HelloCnt;
		const int64_t login = stat.LoginOkCnt;
		const int64_t echo = stat.EchoCnt;
		SYSTEMTIME localTime;
		GetLocalTime(&localTime);
		printf("[%02u:%02u:%02u] [stat]%s 연결=%lld Hello/s=%lld 로그인/s=%lld 에코/s=%lld | 누적 접속=%lld 접속실패=%lld 가입=%lld(이미있음 %lld) 로그인=%lld 에코=%lld FIN=%lld RST=%lld | 프로토콜오류=%lld 예상밖끊김=%lld\n",
			localTime.wHour, localTime.wMinute, localTime.wSecond, client.IsPaused() ? " [일시정지]" : "",
			stat.ConnectedCnt.load(), hello - lastHello, login - lastLogin, echo - lastEcho,
			stat.ConnectCnt.load(), stat.ConnectFailCnt.load(), stat.RegisterOkCnt.load(), stat.RegisterExistsCnt.load(), login, echo,
			stat.FinCloseCnt.load(), stat.RstCloseCnt.load(), stat.ProtocolErrorCnt.load(), stat.UnexpectedCloseCnt.load());
		fflush(stdout);
		lastHello = hello;
		lastLogin = login;
		lastEcho = echo;
	}
}

int main()
{
	ClientConfig config;
	if (!loadConfig("client.cfg", &config))
	{
		return 1;
	}
	uint8_t serverSignKey[RSA_MODULUS_LEN];
	if (!loadPublicKey(config.KeyFile, serverSignKey))
	{
		fprintf(stderr, "[main] 서버 고정 공개 키를 읽지 못함: %s - serverPublicKey 줄이 있어야 한다(RsaEcho\\Server에서 RsaEchoServer.exe --genkey - AES 에코도 같은 키)\n", config.KeyFile.c_str());
		return 1;
	}
	WSADATA wsa;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
	{
		__debugbreak();
		return 1;
	}
	timeBeginPeriod(1); // select 스레드의 Sleep(1)이 약 15.6ms를 자지 않게

	AesEchoClient client;
	if (!client.Start(config, serverSignKey))
	{
		printf("[main] 클라 시작 실패 - IP 형식(%s) 확인\n", config.IP.c_str());
		timeEndPeriod(1);
		WSACleanup();
		return 1;
	}
	std::atomic<bool> bStatRunning = true;
	std::thread statThread(runStatLoop, std::cref(client), std::cref(bStatRunning));
	printf("[main] 시작 - S로 일시정지/다시 시작, Q로 종료\n");

	while (true)
	{
		if (_kbhit())
		{
			const int key = _getch();
			if (key == 'q' || key == 'Q')
			{
				break;
			}
			if (key == 's' || key == 'S')
			{
				client.TogglePause();
				printf("[main] %s\n", client.IsPaused() ? "일시정지" : "다시 시작");
				fflush(stdout);
			}
		}
		Sleep(100);
	}

	client.Stop();
	bStatRunning = false;
	statThread.join();
	timeEndPeriod(1);

	const ClientStat& stat = client.GetStat();
	const bool bPass = stat.ProtocolErrorCnt == 0 && stat.UnexpectedCloseCnt == 0 && stat.ShutdownTimeoutCnt == 0 && stat.EchoCnt > 0;
	printf("[result] %s 접속=%lld Hello=%lld 가입=%lld(이미있음 %lld) 로그인=%lld 에코=%lld 묶음=%lld FIN=%lld RST=%lld 프로토콜오류=%lld 예상밖끊김=%lld 종료대기초과=%lld\n",
		bPass ? "PASS" : "FAIL", stat.ConnectCnt.load(), stat.HelloCnt.load(), stat.RegisterOkCnt.load(), stat.RegisterExistsCnt.load(),
		stat.LoginOkCnt.load(), stat.EchoCnt.load(), stat.BatchCnt.load(), stat.FinCloseCnt.load(), stat.RstCloseCnt.load(),
		stat.ProtocolErrorCnt.load(), stat.UnexpectedCloseCnt.load(), stat.ShutdownTimeoutCnt.load());
	WSACleanup();
	return bPass ? 0 : 1;
}
