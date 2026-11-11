#pragma once
#include <WinSock2.h>
#include <cstdint>
#include <mutex>
#include "IoOverlapped.h"
#include "NetConfig.h"
#include "RingBuffer.h"

// 세션 하나의 상태
// 라이브러리 안에서만 다룬다(컨텐츠는 세션 ID로만). 멤버는 NetworkLib이 friend로 직접 쓴다

constexpr int64_t RELEASE_FLAG = INT64_MIN;	// 해제 표시 - 해제 작업을 넣은 뒤부터 다음 수락까지. 빈 슬롯도 이 값으로 시작

class Session
{
	friend class NetworkLib;

private:
	SOCKET mSock = INVALID_SOCKET;
	int32_t mIdx = 0;   // 슬롯 번호 - 해제할 때 빈 슬롯 스택에 돌려준다
	uint64_t mID = 0;   // 세션 ID = (세대 << 32) | 슬롯 번호. 슬롯을 다시 쓸 때마다 세대가 올라 이전 ID와 겹치지 않는다

	// 여러 스레드가 쓰는 변수는 volatile + Interlocked 함수로 쓴다(일반 대입·++ 금지) - CODING_STANDARD "스레드 공유 변수"
	volatile LONG mbDisconnect = 0; // 릴리즈 Flag 해제할 때 0으로 돌아감
	volatile LONG mHoldCnt = 0;     // 워커 밖 스레드가 붙잡은 수(HoldSession +1, UnholdSession -1). 음수면 오용, 해제 때는 0
	volatile LONG mbSendTarget = 0; // 모아보내는 Flag 1이면 어느 워커의 보낼 목록(tlsSendTargets)에 올라 있음
	bool mbSendFlag = false;        // WSASend Flag - 세션당 송신 1개.
	int64_t mWsaSendQPC = 0;        // 지금 걸린 송신을 건 시각 - 송신 완료까지 걸린 시간 측정과 송신 정체 판정용. 워커 밖 스레드가 워커에 맡긴 동안은 INT64_MAX(아직 WSASend 전)

	MyOverlapped mRecvOv{};
	MyOverlapped mSendOv{};
	MyOverlapped mReleaseOv{};

	RingBuffer<RECV_RING_SIZE> mRecvBuf; // 받은 바이트. 완성된 패킷만 꺼내고, 잘린 나머지는 다음 수신까지 남는다 (수신은 세션당 1건이라 락 없음)
	std::mutex mSendLock;                // mSendBuf, mbSendFlag 보호
	RingBuffer<SEND_RING_SIZE> mSendBuf; // 보낼 데이터(완료를 기다리는 몫 포함). 넣을 자리가 없으면 세션을 끊는다

	alignas(64) volatile int64_t mIoCount = RELEASE_FLAG; // 잡고 있는 몫(I/O + 사용 중 스레드). 0이면 해제, 63번 비트 = 해제 표시
};
