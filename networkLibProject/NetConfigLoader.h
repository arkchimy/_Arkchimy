#pragma once
#include "NetworkLib.h"
#include "ParserLib.h"

// cfg 값을 라이브러리 설정으로 옮긴다 - 라이브러리가 파일 형식을 모르게 서버 쪽에 둔다. 서버 exe들이 함께 쓴다
// cfg에 없는 선택 키는 NetConfig의 기본값을 쓴다. false: 필수 키(port, backlog, workerCnt, maxSession) 누락 또는 값 오류
bool LoadNetConfig(const utility::Parser& parser, NetConfig* outConfig);
