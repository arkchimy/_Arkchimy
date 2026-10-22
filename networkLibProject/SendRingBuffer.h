#pragma once
#include <WinSock2.h>
#include <cstdint>

// 세션 송신용 링버퍼. 모든 호출은 mSendLock 안에서 한다
class SendRingBuffer
{
public:
	static constexpr int32_t CAPACITY = 16384;

	bool push(const char* src, int32_t len);	// 남은 공간이 부족하면 아무것도 넣지 않고 false
	int32_t getSendBufs(WSABUF* bufs);	// 쌓인 데이터를 WSABUF 최대 2개로 채운다. 반환: 버퍼 수(0이면 빈 상태)
	void pop(int32_t len);	// 보낸 만큼 읽기 위치를 전진
	void clear();

private:
	char mBuffer[CAPACITY];
	int32_t mRead = 0;	// 다음에 보낼 위치
	int32_t mWrite = 0;	// 다음에 쓸 위치
	int32_t mUseSize = 0;	// 쌓인 바이트 수 - 가득 참/빔을 구분하려고 칸 하나를 비워두지 않는다
};
