// WSASend 비용 측정 - WSABUF 개수(bufCnt)와 길이(len)에 따라 호출 1번이 얼마나 걸리는지
// 루프백 TCP 연결 하나, 받는 쪽 스레드는 recv로 비우기만 한다. 한 번에 송신 1개만 걸어 둔다(라이브러리와 같은 조건)
//
// 모드
//   sync     : SO_SNDBUF 기본값 - 커널이 송신 버퍼로 복사하고 WSASend가 0(즉시 성공)을 돌려준다.
//              FILE_SKIP_COMPLETION_PORT_ON_SUCCESS라 완료 통지도 없다. 측정값 = WSASend 반환까지
//   zerocopy : SO_SNDBUF = 0 - 커널이 사용자 버퍼를 직접 쓰므로 복사가 없고 WSASend는 대기(WSA_IO_PENDING)로 넘어간다.
//              측정값 = WSASend 반환까지 + 완료를 GQCS로 받기까지
// 받는 쪽이 늦어 sync가 대기로 넘어가면 완료를 기다리고, 그 비율을 pendingPct로 남긴다
//
// 출력: CSV 한 줄 = 한 측정점 (round, mode, set, totalBytes, bufCnt, perBuf, iters, callMedNs, callP99Ns, totalMedNs, totalP99Ns, pendingPct, MBps)
// 사용: wsasend_bench.exe [rounds] [iters] > 결과.csv
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>
#pragma comment(lib, "ws2_32.lib")

namespace
{
	double gNsPerTick = 0.0;

	int64_t qpcNow()
	{
		LARGE_INTEGER t;
		QueryPerformanceCounter(&t);
		return t.QuadPart;
	}

	void fail(const char* what)
	{
		fprintf(stderr, "%s 실패 err=%d\n", what, WSAGetLastError());
		exit(1);
	}

	struct Conn
	{
		SOCKET sender = INVALID_SOCKET;
		SOCKET receiver = INVALID_SOCKET;
		HANDLE iocp = nullptr;
		std::thread drainer;
	};

	// 루프백 연결을 만들고 받는 쪽은 별도 스레드에서 계속 비운다
	void openConn(Conn& c, bool zeroCopy)
	{
		SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		addr.sin_port = 0;	// 빈 포트
		if (bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) fail("bind");
		if (listen(listener, 1) != 0) fail("listen");
		int len = sizeof(addr);
		getsockname(listener, reinterpret_cast<sockaddr*>(&addr), &len);

		c.sender = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
		if (c.sender == INVALID_SOCKET) fail("WSASocket");
		if (connect(c.sender, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) fail("connect");
		c.receiver = accept(listener, nullptr, nullptr);
		if (c.receiver == INVALID_SOCKET) fail("accept");
		closesocket(listener);

		BOOL noDelay = TRUE;	// 작은 송신이 Nagle로 묶여 완료가 늦어지는 변수를 없앤다
		setsockopt(c.sender, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
		if (zeroCopy)
		{
			int zero = 0;
			if (setsockopt(c.sender, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&zero), sizeof(zero)) != 0) fail("SO_SNDBUF");
		}
		c.iocp = CreateIoCompletionPort(reinterpret_cast<HANDLE>(c.sender), nullptr, 1, 1);
		if (c.iocp == nullptr) fail("CreateIoCompletionPort");
		if (!SetFileCompletionNotificationModes(reinterpret_cast<HANDLE>(c.sender), FILE_SKIP_COMPLETION_PORT_ON_SUCCESS)) fail("skip");

		SOCKET r = c.receiver;
		c.drainer = std::thread([r]()
		{
			std::vector<char> buf(1 << 20);
			while (recv(r, buf.data(), static_cast<int>(buf.size()), 0) > 0) {}
		});
	}

	void closeConn(Conn& c)
	{
		shutdown(c.sender, SD_SEND);	// 받는 쪽 recv가 0을 받고 끝난다
		c.drainer.join();
		closesocket(c.sender);
		closesocket(c.receiver);
		CloseHandle(c.iocp);
	}

	struct Point
	{
		const char* set;	// A: 전체 길이 고정·개수 변화, B: 개수 1·길이 변화, C: 34B × n개 vs 34n B × 1개, D: 페이지 배치(아래 main)
		int totalBytes;
		int bufCnt;
		int stride = 0;	// 조각 시작 사이 간격. 0이면 조각 크기를 64바이트로 올림
		int offset = 0;	// 첫 조각의 페이지 안 시작 위치 - 데이터 영역은 페이지 경계에서 시작한다
	};

	struct Result
	{
		double callMed, callP99, totalMed, totalP99, pendingPct, mbps;
	};

	double pct(std::vector<int64_t>& v, double p)
	{
		size_t idx = static_cast<size_t>(p * (v.size() - 1));
		std::nth_element(v.begin(), v.begin() + idx, v.end());
		return v[idx] * gNsPerTick;
	}

	// 기본은 버퍼마다 64바이트 경계로 떨어뜨려 놓는다 - 메시지마다 따로 할당된 버퍼처럼(한 덩어리의 이웃 조각이 아니게)
	Result runPoint(Conn& c, const Point& pt, int iters, char* data)
	{
		std::vector<WSABUF> bufs(pt.bufCnt);
		int per = pt.totalBytes / pt.bufCnt;
		int stride = (pt.stride > 0) ? pt.stride : (per + 63) / 64 * 64;
		for (int i = 0; i < pt.bufCnt; ++i)
		{
			bufs[i].buf = data + pt.offset + static_cast<size_t>(i) * stride;
			bufs[i].len = (i == pt.bufCnt - 1) ? pt.totalBytes - per * (pt.bufCnt - 1) : per;
		}

		std::vector<int64_t> callTicks(iters);
		std::vector<int64_t> totalTicks(iters);
		int pending = 0;
		int warmup = iters / 10;
		int64_t start = 0;
		for (int i = -warmup; i < iters; ++i)
		{
			if (i == 0)
			{
				start = qpcNow();
				pending = 0;
			}
			OVERLAPPED ov{};
			DWORD sent = 0;
			int64_t t0 = qpcNow();
			int ret = WSASend(c.sender, bufs.data(), pt.bufCnt, &sent, 0, &ov, nullptr);
			int64_t t1 = qpcNow();
			int64_t t2 = t1;
			if (ret != 0)
			{
				if (WSAGetLastError() != WSA_IO_PENDING) fail("WSASend");
				++pending;
				DWORD bytes = 0;
				ULONG_PTR key = 0;
				OVERLAPPED* done = nullptr;
				if (!GetQueuedCompletionStatus(c.iocp, &bytes, &key, &done, INFINITE)) fail("GQCS");
				t2 = qpcNow();
			}
			if (i >= 0)
			{
				callTicks[i] = t1 - t0;
				totalTicks[i] = t2 - t0;
			}
		}
		double sec = (qpcNow() - start) * gNsPerTick / 1e9;

		Result r{};
		r.callMed = pct(callTicks, 0.5);
		r.callP99 = pct(callTicks, 0.99);
		r.totalMed = pct(totalTicks, 0.5);
		r.totalP99 = pct(totalTicks, 0.99);
		r.pendingPct = 100.0 * pending / iters;
		r.mbps = static_cast<double>(pt.totalBytes) * iters / sec / 1e6;
		return r;
	}
}

int main(int argc, char** argv)
{
	int rounds = (argc > 1) ? atoi(argv[1]) : 3;
	int iters = (argc > 2) ? atoi(argv[2]) : 10000;

	LARGE_INTEGER freq;
	QueryPerformanceFrequency(&freq);
	gNsPerTick = 1e9 / static_cast<double>(freq.QuadPart);

	WSADATA wsa;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 1;

	// 세 번째 인자 d: 페이지 배치 시험(D)만. 없으면 A-C
	bool setD = (argc > 3) && argv[3][0] == 'd';
	std::vector<Point> points;
	std::vector<Point> lockPoints;	// D3 - VirtualLock 안 걸고 / 걸고 같은 점을 두 번
	if (!setD)
	{
		for (int n : { 1, 2, 4, 8, 16, 32, 64, 128, 256, 512 }) points.push_back({ "A", 4096, n });
		for (int len : { 32, 128, 512, 2048, 8192, 32768, 65536 }) points.push_back({ "B", len, 1 });
		for (int n : { 1, 4, 16, 64, 150, 256, 512 })
		{
			points.push_back({ "C-many", 34 * n, n });	// 34B 메시지 n개를 WSABUF n개로
			points.push_back({ "C-one", 34 * n, 1 });	// 같은 바이트를 이어 붙여 WSABUF 1개로(링 방식)
		}
	}
	else
	{
		// D1: 조각 수·바이트는 같게, 조각을 한 페이지 안에 몰기(간격 64B) vs 조각마다 다른 페이지(간격 4096B)
		for (int n : { 16, 64 })
		{
			points.push_back({ "D1-same", 34 * n, n, 64, 0 });
			points.push_back({ "D1-diff", 34 * n, n, 4096, 0 });
		}
		// D2: 조각 1개, 바이트는 같게, 페이지 경계에 안 걸치기 vs 걸치기 (걸치면 페이지가 1개 늘어남)
		points.push_back({ "D2-in", 64, 1, 0, 0 });	// 1페이지
		points.push_back({ "D2-cross", 64, 1, 0, 4064 });	// 2페이지
		points.push_back({ "D2-in", 4096, 1, 0, 0 });	// 1페이지
		points.push_back({ "D2-cross", 4096, 1, 0, 64 });	// 2페이지
		points.push_back({ "D2-in", 8192, 1, 0, 0 });	// 2페이지
		points.push_back({ "D2-cross", 8192, 1, 0, 64 });	// 3페이지
		// D3: 같은 점을 VirtualLock 없이 / 걸고 - 프로세스 차원의 잠금이 I/O마다의 잠금을 대신하는지
		lockPoints.push_back({ "D3", 4096, 1, 0, 0 });
		lockPoints.push_back({ "D3", 34 * 64, 64, 64, 0 });
		lockPoints.push_back({ "D3", 34 * 64, 64, 4096, 0 });
	}

	// 페이지 경계에서 시작하는 데이터 영역 - D 시험이 페이지 안 위치를 정확히 맞춘다. 미리 써서 페이지를 올려 둔다
	const size_t dataSize = 16 << 20;
	char* data = static_cast<char*>(VirtualAlloc(nullptr, dataSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
	if (data == nullptr) fail("VirtualAlloc");
	memset(data, 'x', dataSize);
	const size_t lockSize = 65 * 4096;	// D3이 쓰는 범위: 64개 × 4096 간격
	if (setD && !SetProcessWorkingSetSize(GetCurrentProcess(), 64 << 20, 256 << 20)) fail("SetProcessWorkingSetSize");	// VirtualLock 한도 = 최소 작업 집합

	printf("round,mode,set,totalBytes,bufCnt,perBuf,stride,offset,locked,iters,callMedNs,callP99Ns,totalMedNs,totalP99Ns,pendingPct,MBps\n");
	auto report = [&](int round, bool zeroCopy, const Point& pt, bool locked, const Result& r)
	{
		printf("%d,%s,%s,%d,%d,%d,%d,%d,%d,%d,%.0f,%.0f,%.0f,%.0f,%.1f,%.1f\n", round, zeroCopy ? "zerocopy" : "sync", pt.set,
			pt.totalBytes, pt.bufCnt, pt.totalBytes / pt.bufCnt, pt.stride, pt.offset, locked ? 1 : 0, iters,
			r.callMed, r.callP99, r.totalMed, r.totalP99, r.pendingPct, r.mbps);
		fflush(stdout);
	};
	for (int round = 1; round <= rounds; ++round)
	{
		for (int zc = 0; zc < 2; ++zc)
		{
			bool zeroCopy = (zc + round) % 2 == 1;	// 회차마다 모드 순서를 바꾼다
			Conn c;
			openConn(c, zeroCopy);
			for (const Point& pt : points)
			{
				report(round, zeroCopy, pt, false, runPoint(c, pt, iters, data));
			}
			for (int lock = 0; lock < 2 && !lockPoints.empty(); ++lock)
			{
				bool locked = (lock + round) % 2 == 1;	// 회차마다 잠금 순서를 바꾼다
				if (locked && !VirtualLock(data, lockSize)) fail("VirtualLock");
				for (const Point& pt : lockPoints)
				{
					report(round, zeroCopy, pt, locked, runPoint(c, pt, iters, data));
				}
				if (locked) VirtualUnlock(data, lockSize);
			}
			closeConn(c);
		}
	}
	VirtualFree(data, 0, MEM_RELEASE);
	WSACleanup();
	return 0;
}
