#pragma once
#include <WinSock2.h>
#include <atomic>
#include <thread>
#include <vector>
#include "ClientConfig.h"
#include "ClientSession.h"
#include "ClientStat.h"

// AES 에코 시험 클라 - Echo 클라와 같은 select 구조. 세션을 ThreadCnt개 select 스레드가 나눠 돌린다
// 접속 → ClientHello / ServerHello(서버 고정 공개 키로 확인) → [첫 접속만] 가입 → 로그인(여기까지 RSA, 응답에 세션 키)
// → 에코 묶음 반복(AES-256-GCM) → 수명이 다하면 끊고 다시. RSA 에코 클라와 다른 것은 에코를 잠그는 방법뿐
class AesEchoClient
{
public:
	~AesEchoClient();

	bool Start(const ClientConfig& config, const uint8_t (&serverSignKey)[RSA_MODULUS_LEN]); // false: 이미 실행 중이거나 IP 형식 오류
	void Stop(); // 새 묶음을 멈추고 진행 중인 응답까지 받은 뒤 닫는다 - select 스레드가 모두 끝나면 돌아온다

	void TogglePause(); // 일시정지면 에코 묶음 사이에서 멈추고 새 접속도 하지 않는다

	bool IsPaused() const
	{
		return mbPaused;
	}

	const ClientStat& GetStat() const
	{
		return mStat;
	}

private:
	void runSelectLoop(const int32_t begin, const int32_t end); // select 스레드 본문 - [begin, end) 세션을 64개씩 나눠 select
	void handleEvent(ClientSession& session, const fd_set& readSet, const fd_set& writeSet, const fd_set& exceptSet);
	void updateIdle(ClientSession& session);   // 소켓이 없는 세션 - 종료 요청이면 Done, 일시정지면 대기, 아니면 새로 접속
	void updatePaused(ClientSession& session); // 일시정지가 풀리거나 종료 요청이면 다음 묶음으로

	void startConnect(ClientSession& session);
	void onConnected(ClientSession& session);   // 클라 임시 키 쌍을 만들고 ClientHello를 보낼 준비
	void onSendable(ClientSession& session);
	void onReceivable(ClientSession& session);  // 받은 바이트에서 완성된 패킷을 꺼내 onPacket으로
	bool onPacket(ClientSession& session, const char* payload, const int32_t len); // false = 세션을 닫았다
	bool onServerHello(ClientSession& session, const char* payload, const int32_t len);
	bool onRegisterRes(ClientSession& session, const std::string& plain);
	bool onLoginRes(ClientSession& session, const std::string& plain);
	bool onEchoRes(ClientSession& session, const std::string& plain);
	void onPausedReadable(ClientSession& session);

	bool sendRegister(ClientSession& session);
	bool sendLogin(ClientSession& session);
	void startNextBatch(ClientSession& session); // 종료 요청이면 닫기, 일시정지면 Paused, 수명이 다했으면 끊기, 아니면 에코 묶음
	void queuePacket(ClientSession& session, const void* payload, const int32_t len); // 헤더를 붙여 SendBuf 뒤에
	bool sealAndQueue(ClientSession& session, const void* plain, const size_t len);   // 잠가서 queuePacket
	void failProtocol(ClientSession& session, const char* reason);                     // 서명·응답이 틀림 - 남기고 RST로 닫는다
	void closeConnected(ClientSession& session, const bool bRst);

	ClientConfig mConfig;
	SOCKADDR_IN mServerAddr{};
	uint8_t mServerSignKey[RSA_MODULUS_LEN] = {}; // 박아 둔 서버 고정 공개 키
	ClientStat mStat;
	std::vector<ClientSession> mSessions;
	std::atomic<bool> mbStopRequested = false;
	std::atomic<bool> mbPaused = false; // main만 바꾸고 select 스레드가 읽는다
	std::vector<std::thread> mSelectThreads;
};
