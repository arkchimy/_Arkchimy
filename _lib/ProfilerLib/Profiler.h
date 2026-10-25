#pragma once
#include <windows.h>
#include <cstdint>
#include <mutex>

// 스레드 하나가 기록할 수 있는 최대 태그 수
constexpr int MAX_SAMPLE = 64;

// 태그 문자열 최대 길이 (널 문자 포함, 넘으면 잘림)
constexpr int MAX_TAG_LEN = 32;

// 스레드 이름 최대 길이 (널 문자 포함, 넘으면 잘림)
constexpr int MAX_THREAD_NAME_LEN = 32;

// 태그 하나에 대한 통계
struct ProfileSample
{
    char        tag[MAX_TAG_LEN];   // 복사해서 보관 (원본 수명과 무관)
    int64_t     total;    // QPC 틱 단위
    int64_t     min;
    int64_t     max;
    int64_t     maxSeq;   // max가 기록된 호출 순번 (1부터)
    int64_t     count;
};

// 스레드 하나가 가진 기록 전체
struct ThreadProfile
{
    std::mutex    lock;                 // 이 스레드 전용 락 (상대는 집계 스레드뿐)
    char          name[MAX_THREAD_NAME_LEN];   // 출력용 스레드 이름 (기본값 "Thread_<ID>")
    ProfileSample samples[MAX_SAMPLE];
    int           sampleCount;
};

// 스톱워치 객체: 생성자에서 시작, 소멸자에서 정지 후 기록
class ProfileScope
{
public:
    explicit ProfileScope(const char* tag);
    ~ProfileScope();

    ProfileScope(const ProfileScope&) = delete;
    ProfileScope& operator=(const ProfileScope&) = delete;

private:
    ProfileSample* mSample;   // 생성자에서 찾아둔 항목 (t_profile 안의 칸)
    int64_t        mStart;    // 시작 시각 (QPC 틱)
};

namespace Profile
{
    // 기록 켜기/끄기 (기본 켬). 끄면 ProfileScope가 시간을 재지 않고 바로 빠져나간다
    // 처리량 측정처럼 프로파일러 비용 자체가 결과를 흐리면 안 될 때 끈다. 측정 시작 전에 한 번 정한다
    void SetEnabled(bool enabled);

    // 지금 스레드의 출력용 이름 지정 (예: "AcceptThread_1"), 스레드 시작 시 1회 호출
    void SetThreadName(const char* name);

    // 모든 스레드의 통계를 "프로젝트명_날짜_시간_Profiler.txt" 파일로 저장 (실패 시 false)
    // 날짜/시간은 exe 실행 시각 기준이라 실행 중에는 같은 파일에 덮어씀
    bool Save();

    // 모든 스레드의 통계(total, min, max, maxSeq, count)를 초기화, 태그와 스레드 이름은 유지
    void Reset();
}
