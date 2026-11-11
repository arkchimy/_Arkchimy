#pragma once
#include <cstdio>

// 프로그램 오류(불변식)를 잡는 Assert - 문제가 있으면 가능한 빨리 멈춘다
// 디버그와 릴리즈 모두 검사한다. 거짓이면 파일·줄·조건·설명을 stderr에 남기고 그 자리에서 멈춘다(덤프 핸들러가 잡을 수 있게 __debugbreak)
// message는 문자열 상수 - 무엇이 깨졌는지 한 줄로 적는다

// 마지막으로 실패한 Assert의 설명 - 덤프를 열고 이 변수를 보면 무엇이 깨졌는지 안다(volatile이라 최적화로 지워지지 않는다)
// 헤더 안의 inline 변수라 DLL과 서버 exe가 각자 하나씩 가진다 - 멈춘 쪽 모듈의 값을 본다
inline const char* volatile gAssertMessage = nullptr;

#define Assert(expr, message) \
	do \
	{ \
		if (!(expr)) \
		{ \
			gAssertMessage = (message); \
			fprintf(stderr, "[Assert] %s(%d): %s - %s\n", __FILE__, __LINE__, #expr, (message)); \
			__debugbreak(); \
		} \
	} while (false)
