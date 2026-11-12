#pragma once
#include <WinSock2.h>
#include <atomic>
#include <random>
#include <thread>
#include "ClientConfig.h"
#include "ClientSession.h"
#include "ClientStat.h"

// 에코 시험 클라 - 세션 전부를 select 스레드 하나가 돌린다
// Start가 select 스레드를 띄우고, Stop은 종료를 요청한 뒤 스레드가 끝날 때까지 기다린다
class EchoClient
{
public:
	~EchoClient();

	bool Start(const ClientConfig& config); // false: 이미 실행 중이거나 IP 형식 오류
	void Stop(); // 새 묶음을 멈추고, 정상 세션은 진행 중인 묶음의 응답까지 받은 뒤 닫는다 - select 스레드가 끝나면 돌아온다

	const ClientStat& GetStat() const
	{
		return mStat;
	}

private:
	void runSelectLoop(); // select 스레드 본문 - 세션을 64개씩 나눠 select로 돌린다(접속, 보내기, 받기, 비교, 끊기, 재접속)
	void handleEvent(ClientSession& session, const fd_set& readSet, const fd_set& writeSet, const fd_set& exceptSet); // select 알림이 온 세션을 다음 단계로
	void updateIdle(ClientSession& session); // 소켓이 없는 세션 - 종료 요청이 없으면 새로 접속, 있으면 Done
	void updateStallWait(ClientSession& session); // (SendStall) 주기마다 요청 100개를 보내 서버가 끊었는지 본다 - 종료 요청 뒤에도 서버가 끊을 때까지

	void createEchoMessages(ClientSession& session);               // (Normal) 요청 100개를 SendBuf에, 돌아와야 할 응답 100개를 같은 순서로 ExpectBuf에
	void createBadHeader(ClientSession& session);                  // (BadHeader) 길이가 범위 밖인 헤더 하나 - 접속마다 4097과 65535를 번갈아
	void startConnect(ClientSession& session);                     // 논블로킹 connect를 건다 → Connecting
	void onConnected(ClientSession& session);                      // 접속 성공 - 역할에 따라 첫 송신을 준비한다
	void onSendable(ClientSession& session);                       // SendBuf를 보낼 수 있는 만큼 보낸다
	void onReceivable(ClientSession& session);                     // (Normal) 응답을 받아 ExpectBuf와 비교한다
	void onBatchDone(ClientSession& session);                      // (Normal) 묶음의 응답을 다 받았다 - 다음 묶음 또는 끊기
	void onWaitCloseReadable(ClientSession& session);              // (BadHeader) 서버가 끊었는지 확인한다
	void closeConnected(ClientSession& session, const bool bRst);  // 연결된 소켓을 닫고 연결 수를 내린다 - 종료 요청이면 Done, 아니면 Idle

	ClientConfig mConfig;
	SOCKADDR_IN mServerAddr{};
	ClientStat mStat;
	std::atomic<bool> mbStopRequested = false;
	std::mt19937 mRng{ 12345 }; // 수명 무작위 - 시드를 고정해 같은 설정이면 같은 순서가 나온다
	std::thread mSelectThread;
};
