#include "RingBuffer.h"
#include <cstring>

// 남은 공간이 부족하면 아무것도 넣지 않고 false - 호출한 쪽이 세션을 끊는다
bool RingBuffer::push(const char* src, int32_t len)
{
	if (mCapacity - mUseSize < len)
	{
		return false;
	}

	// 버퍼 끝까지 남은 칸에 먼저 쓰고, 모자란 만큼 앞에서부터 이어 쓴다
	int32_t firstLen = mCapacity - mWrite;
	if (len < firstLen)
	{
		firstLen = len;
	}
	memcpy(mBuffer + mWrite, src, firstLen);
	memcpy(mBuffer, src + firstLen, len - firstLen);

	mWrite = (mWrite + len) % mCapacity;
	mUseSize += len;
	return true;
}

// 헤더를 먼저 보고 패킷이 다 왔는지 판단할 때 쓴다 - 읽기 위치는 그대로
bool RingBuffer::peek(char* dst, int32_t len) const
{
	if (mUseSize < len)
	{
		return false;
	}

	int32_t firstLen = mCapacity - mRead;
	if (len < firstLen)
	{
		firstLen = len;
	}
	memcpy(dst, mBuffer + mRead, firstLen);
	memcpy(dst + firstLen, mBuffer, len - firstLen);
	return true;
}

// 읽기 위치를 전진 - 송신 완료(보낸 만큼)와 패킷 꺼내기(peek 후)에서 호출한다
void RingBuffer::pop(int32_t len)
{
	mRead = (mRead + len) % mCapacity;
	mUseSize -= len;
}

// 쌓인 데이터를 WSABUF 최대 2개로 채운다 - 끝에서 감겨 있으면 [읽기 위치~끝], [처음~나머지]
int32_t RingBuffer::getReadBufs(WSABUF* bufs) const
{
	if (mUseSize == 0)
	{
		return 0;
	}

	int32_t firstLen = mCapacity - mRead;
	if (mUseSize < firstLen)
	{
		firstLen = mUseSize;
	}
	bufs[0].buf = const_cast<char*>(mBuffer + mRead);	// WSABUF가 char*만 받는다 - WSASend는 읽기만 한다
	bufs[0].len = firstLen;
	if (firstLen == mUseSize)
	{
		return 1;
	}

	bufs[1].buf = const_cast<char*>(mBuffer);
	bufs[1].len = mUseSize - firstLen;
	return 2;
}

// 빈 공간을 WSABUF 최대 2개로 채운다 - 끝에서 감겨 있으면 [쓰기 위치~끝], [처음~읽기 위치]
int32_t RingBuffer::getWriteBufs(WSABUF* bufs)
{
	int32_t freeSize = mCapacity - mUseSize;
	if (freeSize == 0)
	{
		return 0;
	}

	int32_t firstLen = mCapacity - mWrite;
	if (freeSize < firstLen)
	{
		firstLen = freeSize;
	}
	bufs[0].buf = mBuffer + mWrite;
	bufs[0].len = firstLen;
	if (firstLen == freeSize)
	{
		return 1;
	}

	bufs[1].buf = mBuffer;
	bufs[1].len = freeSize - firstLen;
	return 2;
}

// 수신 완료에서 transferred로 호출한다
void RingBuffer::moveWrite(int32_t len)
{
	mWrite = (mWrite + len) % mCapacity;
	mUseSize += len;
}

// 세션 해제 시 비운다
void RingBuffer::clear()
{
	mRead = 0;
	mWrite = 0;
	mUseSize = 0;
}

// 송신 넘침 한도를 설정으로 바꿀 때 서버 시작 전에 호출한다
void RingBuffer::reset(int32_t capacity)
{
	delete[] mBuffer;
	mBuffer = new char[capacity];
	mCapacity = capacity;
	clear();
}
