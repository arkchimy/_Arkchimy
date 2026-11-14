#pragma once
#include <cstdint>

// 아이템 하나하나의 고유 번호 - 계정 생성(시작 아이템)과 던전(조사 보상·전리품)이 함께 쓴다. 여러 스레드에서 불러도 된다
// 0은 "비어 있음"(장비 칸)이라 1부터. DB를 붙이면 DB에서 받는다
uint64_t NewItemUID();
