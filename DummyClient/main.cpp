#include <WinSock2.h>
#include "ParserLib.h"
#include <cstdio>
#include <string>
#pragma comment(lib, "ws2_32.lib")

int runBacklogClient(const char *ip, USHORT port, int count);

int main()
{
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
    {
        printf("WSAStartup 실패\n");
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
    int connectCount;
    if (!parser->Get("ip", ip) || !parser->Get("port", port) || !parser->Get("connectCount", connectCount))
    {
        printf("설정 키 누락 또는 값 오류 (ip, port, connectCount)\n");
        WSACleanup();
        return 1;
    }

    int ret = runBacklogClient(ip.c_str(), port, connectCount);
    WSACleanup();
    return ret;
}
