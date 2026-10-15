#include "FloodClient.h"
#include "ParserLib.h"
#include <cstdio>
#include <conio.h>

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
    int slotCnt;
    int workerCnt;
    if (!parser->Get("ip", ip) || !parser->Get("port", port) ||
        !parser->Get("slotCnt", slotCnt) || !parser->Get("workerCnt", workerCnt))
    {
        printf("설정 키 누락 또는 값 오류 (ip, port, slotCnt, workerCnt)\n");
        WSACleanup();
        return 1;
    }

    printf("'s' 키를 누르면 시작합니다...\n");
    while (_getch() != 's') {}

    FloodClient client;
    if (!client.init(ip, port, slotCnt, workerCnt))
    {
        WSACleanup();
        return 1;
    }
    printf("[main] FloodClient 시작. Enter 입력 시 종료\n");
    system("pause");
    client.stop();

    WSACleanup();
    return 0;
}
