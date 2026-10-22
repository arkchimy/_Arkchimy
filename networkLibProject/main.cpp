#include "NetworkLib.h"
#include "ExeName.h"
#include "ParserLib.h"
#include "Profiler.h"
#include <cctype>
#include <conio.h>
#include <cstdio>

// 서버 실행 중 키 입력 처리: A 프로파일 저장, D 프로파일 리셋, Q 종료
// 라이브러리가 종료를 요청하면(치명적 오류) 키 입력 없이도 빠져나온다
static void runKeyLoop(const NetworkLib& server)
{
    printf("[main] [A] 프로파일 저장  [D] 프로파일 리셋  [Q] 종료\n");

    while (server.isRunning())
    {
        if (_kbhit() == 0)
        {
            Sleep(100);
            continue;
        }

        int key = toupper(_getch());

        switch (key)
        {
        case 'A':
            if (Profile::Save())
            {
                printf("[main] 프로파일 저장\n");
            }
            else
            {
                printf("[main] 프로파일 저장 실패\n");
            }
            break;

        case 'D':
            Profile::Reset();
            printf("[main] 프로파일 리셋\n");
            break;

        case 'Q':
            return;

        default:
            break;
        }
    }
    printf("[main] 라이브러리 종료 요청 - 종료합니다\n");
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

    NetworkLib server;
    if (!server.start(port, backlog, workerCnt, slotCnt, maxSession))
    {
        return 1;	// 소멸자가 stop으로 정리
    }
    printf("[main] 서버 시작 (maxSession = %d)\n", maxSession);
    runKeyLoop(server);
    server.stop();

    return 0;
}
