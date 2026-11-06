#include "FloodClient.h"
#include "ExeName.h"
#include "ParserLib.h"
#include "Profiler.h"
#include <cctype>
#include <cstdio>
#include <iostream>
#include <conio.h>

// 실행 중 키 입력 처리: A 프로파일 저장, D 프로파일 리셋, Q 종료
static void runKeyLoop()
{
    std::cout << "[main] [A] 프로파일 저장  [D] 프로파일 리셋  [Q] 종료\n";

    bool quit = false;
    while (quit == false)
    {
        int key = toupper(_getch());

        switch (key)
        {
        case 'A':
            if (Profile::Save())
            {
                std::cout << "[main] 프로파일 저장\n";
            }
            else
            {
                std::cout << "[main] 프로파일 저장 실패\n";
            }
            break;

        case 'D':
            Profile::Reset();
            std::cout << "[main] 프로파일 리셋\n";
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
        std::cout << "설정 파일 오류 err=" << debug.err << " key=" << debug.lastKey << "\n";
        WSACleanup();
        return 1;
    }

    std::string ip;
    USHORT port;
    int threadCnt;
    if (!parser->Get("ip", ip) || !parser->Get("port", port) || !parser->Get("threadCnt", threadCnt))
    {
        std::cout << "설정 키 누락 또는 값 오류 (ip, port, threadCnt)\n";
        WSACleanup();
        return 1;
    }

    // 파일명 토큰이 있으면 cfg 값을 덮어씀 (예: FloodClient_Select_t2_o10.exe)
    const char* threadCntSrc = "cfg";
    if (ExeName::GetInt('t', threadCnt))
    {
        threadCntSrc = "파일명";
    }
    std::cout << "[main] threadCnt = " << threadCnt << " (" << threadCntSrc << "), 스레드당 세션 64\n";

    // select 타임아웃(ms). 우선순위: 파일명(o) > cfg > 100. 0이면 즉시 반환(폴링)
    int selectTimeoutMs = 100;
    const char* selectTimeoutSrc = "기본";
    if (parser->Get("selectTimeoutMs", selectTimeoutMs))
    {
        selectTimeoutSrc = "cfg";
    }
    if (ExeName::GetInt('o', selectTimeoutMs))
    {
        selectTimeoutSrc = "파일명";
    }
    std::cout << "[main] selectTimeoutMs = " << selectTimeoutMs << " (" << selectTimeoutSrc << ")\n";

    FloodClient client;
    if (!client.init(ip, port, threadCnt, selectTimeoutMs))
    {
        WSACleanup();
        return 1;
    }
    std::cout << "[main] FloodClient 시작\n";
    runKeyLoop();
    client.stop();

    WSACleanup();
    return 0;
}
