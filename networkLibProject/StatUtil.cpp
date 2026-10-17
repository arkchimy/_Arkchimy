#include "StatUtil.h"
#include <timeapi.h>
#pragma comment(lib, "winmm.lib")

namespace
{
    // 통계 주기 (ms)
    constexpr int STAT_PERIOD_MS = 1000;

    int64_t ToInt64(const FILETIME& time)
    {
        return (static_cast<int64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
    }

    int64_t NowQpc()
    {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        return now.QuadPart;
    }

    int64_t NowWall()
    {
        FILETIME now;
        GetSystemTimeAsFileTime(&now);
        return ToInt64(now);
    }
}

StatTicker::StatTicker()
{
    timeBeginPeriod(1);

    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    mQpcFreq = freq.QuadPart;

    mNextTime = timeGetTime();
    mLastQpc = NowQpc();
}

StatTicker::~StatTicker()
{
    timeEndPeriod(1);
}

double StatTicker::waitNext()
{
    mNextTime += STAT_PERIOD_MS;

    DWORD currentTime = timeGetTime();
    int remain = static_cast<int>(mNextTime - currentTime);   // 부호 있는 차이 (랩어라운드 안전)
    if (remain > 0)
    {
        Sleep(remain);
    }
    else if (remain < -STAT_PERIOD_MS)
    {
        // 디버거 정지 등으로 1주기 이상 밀리면 몰아서 출력하지 않고 기준을 다시 잡음
        mNextTime = currentTime;
    }

    int64_t now = NowQpc();
    double elapsedSec = static_cast<double>(now - mLastQpc) / static_cast<double>(mQpcFreq);
    mLastQpc = now;
    return elapsedSec;
}

CpuSampler::CpuSampler()
{
    mCoreCnt = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    mLastWall = NowWall();

    FILETIME creation;
    FILETIME exit;
    FILETIME kernel;
    FILETIME user;
    GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user);
    mLastProcKernel = ToInt64(kernel);
    mLastProcUser = ToInt64(user);

    FILETIME sysIdle;
    FILETIME sysKernel;
    FILETIME sysUser;
    GetSystemTimes(&sysIdle, &sysKernel, &sysUser);
    mLastSysIdle = ToInt64(sysIdle);
    mLastSysKernel = ToInt64(sysKernel);
    mLastSysUser = ToInt64(sysUser);
}

CpuUsage CpuSampler::sample()
{
    CpuUsage usage = {};

    int64_t wall = NowWall();

    FILETIME creation;
    FILETIME exit;
    FILETIME kernel;
    FILETIME user;
    GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user);
    int64_t procKernel = ToInt64(kernel);
    int64_t procUser = ToInt64(user);

    FILETIME sysIdle;
    FILETIME sysKernel;
    FILETIME sysUser;
    GetSystemTimes(&sysIdle, &sysKernel, &sysUser);
    int64_t idle = ToInt64(sysIdle);
    int64_t sysK = ToInt64(sysKernel);
    int64_t sysU = ToInt64(sysUser);

    // 프로세스: 구간 동안 쓴 CPU 시간 ÷ (구간 길이 × 코어 수) → 작업관리자와 같은 기준
    int64_t wallDelta = wall - mLastWall;
    if (wallDelta > 0)
    {
        double capacity = static_cast<double>(wallDelta) * static_cast<double>(mCoreCnt);
        usage.processUser = 100.0 * static_cast<double>(procUser - mLastProcUser) / capacity;
        usage.processKernel = 100.0 * static_cast<double>(procKernel - mLastProcKernel) / capacity;
    }

    // 시스템: (커널 + 유저 - idle) ÷ (커널 + 유저)
    int64_t sysTotal = (sysK - mLastSysKernel) + (sysU - mLastSysUser);
    if (sysTotal > 0)
    {
        usage.system = 100.0 * static_cast<double>(sysTotal - (idle - mLastSysIdle)) / static_cast<double>(sysTotal);
    }

    mLastWall = wall;
    mLastProcKernel = procKernel;
    mLastProcUser = procUser;
    mLastSysIdle = idle;
    mLastSysKernel = sysK;
    mLastSysUser = sysU;
    return usage;
}
