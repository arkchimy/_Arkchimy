#include "LoadClient.h"
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
        if (key == 'Q')
        {
            quit = true;
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

    LoadClientConfig cfg;
    if (!parser->Get("ip", cfg.ip) || !parser->Get("port", cfg.port) || !parser->Get("threadCnt", cfg.threadCnt) ||
        !parser->Get("overSend", cfg.overSend))
    {
        std::cout << "설정 키 누락 또는 값 오류 (ip, port, threadCnt, overSend)\n";
        WSACleanup();
        return 1;
    }
    parser->Get("sessionPerThread", cfg.sessionPerThread);	// cfg에 없으면 64
    parser->Get("msgSize", cfg.msgSize);	// cfg에 없으면 32
    parser->Get("ratePerSession", cfg.ratePerSession);	// cfg에 없으면 0 (closed-loop)
    parser->Get("roomSize", cfg.roomSize);	// cfg에 없으면 0 (에코)
    parser->Get("heartbeatSec", cfg.heartbeatSec);	// cfg에 없으면 0 (하트비트 안 보냄) - 채팅만
    int chatIdle = 0;
    int chatNoJoin = 0;
    parser->Get("chatIdle", chatIdle);	// 1이면 입장 후 채팅을 보내지 않음(무응답 시험)
    parser->Get("chatNoJoin", chatNoJoin);	// 1이면 Join도 보내지 않음(입장 제한 시험)
    cfg.chatIdle = chatIdle != 0;
    cfg.chatNoJoin = chatNoJoin != 0;
    parser->Get("roomMoveBatches", cfg.roomMoveBatches);	// cfg에 없으면 0 (한 방에만). N이면 자기 묶음 N개마다 다음 방으로 옮긴다
    if (cfg.roomSize > 0)
    {
        int sessionCnt = cfg.threadCnt * cfg.sessionPerThread;
        std::cout << "[main] roomSize = " << cfg.roomSize << " (채팅: 방 " << (sessionCnt + cfg.roomSize - 1) / cfg.roomSize
            << "개 - 서버 chatRoomCnt가 이 이상이어야 한다)\n";
        std::cout << "[main] heartbeatSec = " << cfg.heartbeatSec << ", chatIdle = " << chatIdle << ", chatNoJoin = " << chatNoJoin
            << ", roomMoveBatches = " << cfg.roomMoveBatches << "\n";
    }
    std::cout << "[main] ratePerSession = " << cfg.ratePerSession
        << (cfg.ratePerSession > 0 ? " (open-loop: 세션마다 초당 이만큼, 응답과 상관없이)" : " (closed-loop: 묶음)") << "\n";

    std::cout << "[main] threadCnt = " << cfg.threadCnt << ", sessionPerThread = " << cfg.sessionPerThread
        << " (세션 " << cfg.threadCnt * cfg.sessionPerThread << "개)\n";
    std::cout << "[main] overSend = " << cfg.overSend << " (메시지 " << cfg.msgSize << "B, 묶음 "
        << cfg.overSend * cfg.msgSize << "B)\n";

    LoadClient client;
    if (!client.init(cfg))
    {
        std::cout << "[main] init 실패 (msgSize " << LoadClient::MIN_MSG_SIZE << "~" << MAX_PACKET_SIZE
            << ", msgSize × overSend ≤ " << LoadClient::MAX_BATCH_BYTES << ", sessionPerThread 1~64"
            << ", 채팅이면 msgSize ≥ " << LoadClient::CHAT_MIN_MSG_SIZE << " · roomSize × overSend × msgSize ≤ " << LoadClient::MAX_ROOM_BATCH_BYTES
            << " · closed-loop만)\n";
        WSACleanup();
        return 1;
    }
    std::cout << "[main] LoadClient 시작\n";
    runKeyLoop();
    client.stop();

    WSACleanup();
    return 0;
}
