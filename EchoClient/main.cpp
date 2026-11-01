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

    // 1이면 받지 않고 보내기만 한다 (서버 송신 링 넘침 테스트). cfg에 없으면 0
    int noRecv = 0;
    parser->Get("noRecv", noRecv);
    std::cout << "[main] noRecv = " << noRecv << "\n";

    // 1이면 연결마다 최대 길이를 넘는 헤더를 보내고 서버가 끊는지 본다 (비정상 패킷 테스트). cfg에 없으면 0
    int badPacket = 0;
    parser->Get("badPacket", badPacket);
    std::cout << "[main] badPacket = " << badPacket << "\n";

    // 돌아오지 않은 에코 바이트 한도 - 줄이면 세션 큐가 얕아진다. cfg에 없으면 최대값
    EchoClientConfig cfg;
    parser->Get("maxInflight", cfg.maxInflight);
    std::cout << "[main] maxInflight = " << cfg.maxInflight << "\n";

    cfg.ip = ip;
    cfg.port = port;
    cfg.threadCnt = threadCnt;
    cfg.bytesPerConn = bytesPerConn;
    cfg.noRecv = noRecv != 0;
    cfg.badPacket = badPacket != 0;

    EchoClient client;
    if (!client.init(cfg))
    {
        std::cout << "[main] init 실패 (maxInflight는 최대 패킷 크기 " << MAX_PACKET_SIZE << " ~ 15360)\n";
        WSACleanup();
        return 1;
    }
    std::cout << "[main] EchoClient 시작\n";
    runKeyLoop();
    client.stop();

    WSACleanup();
    return 0;
}
