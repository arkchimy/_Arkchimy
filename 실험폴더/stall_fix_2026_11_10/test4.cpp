#include <WinSock2.h>
#include <WS2tcpip.h>
#include <atomic>
#include <cstdio>
#include <thread>
#include "NetworkLib.h"
class T final : public NetworkLib
{
public:
	std::atomic<uint64_t> Id{ 0 };
	std::atomic<int> Released{ 0 };
	~T() override { Stop(); }
protected:
	void OnAccept(const uint64_t id) override { Id = id; }
	void OnRecv(const uint64_t, const char*, const int32_t) override {}
	void OnRelease(const uint64_t) override { ++Released; }
};
int main()
{
	SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
	setvbuf(stdout, nullptr, _IONBF, 0);
	std::thread([] { Sleep(20000); printf("WATCHDOG\n"); TerminateProcess(GetCurrentProcess(), 3); }).detach();
	WsadataRAII wsa;
	static T server;
	NetConfig cfg; cfg.Port = 32079; cfg.WorkerCnt = 2; cfg.SlotCnt = 1; cfg.MaxSession = 4; cfg.SendStallSec = 1; cfg.bSendBufZero = true;
	if (!server.Start(cfg)) { printf("START_FAIL\n"); return 2; }
	SOCKET c = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	int rcv = 1024; setsockopt(c, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<char*>(&rcv), sizeof(rcv)); // 받지 않는 클라 - 곧 수신 윈도우가 0이 된다
	sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(32079); inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
	if (connect(c, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) { printf("CONNECT_FAIL\n"); return 2; }
	while (server.Id == 0) { Sleep(1); }
	const uint64_t id = server.Id;
	static char big[4000] = {};
	int ok = 0;
	std::thread([&] { for (int i = 0; i < 6; ++i) { if (server.SendPacket(id, big, sizeof(big))) { ++ok; } } }).join(); // 워커 밖 스레드가 24KB를 맡긴다 (링 32KB 안)
	for (int i = 0; i < 40 && server.Released == 0; ++i) { Sleep(100); }
	printf("send_ok=%d stall=%lld overflow=%lld released=%d\n", ok, static_cast<long long>(server.GetStat().SendStallCnt), static_cast<long long>(server.GetStat().OverflowCnt), server.Released.load());
	closesocket(c);
	server.Stop();
	printf("stop_ok\n");
	return 0;
}