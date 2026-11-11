#pragma once
#include <WinSock2.h>
#include <cstdint>
#include "Assert.h"
#include "Memory.h"

// 세션 송수신용 링버퍼. 락은 쓰는 쪽이 건다 (송신 링 = mSendLock 안, 수신 링 = 수신은 세션당 1건이라 락 없음)
// 크기는 컴파일 시점에 정한다 - 세션 안에 배열로 들어가 할당이 따로 없다(세션 배열을 만들 때 한 번에 잡힌다)
template <int32_t CAPACITY>
class RingBuffer
{
	static_assert(CAPACITY > 0, "링 크기는 0보다 커야 한다");

public:
	RingBuffer();
	RingBuffer(const RingBuffer&) = delete;
	RingBuffer& operator=(const RingBuffer&) = delete;

	// 남은 공간이 부족하면 아무것도 넣지 않고 false
	bool Push(const char* src, const int32_t len)
	{
		if (CAPACITY - mUseSize < len)
		{
			return false;
		}

		// 버퍼 끝까지 남은 칸에 먼저 쓰고, 모자란 만큼 앞에서부터 이어 쓴다
		int32_t firstLen = CAPACITY - mWrite;
		if (len < firstLen)
		{
			firstLen = len;
		}
		MemCpy(mBuffer + mWrite, src, firstLen);
		MemCpy(mBuffer, src + firstLen, len - firstLen);

		mWrite = (mWrite + len) % CAPACITY;
		mUseSize += len;
		return true;
	}

	// 꺼내지 않고 앞에서 len만큼 복사 - 헤더를 먼저 보고 패킷이 다 왔는지 판단할 때 쓴다. 쌓인 양이 부족하면 false
	bool Peek(char* outBuffer, const int32_t len) const
	{
		Assert(outBuffer != nullptr, "RingBuffer::Peek: outBuffer가 nullptr");
		if (mUseSize < len)
		{
			return false;
		}

		int32_t firstLen = CAPACITY - mRead;
		if (len < firstLen)
		{
			firstLen = len;
		}
		MemCpy(outBuffer, mBuffer + mRead, firstLen);
		MemCpy(outBuffer + firstLen, mBuffer, len - firstLen);
		return true;
	}

	// 읽기 위치를 전진 - 송신 완료(보낸 만큼)와 패킷 꺼내기(Peek 후)에서 호출한다
	void Pop(const int32_t len)
	{
		Assert(0 <= len && len <= mUseSize, "RingBuffer: 쓴 양보다 많이 꺼냄");
		mRead = (mRead + len) % CAPACITY;
		mUseSize -= len;
	}

	int32_t GetUseSize() const
	{
		return mUseSize;
	}

	int32_t GetFreeSize() const // 여러 번 Push할 때 전부 들어갈지 먼저 확인용
	{
		return CAPACITY - mUseSize;
	}

	// WSASend/WSARecv에 링 메모리를 직접 넘기기 위한 함수 - 끝에서 감기면 WSABUF 2개가 된다
	// 쌓인 데이터 영역 [읽기 위치–끝], [처음–나머지]. 반환: 버퍼 수(0이면 빈 상태)
	int32_t GetReadBufs(WSABUF* outBufs) const
	{
		Assert(outBufs != nullptr, "RingBuffer::GetReadBufs: outBufs가 nullptr");
		if (mUseSize == 0)
		{
			return 0;
		}

		int32_t firstLen = CAPACITY - mRead;
		if (mUseSize < firstLen)
		{
			firstLen = mUseSize;
		}
		outBufs[0].buf = const_cast<char*>(mBuffer + mRead); // WSABUF가 char*만 받는다 - WSASend는 읽기만 한다
		outBufs[0].len = firstLen;
		if (firstLen == mUseSize)
		{
			return 1;
		}

		outBufs[1].buf = const_cast<char*>(mBuffer);
		outBufs[1].len = mUseSize - firstLen;
		return 2;
	}

	// 빈 공간 영역 [쓰기 위치–끝], [처음–읽기 위치]. 반환: 버퍼 수(0이면 가득 참)
	int32_t GetWriteBufs(WSABUF* outBufs)
	{
		Assert(outBufs != nullptr, "RingBuffer::GetWriteBufs: outBufs가 nullptr");
		const int32_t freeSize = CAPACITY - mUseSize;
		if (freeSize == 0)
		{
			return 0;
		}

		int32_t firstLen = CAPACITY - mWrite;
		if (freeSize < firstLen)
		{
			firstLen = freeSize;
		}
		outBufs[0].buf = mBuffer + mWrite;
		outBufs[0].len = firstLen;
		if (firstLen == freeSize)
		{
			return 1;
		}

		outBufs[1].buf = mBuffer;
		outBufs[1].len = freeSize - firstLen;
		return 2;
	}

	// 링 메모리에 직접 받은 만큼 쓰기 위치를 전진 (GetWriteBufs와 짝) - 수신 완료에서 transferred로 호출한다
	void MoveWrite(const int32_t len)
	{
		Assert(0 <= len && len <= CAPACITY - mUseSize, "RingBuffer: 빈 공간보다 많이 씀");
		mWrite = (mWrite + len) % CAPACITY;
		mUseSize += len;
	}

	// 세션 해제 시 비운다
	void Clear()
	{
		mRead = 0;
		mWrite = 0;
		mUseSize = 0;
	}

private:
	char mBuffer[CAPACITY]{}; 
	int32_t mRead;          // 다음에 읽을 위치
	int32_t mWrite;         // 다음에 쓸 위치
	int32_t mUseSize;       // 쌓인 바이트 수 - 가득 참/빔을 구분하려고 칸 하나를 비워두지 않는다
};

template <int32_t CAPACITY>
inline RingBuffer<CAPACITY>::RingBuffer()
	: mRead(0)
	, mWrite(0)
	, mUseSize(0)
{
}
