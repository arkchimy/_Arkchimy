#pragma once
#include <WinSock2.h>
#include <cstdint>

// 세션 버퍼 크기 - 상수
enum : int32_t
{
	RECV_RING_SIZE = 16384, // 수신 링 - 최대 패킷보다 커야 한다(static_assert). 완성된 패킷을 매번 다 꺼내므로 가득 차지 않는다
	SEND_RING_SIZE = 32768, // 송신 링 크기 = 송신 넘침 한도. 가득 차면 끊는다 - 받는 속도가 느린 클라, 또는 주변 세션이 너무 많이 보낼 때
};

// 라이브러리 설정 - Start에 넘긴다. 기본값은 여기 한곳에 둔다
struct NetConfig
{
	USHORT Port = 0;
	bool bLoopbackOnly = false; // true면 127.0.0.1에만 묶는다 - 같은 PC에서만 접속(운영자 포트처럼 밖에 열면 안 되는 포트)
	int Backlog = 512;
	int WorkerCnt = 2;     // IOCP 워커 수
	int IocpConcurrency = 0; // IOCP가 동시에 깨워 일하게 할 스레드 수(CreateIoCompletionPort 마지막 인자). 0이면 코어 수, 음수는 Start가 0으로
	int SlotCnt = 2;       // 걸어두는 AcceptEx 수
	int MaxSession = 1000; // 최대 동시 접속

	// 워커가 완료를 꺼내는 방식. true면 GQCSEx로 최대 GqcsBatch개씩, false면 GQCS로 1개씩
	// GQCS는 기본값을 정할 때 비교한 대상이라 같은 코드로 다시 잴 수 있게 남겨 둔다.
	bool bGqcsEx = true;
	int GqcsBatch = 64; // 크면 호출 수는 줄지만 한 워커가 몰아 처리하는 동안 다른 워커가 놀 수 있다. 1보다 작으면 Start가 1로 올린다

	// 완료 통지 skip 방식 (FILE_SKIP_COMPLETION_PORT_ON_SUCCESS)
	// 켜면 WSASend/WSARecv가 즉시 성공했을 때 완료 통지를 큐에 넣지 않고 건 스레드가 그 자리에서 처리한다. IFS가 아닌 LSP가 있으면 Start가 끈다
	bool bSkipOnSuccess = true;

	// 송신 합치기 범위 - 모아 보내기는 항상 켜져 있고, 이 설정은 "어디까지 모았다가 보내나"를 정한다
	// false: 수신 완료 하나를 처리할 때마다 보낸다.
	// true: 워커가 한 번에 꺼낸 완료(GQCSEx면 최대 GqcsBatch개)를 다 처리한 뒤 보낸다 
	bool bMergeSends = true;

	// 송신 정체 판정 기준(초). 0이면 끔(음수는 Start가 0으로). 켜면 감시 스레드가 1초마다 세션을 돌며, 
	// WSASend가 이 시간 넘게 안 끝난 세션을 끊는다
	int SendStallSec = 0;

	// 송신 버퍼 0 실험. 켜면 수락한 소켓마다 SO_SNDBUF = 0 (커널이 송신 버퍼로 복사하지 않고 송신이 비동기로 넘어간다)
	bool bSendBufZero = false;

	// [테스트 전용] 송신 완료를 처리하기 전에 ms만큼 잠든다 - 완료 처리 지연을 일부러 만들어 넘침 판정을 결정적으로 시험한다. 0이면 끔
	int TestSendCompleteDelayMs = 0;

	// 즉시 완료 반복 상한(완료 통지 skip을 켰을 때만) - 한 번의 postRecv·postSend 안에서 WSARecv·WSASend가 연달아 즉시 끝난 횟수가
	// 이 값에 닿으면 다시 걸지 않고 다음 차례를 PQCS로 IOCP 큐 뒤에 넣는다(한 세션이 워커를 붙잡지 않게). 0이면 상한 없음, 음수는 Start가 0으로
	// 값 자체에 정확한 근거는 없다 - 반복이 끝없이 이어지지 않게 하는 상한이다(설계 portfolio/design/즉시완료_반복_상한_설계.docx)
	int SkipLoopLimit = 3;

	// 수락한 소켓마다 TCP_NODELAY(Nagle 알고리즘 끔). false면 OS 기본(Nagle 켬) - 보낸 작은 데이터의 ACK가 오기 전에는 다음 작은 송신을 붙잡아 둔다
	bool bTcpNoDelay = false;
};

// 라이브러리 오류 - 라이브러리는 출력하지 않는다. 
// 실패한 단계와 코드만 남기고 출력은 쓰는 쪽이 정한다
struct NetError
{
	const char* Step = nullptr; // 실패한 단계("bind", "GQCS" 등 문자열 상수). nullptr이면 오류 없음
	int32_t Code = 0;           // WSAGetLastError 또는 GetLastError 값. 코드가 없는 오류면 0
};

// cfg 파일(키 = 값;)을 읽어 outConfig를 채운다 - Start 전에 부르고, 읽은 뒤 값을 고쳐서 넘겨도 된다
// 필수 키: port, workerCnt, maxSession. 나머지 키가 없으면 NetConfig의 기본값을 그대로 둔다(bool 설정은 cfg에 0/1로 적는다)
// false: 파일이 없거나 형식 오류, 필수 키가 없거나 숫자가 아님, workerCnt·maxSession·slotCnt가 1보다 작음 - outError에 실패한 단계(키 이름)를 남긴다
bool LoadNetConfig(const char* path, NetConfig* outConfig, NetError* outError);
