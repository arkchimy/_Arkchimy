#include "NetworkLib.h"
#include "ParserLib.h"
#include <cstdio>

int runBacklogServer(USHORT port, int backlog, bool useHint, int seconds, int acceptDelayMs);

int main()
{
    WsadataRAII wsa;
    if (!wsa.mOk)
    {
        return 1;
    }

    // 상대 경로 → 작업 디렉터리 기준 (VS F5 실행 시 프로젝트 폴더)
    utility::stDebug debug;
    auto parser = utility::Parser::CreateParser("server.cfg", debug);
    if (parser == nullptr)
    {
        printf("설정 파일 오류 err=%d key=%s\n", debug.err, debug.lastKey.c_str());
        return 1;
    }

    // from_chars는 bool을 지원하지 않으므로 useHint는 int로 읽는다
    USHORT port;
    int backlog;
    int useHint;
    int waitSec;
    int acceptDelayMs;
    if (!parser->Get("port", port) || !parser->Get("backlog", backlog) || !parser->Get("useHint", useHint) ||
        !parser->Get("waitSec", waitSec) || !parser->Get("acceptDelayMs", acceptDelayMs))
    {
        printf("설정 키 누락 또는 값 오류 (port, backlog, useHint, waitSec, acceptDelayMs)\n");
        return 1;
    }

    return runBacklogServer(port, backlog, useHint != 0, waitSec, acceptDelayMs);
}
