#pragma once
#include <atomic>
#include <cstdint>

// 시험 클라 통계 - select 스레드만 올리고 통계 스레드·main이 읽는다(쓰는 스레드가 하나라 락 없음, 읽는 쪽 때문에 atomic)
// 모두 누적 값
struct ClientStat
{
	std::atomic<int64_t> ConnectedCnt = 0;      // 지금 연결된 세션 수
	std::atomic<int64_t> ConnectCnt = 0;        // 접속 성공 수
	std::atomic<int64_t> ConnectFailCnt = 0;    // 접속 실패 수
	std::atomic<int64_t> BatchCnt = 0;          // 응답까지 모두 확인한 묶음 수(정상 세션)
	std::atomic<int64_t> FinCloseCnt = 0;       // 정상 세션이 FIN으로 끊은 수
	std::atomic<int64_t> RstCloseCnt = 0;       // 정상 세션이 RST로 끊은 수
	std::atomic<int64_t> BadHeaderSentCnt = 0;  // 범위 밖 헤더를 보낸 수 - 서버 BadPacketCnt와 같아야 한다
	std::atomic<int64_t> BadHeaderClosedCnt = 0; // 범위 밖 헤더를 보낸 뒤 서버가 끊은 것을 확인한 수
	std::atomic<int64_t> BadHeaderSendFailCnt = 0; // 범위 밖 헤더를 보내다 실패한 수(접속 직후 서버가 끊음 등) - 서버 BadPacketCnt에 안 잡힌 시도
	std::atomic<int64_t> UnexpectedCloseCnt = 0; // 정상 세션인데 서버가 먼저 끊은 수 - 0이어야 한다
	std::atomic<int64_t> ShutdownTimeoutCnt = 0; // 종료할 때 응답을 기다리다 시간이 넘은 세션 수 - 0이어야 한다
};
