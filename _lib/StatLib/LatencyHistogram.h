#pragma once
#include <atomic>
#include <bit>
#include <cstdint>
#include <vector>

// 지연 시간(µs) 히스토그램 - 칸 폭이 값에 비례(2의 거듭제곱 구간마다 64칸)해서 상대 오차 ~1.6%로 1µs~수 일을 담는다
// 0~127µs는 1µs 단위로 정확히 센다
// 쓰는 스레드는 하나(워커별 1개)라 원자적 증가(lock xadd) 없이 load + store로 센다
// 읽는 쪽은 조금 늦은 값을 볼 수 있을 뿐 찢어진 값은 보지 않는다
class alignas(64) LatencyHistogram
{
public:
	static constexpr int SUB_BITS = 6;
	static constexpr int SUB_COUNT = 1 << SUB_BITS;	// 구간당 칸 수
	static constexpr int LINEAR_COUNT = SUB_COUNT * 2;	// 0~127µs는 1µs 단위
	static constexpr int MAX_MSB = 40;	// 2^41µs(약 25일) 이상은 마지막 칸
	static constexpr int BUCKET_COUNT = LINEAR_COUNT + (MAX_MSB - SUB_BITS) * SUB_COUNT;

	void record(uint64_t us)
	{
		std::atomic<uint64_t>& c = mCounts[bucketOf(us)];
		c.store(c.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
	}

	// 여러 워커의 히스토그램을 합칠 때 쓴다. counts.size() == BUCKET_COUNT
	void addTo(std::vector<uint64_t>& counts) const
	{
		for (int i = 0; i < BUCKET_COUNT; ++i)
		{
			counts[i] += mCounts[i].load(std::memory_order_relaxed);
		}
	}

	static int bucketOf(uint64_t us)
	{
		if (us < LINEAR_COUNT)
		{
			return static_cast<int>(us);
		}
		int msb = 63 - std::countl_zero(us);	// 7 이상
		if (msb > MAX_MSB)
		{
			return BUCKET_COUNT - 1;
		}
		int shift = msb - SUB_BITS;	// us >> shift 가 [64, 127]에 들어오게
		return LINEAR_COUNT + (shift - 1) * SUB_COUNT + static_cast<int>((us >> shift) - SUB_COUNT);
	}

	// 칸에 들어가는 가장 큰 값 - 백분위수를 실제보다 작게 말하지 않도록 위쪽 경계를 쓴다
	static uint64_t bucketUpper(int idx)
	{
		if (idx < LINEAR_COUNT)
		{
			return static_cast<uint64_t>(idx);
		}
		int k = idx - LINEAR_COUNT;
		int shift = k / SUB_COUNT + 1;
		uint64_t mantissa = SUB_COUNT + k % SUB_COUNT;
		return ((mantissa + 1) << shift) - 1;
	}

private:
	std::atomic<uint64_t> mCounts[BUCKET_COUNT] = {};
};

struct LatencySummary
{
	uint64_t count = 0;
	uint64_t p50 = 0;
	uint64_t p99 = 0;
	uint64_t p999 = 0;
	uint64_t max = 0;
};

// 칸별 개수(누적이든 1초 차분이든)에서 백분위수를 구한다. 값은 칸의 위쪽 경계(µs)
inline LatencySummary summarizeLatency(const std::vector<uint64_t>& counts)
{
	LatencySummary s;
	for (uint64_t c : counts)
	{
		s.count += c;
	}
	if (s.count == 0)
	{
		return s;
	}

	// 순위 = ceil(q * count) - 그 순위의 표본이 들어 있는 칸
	uint64_t rank50 = (s.count * 50 + 99) / 100;
	uint64_t rank99 = (s.count * 99 + 99) / 100;
	uint64_t rank999 = (s.count * 999 + 999) / 1000;
	uint64_t seen = 0;
	bool found50 = false;	// 값 0(0µs 칸)도 답일 수 있어 0으로 "아직 못 찾음"을 표시할 수 없다
	bool found99 = false;
	bool found999 = false;
	for (int i = 0; i < static_cast<int>(counts.size()); ++i)
	{
		if (counts[i] == 0)
		{
			continue;
		}
		seen += counts[i];
		uint64_t upper = LatencyHistogram::bucketUpper(i);
		if (!found50 && seen >= rank50)
		{
			s.p50 = upper;
			found50 = true;
		}
		if (!found99 && seen >= rank99)
		{
			s.p99 = upper;
			found99 = true;
		}
		if (!found999 && seen >= rank999)
		{
			s.p999 = upper;
			found999 = true;
		}
		s.max = upper;
	}
	return s;
}
