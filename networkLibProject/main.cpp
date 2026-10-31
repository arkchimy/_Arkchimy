#include "ChatServer.h"
#include "EchoServer.h"
#include "ExeName.h"
#include "ParserLib.h"
#include "Profiler.h"
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

// 아래 함수들이 템플릿인 이유: NetworkLib(GQCS)과 NetworkLibEx(GQCSEx)를 같은 main 코드로 돌려 비교하기 위해

// Ctrl+C가 오거나 라이브러리가 종료를 요청할 때(치명적 오류)까지 기다린다
template <typename Server>
static void waitStop(const Server& server)
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
// CPU는 "사용 코어 / 워커 수"로 출력 - 워커 1개는 최대 1코어라, 사용 코어가 워커 수에 가까우면 서버가 한계
// 프로파일 구간 - 서버 시작 후 resetSec초에 기록을 비우고 saveSec초에 파일로 저장한다 (0이면 안 함)
// GQCS 구간은 완료를 기다린 시간도 포함하므로, 클라가 붙어 있는 부하 구간만 잘라 재야 대기 시간이 섞이지 않는다
struct ProfileWindow
{
    int resetSec = 0;
    int saveSec = 0;
};

template <typename Server>
static void runStatLoop(const Server& server, int workerCnt, ProfileWindow profileWindow, const std::atomic<bool>& statRunning)
{
    StatTicker ticker;
    CpuSampler cpu;
    double coreCnt = static_cast<double>(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
    NetStat last;
    std::vector<uint64_t> lastRtt(LatencyHistogram::BUCKET_COUNT, 0);
    LARGE_INTEGER qpcFreqLi;
    QueryPerformanceFrequency(&qpcFreqLi);
    const double qpcFreq = static_cast<double>(qpcFreqLi.QuadPart);
    int tick = 0;
    NetStat profileStartStat;
    LARGE_INTEGER profileStartQpc{};
    ChatStat lastChat;	// 채팅 컨텐츠일 때만 쓴다
    while (statRunning)
    {
        double intervalSec = ticker.waitNext();	// 실제 구간 길이 (QPC)
        CpuUsage usage = cpu.sample();
        NetStat now = server.getStat();

        ++tick;
        if (profileWindow.resetSec > 0 && tick == profileWindow.resetSec)
        {
            Profile::Reset();
            profileStartStat = now;
            QueryPerformanceCounter(&profileStartQpc);
        }
        else if (profileWindow.saveSec > profileWindow.resetSec && tick == profileWindow.saveSec)
        {
            // 패킷당 비용 = 태그 총시간 ÷ 이 구간의 패킷 수 - 분석 스크립트가 이 줄을 읽는다
            bool saved = Profile::Save();
            LARGE_INTEGER nowQpc;
            LARGE_INTEGER freq;
            QueryPerformanceCounter(&nowQpc);
            QueryPerformanceFrequency(&freq);
            printf("[profile] 저장=%d 구간=%.3fs 패킷=%lld 수신완료=%lld 송신완료=%lld 워커=%d\n",
                saved ? 1 : 0,
                static_cast<double>(nowQpc.QuadPart - profileStartQpc.QuadPart) / static_cast<double>(freq.QuadPart),
                now.recvDataCnt - profileStartStat.recvDataCnt,
                now.recvCompleteCnt - profileStartStat.recvCompleteCnt,
                now.sendCompleteCnt - profileStartStat.sendCompleteCnt,
                workerCnt);
        }

        // 누적 히스토그램의 1초 차분 = 직전 1초 동안 받은 퐁의 RTT 분포
        std::vector<uint64_t> nowRtt = server.getRttCounts();
        std::vector<uint64_t> diff(nowRtt.size());
        for (size_t i = 0; i < nowRtt.size(); ++i)
        {
            diff[i] = nowRtt[i] - lastRtt[i];
        }
        LatencySummary rtt = summarizeLatency(diff);

        // CpuSampler는 PC 전체(모든 코어) 대비 %라 작게 보인다 - 코어 수로 환산하고, 유저/커널은 프로세스 CPU 안의 비율로
        double procPct = usage.processUser + usage.processKernel;
        double cores = procPct * coreCnt / 100.0;
        double kernelRatio = (procPct > 0.0) ? 100.0 * usage.processKernel / procPct : 0.0;

        // 시스템 콜 효율 - 완료 1건마다 GQCS 1번 + WSARecv/WSASend 1번. 한 번에 실린 일이 적을수록 패킷당 커널 비용이 커진다
        int64_t recvComplete = now.recvCompleteCnt - last.recvCompleteCnt;
        int64_t sendComplete = now.sendCompleteCnt - last.sendCompleteCnt;
        double pktPerRecv = (recvComplete > 0) ? static_cast<double>(now.recvDataCnt - last.recvDataCnt) / recvComplete : 0.0;
        double bytesPerSend = (sendComplete > 0) ? static_cast<double>(now.sendBytes - last.sendBytes) / sendComplete : 0.0;
        // GQCS 호출 1번에 꺼낸 완료 수 - NetworkLib은 항상 1, NetworkLibEx는 완료가 쌓여 있을수록 커진다
        // 큐완료: IOCP에서 꺼낸 완료 수 (수신·송신 완료 + 해제 작업)
        int64_t gqcsCall = now.gqcsCallCnt - last.gqcsCallCnt;
        int64_t iocpComplete = now.iocpCompleteCnt - last.iocpCompleteCnt;
        double completePerGqcs = (gqcsCall > 0) ? static_cast<double>(iocpComplete) / gqcsCall : 0.0;

        // WSASend/WSARecv 반환 결과 - 즉시 성공 비율과, 즉시 성공 / 대기 각각의 평균 반환 시간(us)
        auto pct = [](int64_t part, int64_t whole) { return (whole > 0) ? 100.0 * part / whole : 0.0; };
        auto avgUs = [&](int64_t ticks, int64_t cnt) { return (cnt > 0) ? 1e6 * ticks / qpcFreq / cnt : 0.0; };
        int64_t sendSync = now.sendSyncCnt - last.sendSyncCnt;
        int64_t sendPending = now.sendPendingCnt - last.sendPendingCnt;
        int64_t recvSync = now.recvSyncCnt - last.recvSyncCnt;
        int64_t recvPending = now.recvPendingCnt - last.recvPendingCnt;

        printf("[stat] 세션=%lld 수락/s=%.0f 해제/s=%.0f | 수신패킷/s=%.0f 패킷/수신완료=%.1f 바이트/송신완료=%.0f 완료/s=%.0f 큐완료/s=%.0f GQCS/s=%.0f 완료/GQCS=%.1f | 넘침끊김=%lld 슬롯부족=%lld 비정상패킷=%lld | cpu코어=%.2f/%d (커널 %.0f%%)"
            " | 핑/s=%.0f 건너뜀=%lld 퐁/s=%.0f rtt(us) p50=%llu p99=%llu p99.9=%llu max=%llu"
            " | 송신즉시=%.1f%% 송신반환(us) 즉시=%.2f 대기=%.2f | 수신즉시=%.1f%% 수신반환(us) 즉시=%.2f 대기=%.2f"
            " | 송신완료까지(us)=%.2f | 핑표시걸림=%.1f%% 핑송신대기(us)=%.1f"
            " | 송신패킷/s=%.0f 패킷/송신완료=%.2f\n",
            now.acceptCnt - now.releaseCnt,
            (now.acceptCnt - last.acceptCnt) / intervalSec,
            (now.releaseCnt - last.releaseCnt) / intervalSec,
            (now.recvDataCnt - last.recvDataCnt) / intervalSec,
            pktPerRecv,
            bytesPerSend,
            (recvComplete + sendComplete) / intervalSec,
            iocpComplete / intervalSec,
            gqcsCall / intervalSec,
            completePerGqcs,
            now.overflowCnt,
            now.sessionFullCnt,
            now.badPacketCnt,
            cores,
            workerCnt,
            kernelRatio,
            (now.pingSentCnt - last.pingSentCnt) / intervalSec,
            now.pingSkipCnt,
            rtt.count / intervalSec,
            rtt.p50, rtt.p99, rtt.p999, rtt.max,
            pct(sendSync, sendSync + sendPending),
            avgUs(now.sendSyncTicks - last.sendSyncTicks, sendSync),
            avgUs(now.sendPendingTicks - last.sendPendingTicks, sendPending),
            pct(recvSync, recvSync + recvPending),
            avgUs(now.recvSyncTicks - last.recvSyncTicks, recvSync),
            avgUs(now.recvPendingTicks - last.recvPendingTicks, recvPending),
            avgUs(now.sendLatencyTicks - last.sendLatencyTicks, sendComplete),	// 송신을 건 순간부터 그 완료를 처리할 때까지
            pct(now.pingFlagBlockedCnt - last.pingFlagBlockedCnt, now.pingSentCnt - last.pingSentCnt),	// 핑을 넣을 때 송신 중 표시가 켜져 있던 비율
            avgUs(now.pingSendWaitTicks - last.pingSendWaitTicks, now.pingSendWaitCnt - last.pingSendWaitCnt),	// 핑을 넣은 뒤 WSASend에 실리기까지
            (now.sendDataCnt - last.sendDataCnt) / intervalSec,
            (sendComplete > 0) ? static_cast<double>(now.sendDataCnt - last.sendDataCnt) / sendComplete : 0.0);	// WSASend 1번에 실린 컨텐츠 메시지 수 - 모아 보내기가 되는지 보는 지표

        // 채팅 컨텐츠 - 방 락 대기·보유 시간은 브로드캐스트 1번당 평균. 보유 시간에는 락 안에서 건 다른 세션 송신(WSASend)이 들어간다
        if constexpr (requires { server.getChatStat(); })
        {
            ChatStat chat = server.getChatStat();
            int64_t broadcast = chat.broadcastCnt - lastChat.broadcastCnt;
            int64_t deliver = chat.deliverCnt - lastChat.deliverCnt;
            printf("[chat] 입장=%lld 브로드캐스트/s=%.0f 전달/s=%.0f 전달/브로드캐스트=%.1f 방락대기(us)=%.2f 방락보유(us)=%.2f 잘못된패킷=%lld\n",
                chat.joinCnt,
                broadcast / intervalSec,
                deliver / intervalSec,
                (broadcast > 0) ? static_cast<double>(deliver) / broadcast : 0.0,
                avgUs(chat.lockWaitTicks - lastChat.lockWaitTicks, broadcast),
                avgUs(chat.lockHoldTicks - lastChat.lockHoldTicks, broadcast),
                chat.badCnt);
            lastChat = chat;
        }
        last = now;
        lastRtt = std::move(nowRtt);
    }
}

// 실행 전체의 RTT 분포 - 측정 결과 기록용
template <typename Server>
static void printRttTotal(const Server& server)
{
    LatencySummary rtt = summarizeLatency(server.getRttCounts());
    printf("[main] RTT 전체(us): 표본=%llu p50=%llu p99=%llu p99.9=%llu max=%llu\n",
        rtt.count, rtt.p50, rtt.p99, rtt.p999, rtt.max);
}

// 서버 시작 → 통계 스레드 → Ctrl+C 대기 → 정상 종료. 반환값은 main의 종료 코드
template <typename Server>
static int runServer(Server& server, USHORT port, int backlog, int workerCnt, int slotCnt, int maxSession,
    int pingIntervalMs, int pingSampleMod, ProfileWindow profileWindow, bool sendBufZero, int testSendCompleteDelayMs, bool skipOnSuccess)
{
    server.setPing(pingIntervalMs, pingSampleMod);
    server.setSendBufZero(sendBufZero);
    server.setTestSendCompleteDelayMs(testSendCompleteDelayMs);
    server.setSkipOnSuccess(skipOnSuccess);
    if (!server.start(port, backlog, workerCnt, slotCnt, maxSession))
    {
        return 1;	// 소멸자가 stop으로 정리
    }
    printf("[main] 서버 시작 (maxSession = %d, skipOnSuccess 적용 = %d)\n", maxSession, server.getSkipOnSuccess() ? 1 : 0);

    std::atomic<bool> statRunning = true;
    std::thread statThread(runStatLoop<Server>, std::cref(server), workerCnt, profileWindow, std::cref(statRunning));

    waitStop(server);

    statRunning = false;
    statThread.join();	// 진행 중인 1초 대기가 끝날 때까지 최대 1초
    server.stop();
    printRttTotal(server);
    printf("[main] 종료 완료\n");
    return 0;
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

    // RTT 측정용 핑 - cfg에 없으면 끈다. 핑 트래픽(세션 수 ÷ 간격)이 전체의 ~1% 이하가 되도록 간격과 표본 비율을 고른다
    int pingIntervalMs = 0;
    int pingSampleMod = 1;
    parser->Get("pingIntervalMs", pingIntervalMs);
    parser->Get("pingSampleMod", pingSampleMod);
    printf("[main] pingIntervalMs = %d, pingSampleMod = %d\n", pingIntervalMs, pingSampleMod);

    // 프로파일러 - 처리량을 잴 때는 끈다(기록 비용이 결과를 흐린다). 켜면 profileResetSec ~ profileSaveSec 구간만 기록해 저장
    int profile = 0;
    ProfileWindow profileWindow;
    parser->Get("profile", profile);
    parser->Get("profileResetSec", profileWindow.resetSec);
    parser->Get("profileSaveSec", profileWindow.saveSec);
    Profile::SetEnabled(profile != 0);
    if (profile == 0)
    {
        profileWindow = ProfileWindow{};
    }
    printf("[main] profile = %d (구간 %d ~ %d초)\n", profile, profileWindow.resetSec, profileWindow.saveSec);

    // 라이브러리 선택 - gqcsEx = 1이면 NetworkLibEx(GQCSEx로 완료를 gqcsBatch개씩), 없거나 0이면 NetworkLib(GQCS)
    int gqcsEx = 0;
    int gqcsBatch = 64;
    parser->Get("gqcsEx", gqcsEx);
    parser->Get("gqcsBatch", gqcsBatch);

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

    // 컨텐츠 선택 - chatRoomCnt > 0이면 방 브로드캐스트 채팅(ChatServer), 없거나 0이면 에코
    int chatRoomCnt = 0;
    parser->Get("chatRoomCnt", chatRoomCnt);
    printf("[main] 컨텐츠 = %s (chatRoomCnt = %d)\n", (chatRoomCnt > 0) ? "채팅" : "에코", chatRoomCnt);

    auto run = [&](auto& server)
    {
        return runServer(server, port, backlog, workerCnt, slotCnt, maxSession, pingIntervalMs, pingSampleMod, profileWindow,
            sendBufZero != 0, testSendCompleteDelayMs, skipOnSuccess != 0);
    };

    if (gqcsEx != 0)
    {
        printf("[main] 라이브러리 = NetworkLibEx (GQCSEx, gqcsBatch = %d)\n", gqcsBatch);
        if (chatRoomCnt > 0)
        {
            ChatServerEx server;
            server.setGqcsBatch(gqcsBatch);
            server.setChat(chatRoomCnt, maxSession);
            return run(server);
        }
        EchoServerEx server;
        server.setGqcsBatch(gqcsBatch);
        return run(server);
    }
    printf("[main] 라이브러리 = NetworkLib (GQCS)\n");
    if (chatRoomCnt > 0)
    {
        ChatServer server;
        server.setChat(chatRoomCnt, maxSession);
        return run(server);
    }
    EchoServer server;
    return run(server);
}
