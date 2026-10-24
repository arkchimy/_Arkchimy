#pragma once
#include <WinSock2.h>
#include <cstdint>

// 세션 송수신용 링버퍼. 락은 쓰는 쪽이 건다 (송신 링 = mSendLock 안, 수신 링 = 수신은 세션당 1건이라 락 없음)
class RingBuffer
{
public:
	static constexpr int32_t CAPACITY = 16384;

	bool push(const char* src, int32_t len);	// 남은 공간이 부족하면 아무것도 넣지 않고 false
	bool peek(char* dst, int32_t len) const;	// 꺼내지 않고 앞에서 len만큼 복사. 쌓인 양이 부족하면 false
	void pop(int32_t len);	// 읽기 위치를 len만큼 전진 (꺼낸 것으로 처리)
	int32_t getUseSize() const { return mUseSize; }

	// WSASend/WSARecv에 링 메모리를 직접 넘기기 위한 함수 - 끝에서 감기면 WSABUF 2개가 된다
	int32_t getReadBufs(WSABUF* bufs) const;	// 쌓인 데이터 영역. 반환: 버퍼 수(0이면 빈 상태)
	int32_t getWriteBufs(WSABUF* bufs);	// 빈 공간 영역. 반환: 버퍼 수(0이면 가득 참)
	void moveWrite(int32_t len);	// 링 메모리에 직접 받은 만큼 쓰기 위치를 전진 (getWriteBufs와 짝)

	void clear();

private:
	char mBuffer[CAPACITY];
	int32_t mRead = 0;	// 다음에 읽을 위치
	int32_t mWrite = 0;	// 다음에 쓸 위치
	int32_t mUseSize = 0;	// 쌓인 바이트 수 - 가득 참/빔을 구분하려고 칸 하나를 비워두지 않는다
};
