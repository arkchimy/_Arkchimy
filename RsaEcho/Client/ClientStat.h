#pragma once
#include <atomic>
#include <cstdint>

// 시험 클라 통계 - select 스레드들이 올리고 통계 스레드·main이 읽는다
// ConnectedCnt는 지금 값, 나머지는 누적 값
struct ClientStat
{
	std::atomic<int64_t> ConnectedCnt = 0;
	std::atomic<int64_t> ConnectCnt = 0;
	std::atomic<int64_t> ConnectFailCnt = 0;
	std::atomic<int64_t> HelloCnt = 0;           // 서버 고정 키 서명까지 확인한 HelloClient
	std::atomic<int64_t> RegisterOkCnt = 0;
	std::atomic<int64_t> RegisterExistsCnt = 0;  // 이미 있는 아이디 - 서버를 끄지 않고 클라만 다시 띄우면 나온다(정상)
	std::atomic<int64_t> LoginOkCnt = 0;
	std::atomic<int64_t> EchoCnt = 0;            // 풀고 확인한 에코 응답
	std::atomic<int64_t> BatchCnt = 0;
	std::atomic<int64_t> FinCloseCnt = 0;        // 수명이 다해 끊음
	std::atomic<int64_t> RstCloseCnt = 0;
	std::atomic<int64_t> ProtocolErrorCnt = 0;   // 서명·번호·패딩이 틀림, 기대와 다른 응답, 로그인 실패 - 0이어야 한다
	std::atomic<int64_t> UnexpectedCloseCnt = 0; // 서버가 먼저 끊음 - 0이어야 한다
	std::atomic<int64_t> ShutdownTimeoutCnt = 0; // 종료할 때 응답을 기다리다 시간이 넘음 - 0이어야 한다
};
