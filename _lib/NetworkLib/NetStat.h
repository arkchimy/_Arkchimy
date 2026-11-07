#pragma once
#include <cstdint>

// 누적 카운터 스냅샷 - 1초 차분으로 초당 값을 만든다
struct NetStat
{
	int64_t AcceptCnt = 0;      // 세션을 만든 수
	int64_t ReleaseCnt = 0;     // 세션을 해제한 수 (현재 세션 수 = accept - release)
	int64_t OverflowCnt = 0;    // 송신 링이 가득 차 끊은 수
	int64_t SendStallCnt = 0;   // 걸린 WSASend가 기준 시간 넘게 안 끝나 끊은 수 - 받지 않거나 너무 느리게 받는 클라
	int64_t SessionFullCnt = 0; // 빈 세션 슬롯이 없어 받자마자 끊은 수
	int64_t BadPacketCnt = 0;   // 헤더 길이가 최대를 넘어 끊은 수
	int64_t RecvPacketCnt = 0;  // 받은 컨텐츠 패킷 수 - 처리량(패킷/초) 교차 확인용
	int64_t SendPacketCnt = 0;  // 송신 링에 넣은 컨텐츠 패킷 수. WSASend 1번당 메시지 수 = 이 값 / 송신 완료 수
	// 시스템 콜 효율 - 완료 1건(= GQCS 1번 + WSARecv/WSASend 1번)에 얼마나 많은 일이 실렸나
	int64_t RecvCompleteCnt = 0; // 데이터가 있는 수신 완료 수
	int64_t SendCompleteCnt = 0; // 성공한 송신 완료 수
	int64_t SendBytes = 0;       // 송신 완료로 보낸 바이트
	int64_t GqcsCallCnt = 0;     // GQCS(또는 GQCSEx) 호출 수
	int64_t IocpCompleteCnt = 0; // IOCP 큐에서 꺼낸 완료 수(해제 작업 포함) - GQCS 호출 1번에 꺼낸 완료 수 = 이 값 / GqcsCallCnt
	// WSASend/WSARecv 반환 결과 - 즉시 성공(0)인지 대기(WSA_IO_PENDING)인지, 각각 반환까지 걸린 시간(QPC 틱 합)
	int64_t SendSyncCnt = 0;
	int64_t SendPendingCnt = 0;
	int64_t SendSyncTicks = 0;
	int64_t SendPendingTicks = 0;
	int64_t RecvSyncCnt = 0;
	int64_t RecvPendingCnt = 0;
	int64_t RecvSyncTicks = 0;
	int64_t RecvPendingTicks = 0;
	int64_t SendWaitTicks = 0; // WSASend를 건 순간부터 그 송신 완료를 처리할 때까지(QPC 틱 합) - 성공한 송신 완료 수로 나눈다
};
