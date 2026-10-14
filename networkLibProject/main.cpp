#include "NetworkLib.h"
#include "AcceptServer.h"
#include "AcceptExServer.h"
#include "ParserLib.h"
#include <cstdio>

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
        printf("[main] AcceptServer 리슨 시작. Enter 입력 시 종료\n");
        system("pause");
        server.stop();
    }
    else if (mode == "acceptex")
    {
        AcceptExServer server;
        if (!server.init(port, backlog, workerCnt))
        {
            return 1;
        }
        printf("[main] AcceptExServer 리슨 시작. Enter 입력 시 종료\n");
        system("pause");
        server.stop();
    }
    else
    {
        printf("알 수 없는 mode: %s\n", mode.c_str());
        return 1;
    }

    return 0;
}
