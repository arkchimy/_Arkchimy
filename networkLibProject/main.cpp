#include <atomic>
#include <cstdio>
#include <functional>
#include <memory>
#include <thread>
#include "ChatServer.h"
#include "EchoServer.h"
#include "ExeName.h"
#include "ParserLib.h"
#include "Profiler.h"
#include "StatUtil.h"

static std::atomic<bool> sbStopRequested = false;

// Ctrl+C / Ctrl+Break가 오면 종료 요청만 남긴다 - 핸들러는 OS가 만든 별도 스레드에서 불리므로 정리는 main이 한다
// 그 외(창 닫기, 로그오프)는 FALSE를 돌려 기본 동작(즉시 종료)에 맡긴다
static BOOL WINAPI onConsoleCtrl(const DWORD ctrlType)
{
	if (ctrlType == CTRL_C_EVENT || ctrlType == CTRL_BREAK_EVENT)
	{
		sbStopRequested = true;
		return TRUE;
	}
	return FALSE;
}

// 아래 함수들이 템플릿인 이유: 컨텐츠(EchoServer, ChatServer)를 같은 main 코드로 돌리고, 채팅일 때만 채팅 통계를 더 찍기 위해

// Ctrl+C가 오거나 라이브러리가 종료를 요청할 때(치명적 오류)까지 기다린다
template <typename Server>
static void waitForStop(const Server& server)
{
	printf("[main] Ctrl+C로 종료\n");

	while (server.IsRunning() && !sbStopRequested)
	{
		Sleep(100);
	}

	if (sbStopRequested)
	{
		printf("[main] 종료 요청 수신 - 종료합니다\n");
	}
	else
	{
		printf("[main] 라이브러리 종료 요청 - 종료합니다\n");
	}
}

// 1초마다 서버 통계 출력 - 라이브러리는 카운터만 세고 출력 형식은 main이 정한다
// CPU는 "사용 코어 / 워커 수"로 출력 - 워커 1개는 최대 1코어라, 사용 코어가 워커 수에 가까우면 서버가 한계
// 프로파일 구간 - 서버 시작 후 ResetSec초에 기록을 비우고 SaveSec초에 파일로 저장한다 (0이면 안 함)
// GQCS 구간은 완료를 기다린 시간도 포함하므로, 클라가 붙어 있는 부하 구간만 잘라 재야 대기 시간이 섞이지 않는다
struct ProfileWindow
{
	int ResetSec = 0;
	int SaveSec = 0;
};

template <typename Server>
static void runStatLoop(const Server& server, const int workerCnt, const ProfileWindow profileWindow, const std::atomic<bool>& bStatRunning)
{
	StatTicker ticker;
	CpuSampler cpu;
	const double coreCnt = static_cast<double>(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
	NetStat last;
	LARGE_INTEGER qpcFreqLi;
	QueryPerformanceFrequency(&qpcFreqLi);
	const double qpcFreq = static_cast<double>(qpcFreqLi.QuadPart);
	int tick = 0;
	NetStat profileStartStat;
	LARGE_INTEGER profileStartQPC{};
	ChatStat lastChat;	// 채팅 컨텐츠일 때만 쓴다
	while (bStatRunning)
	{
		const double intervalSec = ticker.waitNext();	// 실제 구간 길이 (QPC)
		const CpuUsage usage = cpu.sample();
		const NetStat now = server.GetStat();

		++tick;
		if (profileWindow.ResetSec > 0 && tick == profileWindow.ResetSec)
		{
			Profile::Reset();
			profileStartStat = now;
			QueryPerformanceCounter(&profileStartQPC);
		}
		else if (profileWindow.SaveSec > profileWindow.ResetSec && tick == profileWindow.SaveSec)
		{
			// 패킷당 비용 = 태그 총시간 ÷ 이 구간의 패킷 수 - 분석 스크립트가 이 줄을 읽는다
			const bool bSaved = Profile::Save();
			LARGE_INTEGER nowQPC;
			LARGE_INTEGER freq;
			QueryPerformanceCounter(&nowQPC);
			QueryPerformanceFrequency(&freq);
			printf("[profile] 저장=%d 구간=%.3fs 패킷=%lld 수신완료=%lld 송신완료=%lld 워커=%d\n",
				bSaved ? 1 : 0,
				static_cast<double>(nowQPC.QuadPart - profileStartQPC.QuadPart) / static_cast<double>(freq.QuadPart),
				now.RecvPacketCnt - profileStartStat.RecvPacketCnt,
				now.RecvCompleteCnt - profileStartStat.RecvCompleteCnt,
				now.SendCompleteCnt - profileStartStat.SendCompleteCnt,
				workerCnt);
		}

		// CpuSampler는 PC 전체(모든 코어) 대비 %라 작게 보인다 - 코어 수로 환산하고, 유저/커널은 프로세스 CPU 안의 비율로
		const double procPct = usage.processUser + usage.processKernel;
		const double cores = procPct * coreCnt / 100.0;
		const double kernelRatio = (procPct > 0.0) ? 100.0 * usage.processKernel / procPct : 0.0;

		// 시스템 콜 효율 - 완료 1건마다 GQCS 1번 + WSARecv/WSASend 1번. 한 번에 실린 일이 적을수록 패킷당 커널 비용이 커진다
		const int64_t recvComplete = now.RecvCompleteCnt - last.RecvCompleteCnt;
		const int64_t sendComplete = now.SendCompleteCnt - last.SendCompleteCnt;
		const double pktPerRecv = (recvComplete > 0) ? static_cast<double>(now.RecvPacketCnt - last.RecvPacketCnt) / recvComplete : 0.0;
		const double bytesPerSend = (sendComplete > 0) ? static_cast<double>(now.SendBytes - last.SendBytes) / sendComplete : 0.0;
		// GQCS 호출 1번에 꺼낸 완료 수 - GQCS는 항상 1, GQCSEx는 완료가 쌓여 있을수록 커진다
		// 큐완료: IOCP에서 꺼낸 완료 수 (수신·송신 완료 + 해제 작업)
		const int64_t gqcsCall = now.GqcsCallCnt - last.GqcsCallCnt;
		const int64_t iocpComplete = now.IocpCompleteCnt - last.IocpCompleteCnt;
		const double completePerGqcs = (gqcsCall > 0) ? static_cast<double>(iocpComplete) / gqcsCall : 0.0;

		// WSASend/WSARecv 반환 결과 - 즉시 성공 비율과, 즉시 성공 / 대기 각각의 평균 반환 시간(us)
		auto pct = [](const int64_t part, const int64_t whole)
		{
			return (whole > 0) ? 100.0 * part / whole : 0.0;
		};
		auto avgUs = [&](const int64_t ticks, const int64_t cnt)
		{
			return (cnt > 0) ? 1e6 * ticks / qpcFreq / cnt : 0.0;
		};
		const int64_t sendSync = now.SendSyncCnt - last.SendSyncCnt;
		const int64_t sendPending = now.SendPendingCnt - last.SendPendingCnt;
		const int64_t recvSync = now.RecvSyncCnt - last.RecvSyncCnt;
		const int64_t recvPending = now.RecvPendingCnt - last.RecvPendingCnt;

		printf("[stat] 세션=%lld 수락/s=%.0f 해제/s=%.0f | 수신패킷/s=%.0f 패킷/수신완료=%.1f 바이트/송신완료=%.0f 완료/s=%.0f 큐완료/s=%.0f GQCS/s=%.0f 완료/GQCS=%.1f | 넘침끊김=%lld 송신정체끊김=%lld 슬롯부족=%lld 비정상패킷=%lld | cpu코어=%.2f/%d (커널 %.0f%%)"
			" | 송신즉시=%.1f%% 송신반환(us) 즉시=%.2f 대기=%.2f | 수신즉시=%.1f%% 수신반환(us) 즉시=%.2f 대기=%.2f"
			" | 송신완료까지(us)=%.2f"
			" | 송신패킷/s=%.0f 패킷/송신완료=%.2f\n",
			now.AcceptCnt - now.ReleaseCnt,
			(now.AcceptCnt - last.AcceptCnt) / intervalSec,
			(now.ReleaseCnt - last.ReleaseCnt) / intervalSec,
			(now.RecvPacketCnt - last.RecvPacketCnt) / intervalSec,
			pktPerRecv,
			bytesPerSend,
			(recvComplete + sendComplete) / intervalSec,
			iocpComplete / intervalSec,
			gqcsCall / intervalSec,
			completePerGqcs,
			now.OverflowCnt,
			now.SendStallCnt,
			now.SessionFullCnt,
			now.BadPacketCnt,
			cores,
			workerCnt,
			kernelRatio,
			pct(sendSync, sendSync + sendPending),
			avgUs(now.SendSyncTicks - last.SendSyncTicks, sendSync),
			avgUs(now.SendPendingTicks - last.SendPendingTicks, sendPending),
			pct(recvSync, recvSync + recvPending),
			avgUs(now.RecvSyncTicks - last.RecvSyncTicks, recvSync),
			avgUs(now.RecvPendingTicks - last.RecvPendingTicks, recvPending),
			avgUs(now.SendWaitTicks - last.SendWaitTicks, sendComplete),	// 송신을 건 순간부터 그 완료를 처리할 때까지
			(now.SendPacketCnt - last.SendPacketCnt) / intervalSec,
			(sendComplete > 0) ? static_cast<double>(now.SendPacketCnt - last.SendPacketCnt) / sendComplete : 0.0);	// WSASend 1번에 실린 컨텐츠 메시지 수 - 모아 보내기가 되는지 보는 지표

		// 채팅 컨텐츠 - 방 락 대기·보유 시간은 브로드캐스트 1번당 평균. 보유 시간은 구성원 전원의 송신 링에 넣는 시간(WSASend는 락 밖)
		if constexpr (requires { server.GetChatStat(); })
		{
			const ChatStat chat = server.GetChatStat();
			const int64_t broadcast = chat.BroadcastCnt - lastChat.BroadcastCnt;
			const int64_t deliver = chat.DeliverCnt - lastChat.DeliverCnt;
			printf("[chat] 입장=%lld 브로드캐스트/s=%.0f 전달/s=%.0f 전달/브로드캐스트=%.1f 방락대기(us)=%.2f 방락보유(us)=%.2f 잘못된패킷=%lld\n",
				chat.JoinCnt,
				broadcast / intervalSec,
				deliver / intervalSec,
				(broadcast > 0) ? static_cast<double>(deliver) / broadcast : 0.0,
				avgUs(chat.LockWaitTicks - lastChat.LockWaitTicks, broadcast),
				avgUs(chat.LockHoldTicks - lastChat.LockHoldTicks, broadcast),
				chat.BadCnt);
			lastChat = chat;
		}
		last = now;
	}
}

// 서버 시작 → 통계 스레드 → Ctrl+C 대기 → 정상 종료. 반환값은 main의 종료 코드
template <typename Server>
static int runServer(Server& server, const USHORT port, const int backlog, const int workerCnt, const int slotCnt, const int maxSession,
	const ProfileWindow profileWindow, const bool bGqcsEx, const int gqcsBatch, const bool bSendBufZero, const int testSendCompleteDelayMs,
	const bool bSkipOnSuccess, const bool bFlushPerBatch, const int sendStallSec)
{
	server.SetGqcsEx(bGqcsEx, gqcsBatch);
	server.SetSendStallSec(sendStallSec);
	server.SetFlushPerBatch(bFlushPerBatch);
	server.SetSendBufZero(bSendBufZero);
	server.SetTestSendCompleteDelayMs(testSendCompleteDelayMs);
	server.SetSkipOnSuccess(bSkipOnSuccess);
	if (!server.Start(port, backlog, workerCnt, slotCnt, maxSession))
	{
		return 1;	// 소멸자가 Stop으로 정리
	}
	printf("[main] 서버 시작 (maxSession = %d, skipOnSuccess 적용 = %d)\n", maxSession, server.IsSkipOnSuccess() ? 1 : 0);

	std::atomic<bool> bStatRunning = true;
	std::thread statThread(runStatLoop<Server>, std::cref(server), workerCnt, profileWindow, std::cref(bStatRunning));

	waitForStop(server);

	bStatRunning = false;
	statThread.join();	// 진행 중인 1초 대기가 끝날 때까지 최대 1초
	server.Stop();
	printf("[main] 종료 완료\n");
	return 0;
}

int main()
{
	// 출력을 파일로 리다이렉트해 자동 측정할 때, 강제 종료돼도 마지막 줄까지 남도록 버퍼링 끔
	setvbuf(stdout, nullptr, _IONBF, 0);

	const WsadataRAII wsa;
	if (!wsa.IsOk())
	{
		return 1;
	}

	utility::stDebug debug;
	const std::unique_ptr<utility::Parser> parser = utility::Parser::CreateParser("server.cfg", debug);
	if (parser == nullptr)
	{
		printf("설정 파일 오류 err=%d key=%s\n", debug.err, debug.lastKey.c_str());
		return 1;
	}

	USHORT port;
	int backlog;
	int workerCnt;
	int maxSession;
	if (!parser->Get("port", port) || !parser->Get("backlog", backlog) || !parser->Get("workerCnt", workerCnt) ||
		!parser->Get("maxSession", maxSession))
	{
		printf("설정 키 누락 또는 값 오류 (port, backlog, workerCnt, maxSession)\n");
		return 1;
	}

	// 파일명 토큰이 있으면 cfg 값을 덮어씀 (예: networkLibProject_w4_s56.exe)
	const char* workerCntSrc = "cfg";
	if (ExeName::GetInt('w', workerCnt))
	{
		workerCntSrc = "파일명";
	}
	printf("[main] workerCnt = %d (%s)\n", workerCnt, workerCntSrc);

	// slotCnt(동시에 걸어두는 AcceptEx 수)
	// cfg/파일명에 없으면 workerCnt와 동일하게 둔다. 우선순위: 파일명(s) > cfg > 기본(=workerCnt)
	int slotCnt = workerCnt;
	const char* slotCntSrc = "기본(=workerCnt)";
	{
		int cfgSlot = 0;
		if (parser->Get("slotCnt", cfgSlot))
		{
			slotCnt = cfgSlot;
			slotCntSrc = "cfg";
		}
	}
	if (ExeName::GetInt('s', slotCnt))
	{
		slotCntSrc = "파일명";
	}
	printf("[main] slotCnt = %d (%s)\n", slotCnt, slotCntSrc);

	if (!SetConsoleCtrlHandler(onConsoleCtrl, TRUE))
	{
		printf("SetConsoleCtrlHandler 실패 err=%lu\n", GetLastError());
		return 1;
	}

	// 프로파일러 - 처리량을 잴 때는 끈다(기록 비용이 결과를 흐린다). 켜면 profileResetSec – profileSaveSec 구간만 기록해 저장
	int profile = 0;
	ProfileWindow profileWindow;
	parser->Get("profile", profile);
	parser->Get("profileResetSec", profileWindow.ResetSec);
	parser->Get("profileSaveSec", profileWindow.SaveSec);
	Profile::SetEnabled(profile != 0);
	if (profile == 0)
	{
		profileWindow = ProfileWindow{};
	}
	printf("[main] profile = %d (구간 %d ~ %d초)\n", profile, profileWindow.ResetSec, profileWindow.SaveSec);

	// 워커가 완료를 꺼내는 방식 - 1(기본)이면 GQCSEx로 gqcsBatch개씩, 0이면 GQCS로 1개씩(비교용)
	int gqcsEx = 1;
	int gqcsBatch = 64;
	parser->Get("gqcsEx", gqcsEx);
	parser->Get("gqcsBatch", gqcsBatch);
	if (gqcsEx != 0)
	{
		printf("[main] 완료 꺼내기 = GQCSEx (gqcsBatch = %d)\n", gqcsBatch);
	}
	else
	{
		printf("[main] 완료 꺼내기 = GQCS\n");
	}

	// 송신 버퍼 0 실험 - 1이면 수락한 소켓마다 SO_SNDBUF = 0 (송신이 즉시 끝나지 않고 비동기로 넘어간다)
	int sendBufZero = 0;
	parser->Get("sendBufZero", sendBufZero);
	printf("[main] sendBufZero = %d\n", sendBufZero);

	// [테스트 전용] 송신 완료 처리 지연 주입 - 넘침 판정이 완료 지연을 상대 탓으로 세는지 결정적으로 시험한다. 측정에서는 0
	int testSendCompleteDelayMs = 0;
	parser->Get("testSendCompleteDelayMs", testSendCompleteDelayMs);
	printf("[main] testSendCompleteDelayMs = %d\n", testSendCompleteDelayMs);

	// 완료 통지 skip 방식 - 1이면 즉시 끝난 WSASend/WSARecv의 완료를 IOCP 큐에 넣지 않고 건 스레드가 바로 처리
	int skipOnSuccess = 0;
	parser->Get("skipOnSuccess", skipOnSuccess);
	printf("[main] skipOnSuccess = %d\n", skipOnSuccess);

	// 수신 처리 중 송신을 보내는 경계 - 0이면 수신 완료 1건 끝, 1이면 워커가 꺼낸 완료 묶음 끝
	int flushPerBatch = 0;
	parser->Get("flushPerBatch", flushPerBatch);
	printf("[main] flushPerBatch = %d\n", flushPerBatch);

	// 송신 넘침 한도는 설정이 아니라 상수(SEND_RING_SIZE) - 송신 링이 가득 차면 끊는다
	printf("[main] 송신 링 = %dKB (가득 차면 끊음)\n", SEND_RING_SIZE / 1024);

	// 송신 정체 판정 기준(초) - 걸린 WSASend가 이 시간 넘게 안 끝나면 끊는다. cfg에 없거나 0이면 끔
	int sendStallSec = 0;
	parser->Get("sendStallSec", sendStallSec);
	printf("[main] sendStallSec = %d%s\n", sendStallSec, (sendStallSec > 0) ? "" : " (끔)");

	// 컨텐츠 선택 - chatRoomCnt > 0이면 방 브로드캐스트 채팅(ChatServer), 없거나 0이면 에코
	int chatRoomCnt = 0;
	parser->Get("chatRoomCnt", chatRoomCnt);
	printf("[main] 컨텐츠 = %s (chatRoomCnt = %d)\n", (chatRoomCnt > 0) ? "채팅" : "에코", chatRoomCnt);

	auto run = [&](auto& server)
	{
		return runServer(server, port, backlog, workerCnt, slotCnt, maxSession, profileWindow,
			gqcsEx != 0, gqcsBatch, sendBufZero != 0, testSendCompleteDelayMs, skipOnSuccess != 0, flushPerBatch != 0, sendStallSec);
	};

	if (chatRoomCnt > 0)
	{
		ChatServer chatServer;
		chatServer.SetChat(chatRoomCnt, maxSession);
		return run(chatServer);
	}
	EchoServer echoServer;
	return run(echoServer);
}
