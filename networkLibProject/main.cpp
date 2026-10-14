#include "NetworkLib.h"
#include <cstdio>

int main()
{
    WsadataRAII wsa;
    if (!wsa.mOk)
    {
        return 1;
    }

    NetworkLib network;
    if (!network.initListenSocket(32000, 512) || !network.initIocp(4))
    {
        return 1;
    }

    printf("[main] 리슨 시작. Enter 입력 시 종료\n");
    getchar();

    network.stopWorkers();
    return 0;
}
