#include "Profiler.h"
#include <atomic>
#include <cctype>
#include <conio.h>
#include <cstdio>
#include <thread>

// 워커 종료 신호 (메인이 쓰고 워커들이 읽음)
std::atomic<bool> g_running = true;

// 측정 대상 예시 함수
void Work()
{
    ProfileScope scope("Work");

    volatile int sum = 0;
    for (int i = 0; i < 10000; ++i)
    {
        sum += i;
    }
}

void ThreadMain(int index)
{
    char name[MAX_THREAD_NAME_LEN];
    sprintf_s(name, MAX_THREAD_NAME_LEN, "WorkerThread_%d", index);
    Profile::SetThreadName(name);

    // 메인이 종료 신호를 줄 때까지 계속 기록
    while (g_running)
    {
        Work();
    }
}

int main()
{
    std::thread t1(ThreadMain, 1);
    std::thread t2(ThreadMain, 2);

    printf("[A] Save  [D] Reset  [Q] Quit\n");

    // 워커가 기록하는 도중에 키 입력으로 저장/리셋 → 기록/집계 동시 접근 테스트
    // _getch는 키를 누를 때까지 메인 스레드만 대기 (워커는 계속 기록)
    bool quit = false;
    while (quit == false)
    {
        int key = toupper(_getch());

        switch (key)
        {
        case 'A':
            if (Profile::Save())
            {
                printf("Saved\n");
            }
            else
            {
                printf("Profile::Save failed\n");
            }
            break;

        case 'D':
            Profile::Reset();
            printf("Reset\n");
            break;

        case 'Q':
            quit = true;
            break;

        default:
            break;
        }
    }

    g_running = false;

    t1.join();
    t2.join();

    return 0;
}
