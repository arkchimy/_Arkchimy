// 원자 카운터 크기별 비용 비교 - Session::mIoCount(std::atomic<int32_t>)를 더 작은 타입으로 바꿀 이유가 있는지
// 스레드마다 같은 변수에 "++ 한 번, -- 한 번(결과 비교)"을 반복한다 - decIoCount의 `--count != 0`과 같은 모양
// 스레드 1개 = 경합 없음, 2개 이상 = 같은 캐시 라인을 여러 코어가 빼앗는 경합 (서버에서 여러 워커가 같은 세션을 건드리는 경우)
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

namespace
{
	template <typename T>
	struct alignas(64) Counter
	{
		std::atomic<T> Value{ 0 };
	};

	std::atomic<int> gSink{ 0 };	// 결과를 버리지 않게 - 컴파일러가 반복을 없애지 못하도록

	// 연산(증가 또는 감소) 1번당 평균 ns. 모든 스레드가 함께 출발한다
	template <typename T>
	double runOnce(const int threadCnt, const int64_t pairsPerThread)
	{
		Counter<T> counter;
		std::atomic<int> ready{ 0 };
		std::atomic<bool> go{ false };
		std::vector<std::thread> threads;
		for (int t = 0; t < threadCnt; ++t)
		{
			threads.emplace_back([&]()
			{
				++ready;
				while (!go.load(std::memory_order_acquire))
				{
				}
				int zeroCnt = 0;
				for (int64_t i = 0; i < pairsPerThread; ++i)
				{
					++counter.Value;	// ++session.mIoCount 와 같은 모양
					if (--counter.Value == 0)	// decIoCount의 if (--session.mIoCount != 0) 과 같은 모양
					{
						++zeroCnt;
					}
				}
				gSink += zeroCnt;
			});
		}
		while (ready.load() != threadCnt)
		{
		}
		const auto start = std::chrono::steady_clock::now();
		go.store(true, std::memory_order_release);
		for (std::thread& th : threads)
		{
			th.join();
		}
		const auto end = std::chrono::steady_clock::now();
		const double ns = std::chrono::duration<double, std::nano>(end - start).count();
		return ns / static_cast<double>(pairsPerThread * 2);	// 스레드 하나가 연산 1번에 쓴 시간(모든 스레드가 동시에 돈다)
	}

	template <typename T>
	void addSample(const int threadCnt, const int64_t pairs, std::vector<double>& samples)
	{
		samples.push_back(runOnce<T>(threadCnt, pairs));
	}
}

int main()
{
	const int threadCounts[] = { 1, 2, 4, 6 };
	const char* names[] = { "int8_t", "int16_t", "int32_t", "int64_t" };
	constexpr int TYPE_CNT = 4;
	constexpr int ROUNDS = 5;

	std::printf("원자 카운터 ++/-- 비용 (ns/연산, %d회 중앙값) - 스레드 1 = 경합 없음\n", ROUNDS);
	std::printf("%-8s", "스레드");
	for (const char* name : names)
	{
		std::printf("%10s", name);
	}
	std::printf("\n");

	for (const int threadCnt : threadCounts)
	{
		const int64_t pairs = (threadCnt == 1) ? 20'000'000 : 3'000'000;	// 경합이 있으면 연산이 수십 배 느려 반복을 줄인다
		std::vector<double> samples[TYPE_CNT];
		for (int r = 0; r < ROUNDS; ++r)
		{
			for (int k = 0; k < TYPE_CNT; ++k)
			{
				const int type = (k + r) % TYPE_CNT;	// 회차마다 순서를 돌려 시간에 따른 변화(클럭 등)가 한 타입에 쏠리지 않게
				switch (type)
				{
				case 0: addSample<int8_t>(threadCnt, pairs, samples[0]); break;
				case 1: addSample<int16_t>(threadCnt, pairs, samples[1]); break;
				case 2: addSample<int32_t>(threadCnt, pairs, samples[2]); break;
				default: addSample<int64_t>(threadCnt, pairs, samples[3]); break;
				}
			}
		}
		std::printf("%-8d", threadCnt);
		for (int k = 0; k < TYPE_CNT; ++k)
		{
			std::sort(samples[k].begin(), samples[k].end());
			std::printf("%10.2f", samples[k][ROUNDS / 2]);
		}
		std::printf("   (범위 int8 %.2f-%.2f, int32 %.2f-%.2f)\n", samples[0].front(), samples[0].back(), samples[2].front(), samples[2].back());
	}
	std::printf("(sink %d)\n", gSink.load());
	return 0;
}
