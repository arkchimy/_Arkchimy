#pragma once

// 프로그램 오류(불변식)를 잡는 Assert - 문제가 있으면 가능한 빨리 멈춘다
// 디버그: 조건이 거짓이면 그 자리에서 디버거로 멈춘다
// 릴리즈: 검사하지 않고 컴파일러에 "항상 참"이라는 힌트만 준다(__assume) - 릴리즈에서 거짓이 될 수 있는 조건(밖에서 오는 값)에는 쓰지 말 것
#ifdef _DEBUG
#define Assert(expr) \
	do \
	{ \
		if (!(expr)) \
		{ \
			__debugbreak(); \
		} \
	} while (false)
#else
#define Assert(expr) __assume(expr)
#endif
