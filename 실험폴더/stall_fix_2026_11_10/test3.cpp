#include <WinSock2.h>
#include <WS2tcpip.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>
#include "NetworkLib.h"
#include "PacketHeader.h"
class T final : public NetworkLib
{
public:
	std::atomic<uint64_t> Id{ 0 };
	std::atomic<int> Released{ 0 };
	~T() override { Stop(); }
protected:
	void OnAccept(const uint64_t id) override { Id = id; }
	void OnRecv(const uint64_t, const char* payload, const int32_t len) override
	{
		if (len > 0 && payload[0] == 'S') { Sleep(2500); } // 워커 하나를 붙잡아 둔다
	}
	void OnRelease(const uint64_t) override { ++Released; }
};
static std::atomic<long long> gRecvBytes{ 0 };
static std::atomic<bool> gClientClosed{ false };
int main()
{
	SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
	setvbuf(stdout, nullptr, _IONBF, 0);
	std::thread([] { Sleep(20000); printf("WATCHDOG\n"); TerminateProcess(GetCurrentProcess(), 3); }).detach();
	WsadataRAII wsa;
	static T server;
	NetConfig cfg; cfg.Port = 32078; cfg.WorkerCnt = 1; cfg.SlotCnt = 1; cfg.MaxSession = 4; cfg.SendStallSec = 1;
	if (!server.Start(cfg)) { printf("START_FAIL %s %d\n", server.GetNetError().Step, server.GetNetError().Code); return 2; }
	SOCKET c = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(32078); inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
	if (connect(c, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) { printf("CONNECT_FAIL\n"); return 2; }
	while (server.Id == 0) { Sleep(1); }
	const uint64_t id = server.Id;
	std::thread rx([c] {
		char buf[4096];
		while (true) { const int n = recv(c, buf, sizeof(buf), 0); if (n <= 0) { gClientClosed = true; return; } gRecvBytes += n; }
	});
	char p[3]; PacketHeader h; h.Len = 1; memcpy(p, &h, 2); p[2] = 'S';
	send(c, p, 3, 0);  // 워커가 2.5초 잠든다
	Sleep(200);
	const char msg[] = "hello";
	bool bRet = false;
	std::thread([&] { bRet = server.SendPacket(id, msg, sizeof(msg)); }).join(); // 워커 밖 스레드가 맡긴다 - 큐에서 약 2.3초 기다린다
	Sleep(3500);
	printf("send_ret=%d stall=%lld recv_bytes=%lld client_closed=%d\n", bRet ? 1 : 0, static_cast<long long>(server.GetStat().SendStallCnt), gRecvBytes.load(), gClientClosed.load() ? 1 : 0);
	closesocket(c);
	rx.join();
	for (int i = 0; i < 200 && server.Released == 0; ++i) { Sleep(10); }
	server.Stop();
	printf("released=%d stop_ok\n", server.Released.load());
	return 0;
}