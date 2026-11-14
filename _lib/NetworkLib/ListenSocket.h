#pragma once
#include <WinSock2.h>
#include <cstdint>

// socket -> bind -> listen까지 공통 처리. 실패 시 INVALID_SOCKET - 실패한 단계(문자열 상수)와 에러 코드를 out에 적는다
// bLoopbackOnly면 127.0.0.1에만 묶는다(같은 PC에서만 접속)
SOCKET CreateListenSocket(const USHORT port, const bool bLoopbackOnly, const int backlog, const char** outStep, int32_t* outErr);
