#include <cstdio>
#include "NetConfigLoader.h" // ExeName.h보다 먼저 - WinSock2.h가 Windows.h보다 먼저 와야 한다
#include "ExeName.h"

// 0/1 키를 bool로 읽는다. 키가 없으면 기본값을 그대로 둔다
static void getBool(const utility::Parser& parser, const char* key, bool* outValue)
{
	Assert(outValue != nullptr);

	int value = *outValue ? 1 : 0;
	parser.Get(key, value);
	*outValue = (value != 0);
}

bool LoadNetConfig(const utility::Parser& parser, NetConfig* outConfig)
{
	Assert(outConfig != nullptr);

	NetConfig& config = *outConfig;
	if (!parser.Get("port", config.Port) || !parser.Get("backlog", config.Backlog) || !parser.Get("workerCnt", config.WorkerCnt) ||
		!parser.Get("maxSession", config.MaxSession))
	{
		printf("설정 키 누락 또는 값 오류 (port, backlog, workerCnt, maxSession)\n");
		return false;
	}
	printf("[main] maxSession = %d\n", config.MaxSession);

	// 파일명 토큰이 있으면 cfg 값을 덮어씀 (예: networkLibProject_w4_s56.exe)
	const char* workerCntSrc = "cfg";
	if (ExeName::GetInt('w', config.WorkerCnt))
	{
		workerCntSrc = "파일명";
	}
	printf("[main] workerCnt = %d (%s)\n", config.WorkerCnt, workerCntSrc);

	// cfg/파일명에 없으면 workerCnt와 동일하게 둔다. 우선순위: 파일명(s) > cfg > 기본(=workerCnt)
	config.SlotCnt = config.WorkerCnt;
	const char* slotCntSrc = "기본(=workerCnt)";
	if (parser.Get("slotCnt", config.SlotCnt))
	{
		slotCntSrc = "cfg";
	}
	if (ExeName::GetInt('s', config.SlotCnt))
	{
		slotCntSrc = "파일명";
	}
	printf("[main] slotCnt = %d (%s)\n", config.SlotCnt, slotCntSrc);

	getBool(parser, "gqcsEx", &config.bGqcsEx);
	parser.Get("gqcsBatch", config.GqcsBatch);
	if (config.bGqcsEx)
	{
		printf("[main] 완료 꺼내기 = GQCSEx (gqcsBatch = %d)\n", config.GqcsBatch);
	}
	else
	{
		printf("[main] 완료 꺼내기 = GQCS\n");
	}

	getBool(parser, "sendBufZero", &config.bSendBufZero);
	printf("[main] sendBufZero = %d\n", config.bSendBufZero ? 1 : 0);

	parser.Get("testSendCompleteDelayMs", config.TestSendCompleteDelayMs);
	printf("[main] testSendCompleteDelayMs = %d\n", config.TestSendCompleteDelayMs);

	getBool(parser, "skipOnSuccess", &config.bSkipOnSuccess);
	printf("[main] skipOnSuccess = %d\n", config.bSkipOnSuccess ? 1 : 0);

	getBool(parser, "mergeSends", &config.bMergeSends);
	printf("[main] mergeSends = %d\n", config.bMergeSends ? 1 : 0);

	parser.Get("jobPerSession", config.JobPerSession);
	printf("[main] jobPerSession = %d\n", config.JobPerSession);

	parser.Get("iocpConcurrency", config.IocpConcurrency);
	printf("[main] iocpConcurrency = %d%s\n", config.IocpConcurrency, (config.IocpConcurrency > 0) ? "" : " (코어 수)");

	parser.Get("sendStallSec", config.SendStallSec);
	printf("[main] sendStallSec = %d%s\n", config.SendStallSec, (config.SendStallSec > 0) ? "" : " (끔)");

	return true;
}
