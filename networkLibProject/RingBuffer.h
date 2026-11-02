#pragma once
#include <WinSock2.h>
#include <cstdint>
#include "Memory.h"

// 세션 송수신용 링버퍼. 락은 쓰는 쪽이 건다 (송신 링 = mSendLock 안, 수신 링 = 수신은 세션당 1건이라 락 없음)
// 크기는 링마다 다르다 - 송신 링은 "WSASend에 넘긴 몫 + 아직 안 넘긴 몫"을 담아야 해서 송신 넘침 한도의 2배
class RingBuffer
{
public:
	explicit RingBuffer(const int32_t capacity)
		: mBuffer(NewArray<char>(capacity))
		, mCapacity(capacity)
	{
	}

	~RingBuffer()
	{
		DeleteArray(mBuffer);
	}

	RingBuffer(const RingBuffer&) = delete;
	RingBuffer& operator=(const RingBuffer&) = delete;

	bool Push(const char* src, const int32_t len);	// 남은 공간이 부족하면 아무것도 넣지 않고 false
	bool Peek(char* outBuffer, const int32_t len) const;	// 꺼내지 않고 앞에서 len만큼 복사. 쌓인 양이 부족하면 false
	void Pop(const int32_t len);	// 읽기 위치를 len만큼 전진 (꺼낸 것으로 처리)

	int32_t GetUseSize() const
	{
		return mUseSize;
	}

	int32_t GetFreeSize() const	// 여러 번 Push할 때 전부 들어갈지 먼저 확인용
	{
		return mCapacity - mUseSize;
	}

	int32_t GetCapacity() const
	{
		return mCapacity;
	}

	// WSASend/WSARecv에 링 메모리를 직접 넘기기 위한 함수 - 끝에서 감기면 WSABUF 2개가 된다
	int32_t GetReadBufs(WSABUF* outBufs) const;	// 쌓인 데이터 영역. 반환: 버퍼 수(0이면 빈 상태)
	int32_t GetWriteBufs(WSABUF* outBufs);	// 빈 공간 영역. 반환: 버퍼 수(0이면 가득 참)
	void MoveWrite(const int32_t len);	// 링 메모리에 직접 받은 만큼 쓰기 위치를 전진 (GetWriteBufs와 짝)

	void Clear();
	void Reset(const int32_t capacity);	// 크기를 바꾸고 비운다 - 아무도 쓰지 않을 때만(서버 시작 전)

private:
	char* mBuffer;
	int32_t mCapacity;
	int32_t mRead = 0;	// 다음에 읽을 위치
	int32_t mWrite = 0;	// 다음에 쓸 위치
	int32_t mUseSize = 0;	// 쌓인 바이트 수 - 가득 참/빔을 구분하려고 칸 하나를 비워두지 않는다
};
