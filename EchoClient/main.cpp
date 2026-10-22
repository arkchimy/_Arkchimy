#include "EchoClient.h"
#include "ExeName.h"
#include "ParserLib.h"
#include <cctype>
#include <cstdio>
#include <iostream>
#include <conio.h>

// 실행 중 키 입력 처리: Q 종료
static void runKeyLoop()
{
    std::cout << "[main] [Q] 종료\n";

    bool quit = false;
    while (quit == false)
    {
        int key = toupper(_getch());

        switch (key)
        {
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
    int64_t bytesPerConn;
    if (!parser->Get("ip", ip) || !parser->Get("port", port) || !parser->Get("threadCnt", threadCnt) ||
        !parser->Get("bytesPerConn", bytesPerConn))
    {
        std::cout << "설정 키 누락 또는 값 오류 (ip, port, threadCnt, bytesPerConn)\n";
        WSACleanup();
        return 1;
    }

    // 파일명 토큰이 있으면 cfg 값을 덮어씀 (예: EchoClient_t2.exe)
    const char* threadCntSrc = "cfg";
    if (ExeName::GetInt('t', threadCnt))
    {
        threadCntSrc = "파일명";
    }
    std::cout << "[main] threadCnt = " << threadCnt << " (" << threadCntSrc << "), 스레드당 세션 64\n";
    std::cout << "[main] bytesPerConn = " << bytesPerConn << "\n";

    EchoClient client;
    if (!client.init(ip, port, threadCnt, bytesPerConn))
    {
        WSACleanup();
        return 1;
    }
    std::cout << "[main] EchoClient 시작\n";
    runKeyLoop();
    client.stop();

    WSACleanup();
    return 0;
}
