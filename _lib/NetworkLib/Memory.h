#pragma once
#include <cstddef>
#include <cstring>
#include <memory>
#include <utility>
#include "Assert.h"

// 메모리 복사·초기화와 할당을 한 곳으로 모은다 - 할당자를 바꾸거나 검사를 넣을 때 여기만 고친다
// memcpy/memset/memmove/ZeroMemory, new/delete, std::make_unique는 직접 쓰지 않는다(CODING_STANDARD.md)

inline void MemCpy(void* dest, const void* src, const size_t size)
{
	Assert(size == 0 || (dest != nullptr && src != nullptr), "MemCpy: 대상 또는 원본이 nullptr");
	memcpy(dest, src, size);
}

inline void MemSet(void* dest, const int value, const size_t size)
{
	Assert(size == 0 || dest != nullptr, "MemSet: 대상이 nullptr");
	memset(dest, value, size);
}

inline void MemMove(void* dest, const void* src, const size_t size)
{
	Assert(size == 0 || (dest != nullptr && src != nullptr), "MemMove: 대상 또는 원본이 nullptr");
	memmove(dest, src, size);
}

template <typename T, typename... Args>
T* New(Args&&... args)
{
	return new T(std::forward<Args>(args)...);
}

template <typename T>
void Delete(T* ptr)
{
	delete ptr;
}

// 원소를 기본 생성한다 - char 같은 기본 타입은 값을 채우지 않는다(new T[count]와 같음)
template <typename T>
T* NewArray(const size_t count)
{
	return new T[count];
}

template <typename T>
void DeleteArray(T* ptr)
{
	delete[] ptr;
}

// std::unique_ptr이 New로 만든 것을 Delete로 지우게 하는 삭제자 - 함수가 있으므로 구조체가 아니라 클래스
class Deleter
{
public:
	template <typename T>
	void operator()(T* ptr) const
	{
		Delete(ptr);
	}
};

class ArrayDeleter
{
public:
	template <typename T>
	void operator()(T* ptr) const
	{
		DeleteArray(ptr);
	}
};

template <typename T>
using UniquePtr = std::unique_ptr<T, Deleter>;

template <typename T>
using UniqueArray = std::unique_ptr<T[], ArrayDeleter>;

template <typename T, typename... Args>
UniquePtr<T> MakeUnique(Args&&... args)
{
	return UniquePtr<T>(New<T>(std::forward<Args>(args)...));
}

template <typename T>
UniqueArray<T> MakeUniqueArray(const size_t count)
{
	return UniqueArray<T>(NewArray<T>(count));
}
