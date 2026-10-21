#include "FloodClient.h"
#include "ExeName.h"
#include "ParserLib.h"
#include "Profiler.h"
#include <cctype>
#include <cstdio>
#include <conio.h>

// 실행 중 키 입력 처리: A 프로파일 저장, D 프로파일 리셋, Q 종료
static void runKeyLoop()
{
    printf("[main] [A] 프로파일 저장  [D] 프로파일 리셋  [Q] 종료\n");

    bool quit = false;
    while (quit == false)
    {
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
            quit = true;
            break;

        default:
            break;
        }
    }
}

int main()
{
    // 출력을 파일로 리다이렉트해 자동 측정할 때, 강제 종료돼도 마지막 줄까지 남도록 버퍼링 끔
    setvbuf(stdout, nullptr, _IONBF, 0);

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
    {
        return 1;
    }

    utility::stDebug debug;
    auto parser = utility::Parser::CreateParser("client.cfg", debug);
    if (parser == nullptr)
    {
        printf("설정 파일 오류 err=%d key=%s\n", debug.err, debug.lastKey.c_str());
        WSACleanup();
        return 1;
    }

    std::string ip;
    USHORT port;
    int threadCnt;
    if (!parser->Get("ip", ip) || !parser->Get("port", port) || !parser->Get("threadCnt", threadCnt))
    {
        printf("설정 키 누락 또는 값 오류 (ip, port, threadCnt)\n");
        WSACleanup();
        return 1;
    }

    // 파일명 토큰이 있으면 cfg 값을 덮어씀 (예: FloodClient_NonBlocking_t8.exe)
    const char* threadCntSrc = "cfg";
    if (ExeName::GetInt('t', threadCnt))
    {
        threadCntSrc = "파일명";
    }
    printf("[main] threadCnt = %d (%s)\n", threadCnt, threadCntSrc);

    // targetRate(초당 목표 시도 수)는 선택 - 없으면 0(제한 없음). 우선순위: 파일명(r) > cfg > 0
    int targetRate = 0;
    const char* targetRateSrc = "기본(제한 없음)";
    if (parser->Get("targetRate", targetRate))
    {
        targetRateSrc = "cfg";
    }
    if (ExeName::GetInt('r', targetRate))
    {
        targetRateSrc = "파일명";
    }
    printf("[main] targetRate = %d (%s)\n", targetRate, targetRateSrc);

    FloodClient client;
    if (!client.init(ip, port, threadCnt, targetRate))
    {
        WSACleanup();
        return 1;
    }
    printf("[main] FloodClient 시작\n");
    runKeyLoop();
    client.stop();

    WSACleanup();
    return 0;
}
