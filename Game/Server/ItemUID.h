#pragma once
#include <cstdint>

// 아이템 하나하나의 고유 번호 - 계정 생성(시작 아이템)과 던전(조사 보상·전리품)이 함께 쓴다. 여러 스레드에서 불러도 된다
// 0은 "비어 있음"(장비 칸)이라 1부터. 서버를 다시 켜도 겹치지 않게 시작할 때 DB의 가장 큰 번호 다음부터 센다
uint64_t NewItemUID();
void SetLastItemUID(const uint64_t lastItemUID); // 시작할 때 한 번 - 스레드를 띄우기 전에
