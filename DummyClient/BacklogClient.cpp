// backlog 실험 클라이언트: 설정된 횟수만큼 connect를 시도하고 결과를 집계한다
// 서버는 accept를 하지 않으므로 성공 개수 = 실제 큐 크기
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <cstdio>
#include <map>
#include <vector>

int runBacklogClient(const char *ip, USHORT port, int count)
{
    SOCKADDR_IN addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1)
    {
        printf("ip 형식 오류: %s\n", ip);
        return 1;
    }

    std::vector<SOCKET> connected;
    std::map<int, int> errCount; // 에러코드 → 발생 횟수
    int failCount = 0;
    int firstFailIndex = -1;  // 처음 실패한 시도 번호 = 큐 크기
    int successAfterFail = 0; // 첫 실패 이후 다시 성공한 횟수 (0이어야 "큐 크기만큼 정확히 받고 이후 실패")

    for (int i = 1; i <= count; ++i)
    {
        SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock == INVALID_SOCKET)
        {
            printf("socket 실패 err=%d\n", WSAGetLastError());
            break;
        }

        if (connect(sock, reinterpret_cast<SOCKADDR *>(&addr), sizeof(addr)) == 0)
        {
            // 닫지 않고 들고 있는다 → 서버 큐에 계속 남게 한다
            connected.push_back(sock);
            if (firstFailIndex >= 0)
            {
                ++successAfterFail;
            }
            continue;
        }

        // closesocket이 에러값을 덮어쓸 수 있으므로 먼저 저장한다
        int err = WSAGetLastError();
        ++errCount[err];
        ++failCount;
        if (firstFailIndex < 0)
        {
            firstFailIndex = i;
        }
        printf("#%d 실패 err=%d\n", i, err);
        closesocket(sock);
    }

    printf("시도 %d회\n", count);
    printf("성공 %zu개\n", connected.size());
    printf("실패 %d개\n", failCount);
    printf("첫 실패: #%d\n", firstFailIndex);
    printf("첫 실패 이후 성공: %d회\n", successAfterFail);
    for (const auto &[err, cnt] : errCount)
    {
        printf("err=%d : %d회\n", err, cnt);
    }
    Sleep(INFINITE);
    for (SOCKET s : connected)
    {
        closesocket(s);
    }
    return 0;
}
