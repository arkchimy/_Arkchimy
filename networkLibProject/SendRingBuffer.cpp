#include "SendRingBuffer.h"
#include <cstring>

// 남은 공간이 부족하면 아무것도 넣지 않고 false - 호출한 쪽이 세션을 끊는다
bool SendRingBuffer::push(const char* src, int32_t len)
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
	memcpy(mBuffer + mWrite, src, firstLen);
	memcpy(mBuffer, src + firstLen, len - firstLen);

	mWrite = (mWrite + len) % CAPACITY;
	mUseSize += len;
	return true;
}

// 쌓인 데이터를 WSABUF 최대 2개로 채운다 - 끝에서 감겨 있으면 [읽기 위치~끝], [처음~나머지]
int32_t SendRingBuffer::getSendBufs(WSABUF* bufs)
{
	if (mUseSize == 0)
	{
		return 0;
	}

	int32_t firstLen = CAPACITY - mRead;
	if (mUseSize < firstLen)
	{
		firstLen = mUseSize;
	}
	bufs[0].buf = mBuffer + mRead;
	bufs[0].len = firstLen;
	if (firstLen == mUseSize)
	{
		return 1;
	}

	bufs[1].buf = mBuffer;
	bufs[1].len = mUseSize - firstLen;
	return 2;
}

// 보낸 만큼 읽기 위치를 전진 - 송신 완료에서 transferred로 호출한다
void SendRingBuffer::pop(int32_t len)
{
	mRead = (mRead + len) % CAPACITY;
	mUseSize -= len;
}

// 세션 해제 시 비운다
void SendRingBuffer::clear()
{
	mRead = 0;
	mWrite = 0;
	mUseSize = 0;
}
