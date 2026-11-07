#include <WinSock2.h>
#include <WS2tcpip.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>
#include "NetworkLib.h"
#include "PacketHeader.h"
extern volatile LONG64 gWsaSendCnt;
#pragma pack(push, 1)
struct Msg { uint32_t Thread; uint32_t Seq; };
#pragma pack(pop)
class T final : public NetworkLib
{
public:
	std::atomic<uint64_t> Id{ 0 };
	std::atomic<int> Released{ 0 };
	~T() override { Stop(); }
protected:
	void OnAccept(const uint64_t id) override { Id = id; }
	void OnRecv(const uint64_t id, const char* payload, const int32_t len) override { SendPacket(id, payload, len); }
	void OnRelease(const uint64_t) override { ++Released; }
};
static const int SENDERS = 2, ROUNDS = 200, BURST = 50;
static std::atomic<bool> gClientStop{ false };
static std::atomic<long long> gRecvExt{ 0 }, gRecvEcho{ 0 }, gOrderErr{ 0 };
static uint32_t gNext[4] = {};
static void clientRecv(SOCKET s)
{
	std::vector<char> buf(1 << 20);
	size_t have = 0;
	while (!gClientStop)
	{
		const int n = recv(s, buf.data() + have, static_cast<int>(buf.size() - have), 0);
		if (n <= 0)
		{
			if (n < 0 && WSAGetLastError() == WSAETIMEDOUT) { continue; }
			return;
		}
		have += n;
		size_t pos = 0;
		while (have - pos >= sizeof(PacketHeader) + sizeof(Msg))
		{
			Msg m;
			memcpy(&m, buf.data() + pos + sizeof(PacketHeader), sizeof(m));
			pos += sizeof(PacketHeader) + sizeof(Msg);
			const uint32_t idx = (m.Thread == 99) ? 3 : m.Thread;
			if (idx >= 4 || m.Seq != gNext[idx]) { ++gOrderErr; }
			if (idx < 4) { gNext[idx] = m.Seq + 1; }
			if (m.Thread == 99) { ++gRecvEcho; } else { ++gRecvExt; }
		}
		memmove(buf.data(), buf.data() + pos, have - pos);
		have -= pos;
	}
}
static void sendEcho(SOCKET s, const uint32_t seq)
{
	char p[sizeof(PacketHeader) + sizeof(Msg)];
	PacketHeader h; h.Len = sizeof(Msg);
	Msg m{ 99, seq };
	memcpy(p, &h, sizeof(h)); memcpy(p + sizeof(h), &m, sizeof(m));
	send(s, p, sizeof(p), 0);
}
int main()
{
	SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
	setvbuf(stdout, nullptr, _IONBF, 0);
	std::thread([] { Sleep(20000); printf("WATCHDOG timeout\n"); TerminateProcess(GetCurrentProcess(), 3); }).detach();
	WsadataRAII wsa;
	static T server;
	NetConfig cfg; cfg.Port = 32077; cfg.WorkerCnt = 2; cfg.SlotCnt = 2; cfg.MaxSession = 16;
	if (!server.Start(cfg)) { printf("START_FAIL %s %d\n", server.GetNetError().Step, server.GetNetError().Code); return 2; }
	SOCKET c = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	DWORD to = 100; setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char*>(&to), sizeof(to));
	sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(32077); inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
	if (connect(c, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) { printf("CONNECT_FAIL\n"); return 2; }
	while (server.Id == 0) { Sleep(1); }
	const uint64_t id = server.Id;
	std::thread rx(clientRecv, c);
	Msg first{ 0, 0 };
	std::thread([&] { printf("single_ret=%d\n", server.SendPacket(id, reinterpret_cast<char*>(&first), sizeof(first)) ? 1 : 0); }).join();
	Sleep(300);
	printf("single_arrived=%lld\n", gRecvExt.load());
	sendEcho(c, 0);
	Sleep(300);
	printf("echo_arrived=%lld\n", gRecvEcho.load());
	const LONG64 wsaBefore = gWsaSendCnt;
	std::atomic<long long> ok{ 0 }, fail{ 0 };
	std::vector<std::thread> senders;
	for (int t = 0; t < SENDERS; ++t)
	{
		senders.emplace_back([&, t] {
			uint32_t seq = (t == 0) ? 1 : 0;
			for (int r = 0; r < ROUNDS; ++r)
			{
				for (int i = 0; i < BURST; ++i)
				{
					Msg m{ static_cast<uint32_t>(t), seq };
					if (server.SendPacket(id, reinterpret_cast<char*>(&m), sizeof(m))) { ++ok; ++seq; } else { ++fail; }
				}
				Sleep(1);
			}
		});
	}
	for (uint32_t e = 1; e <= 200; ++e) { sendEcho(c, e); Sleep(1); }
	for (std::thread& s : senders) { s.join(); }
	Sleep(500);
	const LONG64 wsaDelta = gWsaSendCnt - wsaBefore;
	printf("bulk_ok=%lld bulk_fail=%lld recv_ext=%lld recv_echo=%lld order_err=%lld wsasend=%lld\n", ok.load(), fail.load(), gRecvExt.load(), gRecvEcho.load(), gOrderErr.load(), static_cast<long long>(wsaDelta));
	gClientStop = true;
	rx.join();
	closesocket(c);
	for (int i = 0; i < 200 && server.Released == 0; ++i) { Sleep(10); }
	printf("released=%d\n", server.Released.load());
	std::atomic<bool> stopped{ false };
	std::thread st([&] { server.Stop(); stopped = true; });
	for (int i = 0; i < 300 && !stopped; ++i) { Sleep(10); }
	if (!stopped) { printf("STOP_HANG\n"); TerminateProcess(GetCurrentProcess(), 4); }
	st.join();
	printf("stop_ok\n");
	return 0;
}