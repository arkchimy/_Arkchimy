#include "FloodClient.h"
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

    printf("'s' 키를 누르면 시작합니다...\n");
    while (_getch() != 's') {}

    FloodClient client;
    if (!client.init(ip, port, threadCnt))
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
