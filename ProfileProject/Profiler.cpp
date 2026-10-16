#include "Profiler.h"
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <vector>

// 집계용 전역 목록: 모든 스레드의 ThreadProfile 주소 (해제하지 않음)
static std::mutex                  g_registryLock;
static std::vector<ThreadProfile*> g_profiles;

// 스레드마다 별도로 존재하는 포인터 (실제 데이터는 힙에 있어 스레드 종료 후에도 남음)
thread_local ThreadProfile* t_profile = nullptr;

// 지금 스레드의 ThreadProfile 반환, 처음 호출이면 생성 후 전역 목록에 등록
static ThreadProfile* GetThreadProfile()
{
    if (t_profile != nullptr)
    {
        return t_profile;
    }

    // () 를 붙여 값 초기화 → samples, sampleCount 모두 0
    ThreadProfile* profile = new ThreadProfile();

    // 이름을 지정하지 않은 스레드도 구분되도록 기본 이름 부여
    sprintf_s(profile->name, MAX_THREAD_NAME_LEN, "Thread_%lu", GetCurrentThreadId());

    {
        // 스레드당 한 번만 실행되므로 전역 락이어도 기록 경로에 영향 없음
        std::lock_guard<std::mutex> guard(g_registryLock);
        g_profiles.push_back(profile);
    }

    t_profile = profile;
    return profile;
}

void ProfileSetThreadName(const char* name)
{
    ThreadProfile* profile = GetThreadProfile();

    // ProfileSave가 이름을 읽는 도중에 바뀌지 않도록 락 안에서 복사
    std::lock_guard<std::mutex> guard(profile->lock);
    strncpy_s(profile->name, MAX_THREAD_NAME_LEN, name, _TRUNCATE);
}

ProfileScope::ProfileScope(const char* tag)
{
    ThreadProfile& profile = *GetThreadProfile();

    // 1. 같은 태그가 이미 있는지 탐색
    ProfileSample* sample = nullptr;
    for (int i = 0; i < profile.sampleCount; ++i)
    {
        // 저장된 태그는 잘렸을 수 있으므로 저장 가능한 길이까지만 비교
        if (strncmp(profile.samples[i].tag, tag, MAX_TAG_LEN - 1) == 0)
        {
            sample = &profile.samples[i];
            break;
        }
    }

    // 2. 없으면 새 칸 추가 (가득 찼으면 nullptr → 소멸자에서 기록 생략)
    if (sample == nullptr && profile.sampleCount < MAX_SAMPLE)
    {
        // 집계 스레드가 sampleCount와 새 칸을 읽을 수 있으므로 쓰기는 락 안에서
        std::lock_guard<std::mutex> guard(profile.lock);

        sample = &profile.samples[profile.sampleCount];
        ++profile.sampleCount;

        strncpy_s(sample->tag, MAX_TAG_LEN, tag, _TRUNCATE);   // 넘치면 잘라서 널 종료
        sample->total = 0;
        sample->min   = INT64_MAX;   // 첫 기록이 무조건 min이 되도록
        sample->max    = 0;
        sample->maxSeq = 0;
        sample->count  = 0;
    }

    mSample = sample;

    // 3. 시작 시각은 맨 마지막에 읽음 (탐색 비용이 측정값에 섞이지 않게)
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    mStart = now.QuadPart;
}

ProfileScope::~ProfileScope()
{
    // 1. 종료 시각은 맨 먼저 읽음 (갱신 비용이 측정값에 섞이지 않게)
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);

    // 생성자에서 칸을 못 받았으면 (MAX_SAMPLE 초과) 기록 생략
    if (mSample == nullptr)
    {
        return;
    }

    // 2. 통계 갱신
    int64_t elapsed = now.QuadPart - mStart;

    // 생성자에서 GetThreadProfile()을 거쳤으므로 t_profile은 nullptr이 아님
    std::lock_guard<std::mutex> guard(t_profile->lock);

    mSample->total += elapsed;
    ++mSample->count;

    if (elapsed < mSample->min)
    {
        mSample->min = elapsed;
    }

    if (elapsed > mSample->max)
    {
        mSample->max    = elapsed;
        mSample->maxSeq = mSample->count;   // 위에서 증가시킨 뒤라 1부터 시작
    }
}

// 파일 저장 직렬화용 (여러 스레드가 동시에 ProfileSave를 호출하면 같은 파일에 섞여 써지므로)
static std::mutex g_saveLock;

// "프로젝트명_날짜_시간_Profiler.txt" 형식의 파일 이름 생성
// 프로젝트명 = 실행 파일 이름, 날짜/시간 = 프로세스 시작 시각 → 실행 한 번에 파일 이름 하나
static void MakeProfileFileName(char* outFileName, int outSize)
{
    char exePath[MAX_PATH];
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);

    // 경로 제거: "C:\...\ProfileProject.exe" → "ProfileProject.exe"
    const char* exeName = strrchr(exePath, '\\');
    if (exeName != nullptr)
    {
        ++exeName;
    }
    else
    {
        exeName = exePath;
    }

    // 확장자 제거: "ProfileProject.exe" → "ProfileProject"
    char projectName[MAX_PATH];
    strncpy_s(projectName, MAX_PATH, exeName, _TRUNCATE);

    char* dot = strrchr(projectName, '.');
    if (dot != nullptr)
    {
        *dot = '\0';
    }

    // 프로세스 시작 시각 (UTC) → 로컬 시각으로 변환
    FILETIME creationTime;
    FILETIME exitTime;
    FILETIME kernelTime;
    FILETIME userTime;
    GetProcessTimes(GetCurrentProcess(), &creationTime, &exitTime, &kernelTime, &userTime);

    FILETIME localCreationTime;
    FileTimeToLocalFileTime(&creationTime, &localCreationTime);

    SYSTEMTIME start;
    FileTimeToSystemTime(&localCreationTime, &start);

    sprintf_s(outFileName, outSize, "%s_%04d%02d%02d_%02d%02d%02d_Profiler.txt",
        projectName,
        start.wYear, start.wMonth, start.wDay,
        start.wHour, start.wMinute, start.wSecond);
}

bool ProfileSave()
{
    std::lock_guard<std::mutex> saveGuard(g_saveLock);

    char fileName[MAX_PATH];
    MakeProfileFileName(fileName, MAX_PATH);

    // "w" 모드: 파일이 있으면 내용을 비우고 처음부터 씀 (덮어쓰기)
    FILE* file = nullptr;
    if (fopen_s(&file, fileName, "w") != 0 || file == nullptr)
    {
        return false;
    }

    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    double tickToUs = 1000000.0 / static_cast<double>(freq.QuadPart);

    // 1. 전역 목록은 복사만 하고 바로 락 해제 (출력 중에 새 스레드 등록을 막지 않도록)
    std::vector<ThreadProfile*> profiles;
    {
        std::lock_guard<std::mutex> guard(g_registryLock);
        profiles = g_profiles;
    }

    // 2. 스레드별 복사본 생성 (스레드 락은 복사하는 동안만 잡음 → 파일 쓰는 동안 기록 스레드가 기다리지 않도록)
    struct ThreadSnapshot
    {
        char          name[MAX_THREAD_NAME_LEN];
        ProfileSample samples[MAX_SAMPLE];
        int           sampleCount;
    };

    std::vector<ThreadSnapshot> snapshots(profiles.size());   // 스레드당 약 4KB라 스택 대신 힙

    for (size_t i = 0; i < profiles.size(); ++i)
    {
        ThreadProfile*  profile  = profiles[i];
        ThreadSnapshot& snapshot = snapshots[i];

        std::lock_guard<std::mutex> guard(profile->lock);
        memcpy(snapshot.name, profile->name, MAX_THREAD_NAME_LEN);
        snapshot.sampleCount = profile->sampleCount;
        memcpy(snapshot.samples, profile->samples, sizeof(ProfileSample) * snapshot.sampleCount);
    }

    // 3. 스레드 이름순 정렬 (등록 순서는 실행마다 달라서 출력 순서가 흔들림)
    std::sort(snapshots.begin(), snapshots.end(),
        [](const ThreadSnapshot& a, const ThreadSnapshot& b)
        {
            return strcmp(a.name, b.name) < 0;
        });

    // 4. 락 밖에서 파일에 기록
    for (const ThreadSnapshot& snapshot : snapshots)
    {
        fprintf(file, "[%s]\n", snapshot.name);
        fprintf(file, "  %-31s %10s %14s %14s %14s %10s\n", "Tag", "Count", "Avg", "Min", "Max", "MaxSeq");

        for (int i = 0; i < snapshot.sampleCount; ++i)
        {
            const ProfileSample& sample = snapshot.samples[i];

            // 칸만 추가되고 아직 소멸자가 안 돈 경우 (min이 INT64_MAX 상태)
            if (sample.count == 0)
            {
                continue;
            }

            double avg = static_cast<double>(sample.total) / static_cast<double>(sample.count);

            fprintf(file, "  %-31s %10lld %12.3fus %12.3fus %12.3fus %10lld\n",
                sample.tag,
                sample.count,
                avg * tickToUs,
                static_cast<double>(sample.min) * tickToUs,
                static_cast<double>(sample.max) * tickToUs,
                sample.maxSeq);
        }

        fprintf(file, "\n");
    }

    fclose(file);
    return true;
}
