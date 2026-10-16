#include "Profiler.h"
#include <atomic>
#include <cstdio>
#include <thread>

// 테스트 실행 시간 (초)
constexpr int RUN_SECONDS = 5;

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
    ProfileSetThreadName(name);

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

    // 워커가 기록하는 도중에 1초마다 파일 저장 → 기록/집계 동시 접근 테스트
    int elapsedSeconds = 0;
    while (elapsedSeconds < RUN_SECONDS)
    {
        Sleep(1000);
        ++elapsedSeconds;

        if (ProfileSave() == false)
        {
            printf("ProfileSave failed (%d sec)\n", elapsedSeconds);
        }
    }

    g_running = false;

    t1.join();
    t2.join();

    return 0;
}
