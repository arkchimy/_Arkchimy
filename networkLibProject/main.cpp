#include "NetworkLib.h"
#include "ExeName.h"
#include "ParserLib.h"
#include "StatUtil.h"
#include <atomic>
#include <cstdio>
#include <functional>
#include <thread>

static std::atomic<bool> gStopRequested = false;

// Ctrl+C / Ctrl+Break가 오면 종료 요청만 남긴다 - 핸들러는 OS가 만든 별도 스레드에서 불리므로 정리는 main이 한다
// 그 외(창 닫기, 로그오프)는 FALSE를 돌려 기본 동작(즉시 종료)에 맡긴다
static BOOL WINAPI onConsoleCtrl(DWORD ctrlType)
{
    if (ctrlType == CTRL_C_EVENT || ctrlType == CTRL_BREAK_EVENT)
    {
        gStopRequested = true;
        return TRUE;
    }
    return FALSE;
}

// Ctrl+C가 오거나 라이브러리가 종료를 요청할 때(치명적 오류)까지 기다린다
static void waitStop(const NetworkLib& server)
{
    printf("[main] Ctrl+C로 종료\n");

    while (server.isRunning() && !gStopRequested)
    {
        Sleep(100);
    }

    if (gStopRequested)
    {
        printf("[main] 종료 요청 수신 - 종료합니다\n");
    }
    else
    {
        printf("[main] 라이브러리 종료 요청 - 종료합니다\n");
    }
}

// 1초마다 서버 통계 출력 - 라이브러리는 카운터만 세고 출력 형식은 main이 정한다
static void runStatLoop(const NetworkLib& server, const std::atomic<bool>& statRunning)
{
    StatTicker ticker;
    CpuSampler cpu;
    NetStat last;
    while (statRunning)
    {
        double intervalSec = ticker.waitNext();	// 실제 구간 길이 (QPC)
        CpuUsage usage = cpu.sample();
        NetStat now = server.getStat();
        printf("[stat] 세션=%lld 수락/s=%.0f 해제/s=%.0f | 넘침끊김=%lld 슬롯부족=%lld 비정상패킷=%lld | cpu 유저=%.1f%% 커널=%.1f%%\n",
            now.acceptCnt - now.releaseCnt,
            (now.acceptCnt - last.acceptCnt) / intervalSec,
            (now.releaseCnt - last.releaseCnt) / intervalSec,
            now.overflowCnt,
            now.sessionFullCnt,
            now.badPacketCnt,
            usage.processUser,
            usage.processKernel);
        last = now;
    }
}

int main()
{
    // 출력을 파일로 리다이렉트해 자동 측정할 때, 강제 종료돼도 마지막 줄까지 남도록 버퍼링 끔
    setvbuf(stdout, nullptr, _IONBF, 0);

    WsadataRAII wsa;
    if (!wsa.mOk)
    {
        return 1;
    }

    utility::stDebug debug;
    auto parser = utility::Parser::CreateParser("server.cfg", debug);
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

    NetworkLib server;
    if (!server.start(port, backlog, workerCnt, slotCnt, maxSession))
    {
        return 1;	// 소멸자가 stop으로 정리
    }
    printf("[main] 서버 시작 (maxSession = %d)\n", maxSession);

    std::atomic<bool> statRunning = true;
    std::thread statThread(runStatLoop, std::cref(server), std::cref(statRunning));

    waitStop(server);

    statRunning = false;
    statThread.join();	// 진행 중인 1초 대기가 끝날 때까지 최대 1초
    server.stop();
    printf("[main] 종료 완료\n");

    return 0;
}
