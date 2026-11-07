#pragma once
#include <WinSock2.h>
#include <cstdint>
#include "Assert.h"

// 워커별 카운터 - 핫패스에서 매 완료마다 올리므로 워커끼리 같은 캐시 라인을 쓰지 않게 64바이트 정렬
// 쓰는 워커는 하나라 Interlocked 대신 읽고 쓰기로 올린다(AddSingleWriter, lock 접두 명령 없음). 이름은 NetStat의 같은 항목과 맞춘다
struct alignas(64) WorkerCounter
{
	volatile LONG64 RecvPacketCnt = 0;
	volatile LONG64 SendPacketCnt = 0;
	volatile LONG64 RecvCompleteCnt = 0;
	volatile LONG64 SendCompleteCnt = 0;
	volatile LONG64 SendBytes = 0;
	volatile LONG64 GqcsCallCnt = 0;
	volatile LONG64 IocpCompleteCnt = 0;
	volatile LONG64 SendSyncCnt = 0;
	volatile LONG64 SendPendingCnt = 0;
	volatile LONG64 SendSyncTicks = 0;
	volatile LONG64 SendPendingTicks = 0;
	volatile LONG64 RecvSyncCnt = 0;
	volatile LONG64 RecvPendingCnt = 0;
	volatile LONG64 RecvSyncTicks = 0;
	volatile LONG64 RecvPendingTicks = 0;
	volatile LONG64 SendWaitTicks = 0;
};

// 단일 쓰기 카운터 증가 - 쓰는 스레드가 하나뿐이라 원자적 RMW(InterlockedAdd64) 대신 읽고 쓰기. 다른 스레드는 읽기만 한다
// x64에서 정렬된 8바이트 읽기·쓰기는 한 번에 일어나므로 읽는 쪽이 반쯤 바뀐 값을 보지 않는다
inline void AddSingleWriter(volatile LONG64& counter, const int64_t value)
{
	counter = counter + value;
}

// WSARecv/WSASend 반환 결과를 즉시 성공 / 대기로 나눠 센다. 즉시 실패(끊긴 소켓 등)는 세지 않는다
inline void CountIoResult(const int ret, const int err, const int64_t ticks,
	volatile LONG64* outSyncCnt, volatile LONG64* outSyncTicks,
	volatile LONG64* outPendingCnt, volatile LONG64* outPendingTicks)
{
	Assert(outSyncCnt != nullptr);
	Assert(outSyncTicks != nullptr);
	Assert(outPendingCnt != nullptr);
	Assert(outPendingTicks != nullptr);
	if (ret == 0)
	{
		AddSingleWriter(*outSyncCnt, 1);
		AddSingleWriter(*outSyncTicks, ticks);
	}
	else if (err == WSA_IO_PENDING)
	{
		AddSingleWriter(*outPendingCnt, 1);
		AddSingleWriter(*outPendingTicks, ticks);
	}
}
