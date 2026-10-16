#include "NetworkLib.h"
#include "AcceptServer.h"
#include "AcceptExServer.h"
#include "ParserLib.h"
#include "Profiler.h"
#include <cctype>
#include <conio.h>
#include <cstdio>

// 서버 실행 중 키 입력 처리: A 프로파일 저장, D 프로파일 리셋, Q 종료
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

    std::string mode;
    USHORT port;
    int backlog;
    int workerCnt;
    if (!parser->Get("mode", mode) || !parser->Get("port", port) ||
        !parser->Get("backlog", backlog) || !parser->Get("workerCnt", workerCnt))
    {
        printf("설정 키 누락 또는 값 오류 (mode, port, backlog, workerCnt)\n");
        return 1;
    }

    if (mode == "accept")
    {
        AcceptServer server;
        if (!server.init(port, backlog))
        {
            return 1;
        }
        printf("[main] AcceptServer 리슨 시작\n");
        runKeyLoop();
        server.stop();
    }
    else if (mode == "acceptex")
    {
        AcceptExServer server;
        if (!server.init(port, backlog, workerCnt))
        {
            return 1;
        }
        printf("[main] AcceptExServer 리슨 시작\n");
        runKeyLoop();
        server.stop();
    }
    else
    {
        printf("알 수 없는 mode: %s\n", mode.c_str());
        return 1;
    }

    return 0;
}
