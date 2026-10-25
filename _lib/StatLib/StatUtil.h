#pragma once
#include <windows.h>
#include <cstdint>

// 1초 주기 통계 스레드용 타이머
// 스케줄: 목표 시각을 누적(mNextTime += 1000)해서 Sleep 오차가 쌓이지 않게 함
// 계산:   각 구간의 실제 길이는 QPC로 재서 돌려줌 → 늦게 깬 구간의 tps가 부풀지 않음
class StatTicker
{
public:
    StatTicker();     // timeBeginPeriod(1), 기준 시각 기록
    ~StatTicker();    // timeEndPeriod(1)

    StatTicker(const StatTicker&) = delete;
    StatTicker& operator=(const StatTicker&) = delete;

    // 다음 목표 시각까지 대기 후, 직전 호출 이후 실제 경과 시간(초)을 반환
    double waitNext();

private:
    DWORD   mNextTime;    // 이상적인 다음 깨어날 시각 (timeGetTime 기준 ms)
    int64_t mLastQpc;     // 직전 구간이 끝난 시각 (QPC 틱)
    int64_t mQpcFreq;
};

// CPU 사용률 (직전 sample 호출 이후 구간, 전체 코어 대비 %)
struct CpuUsage
{
    double processUser;     // 이 프로세스 유저 모드
    double processKernel;   // 이 프로세스 커널 모드
    double system;          // 시스템 전체 (다른 프로세스 포함)
};

class CpuSampler
{
public:
    CpuSampler();   // 기준값 기록

    // 직전 호출(또는 생성) 이후 구간의 CPU 사용률
    CpuUsage sample();

private:
    int64_t mCoreCnt;
    int64_t mLastWall;            // 벽시계 (100ns 단위)
    int64_t mLastProcUser;
    int64_t mLastProcKernel;
    int64_t mLastSysIdle;
    int64_t mLastSysKernel;       // GetSystemTimes의 커널 시간은 idle을 포함함
    int64_t mLastSysUser;
};
