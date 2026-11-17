#include <WinSock2.h>
#include <WS2tcpip.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <random>
#include <string>
#include <thread>
#include <vector>
#include "RsaEchoProtocol.h"
#include "LoginCrypto.h"
#include "Memory.h"
#include "PacketHeader.h"
#include "Profiler.h" // WinSock2.h 뒤에 - Windows.h가 먼저 들어오면 재정의 오류(README_ERRORS 1-26)

#pragma comment(lib, "ws2_32.lib")

// RSA 에코 클라 - ① RSA 연산 하나씩 따로 재기 ② 서버와 로그인 서버 방식으로 잠근 메시지를 주고받으며 단계별로 재기
// 결과는 ProfilerLib 파일(작업 폴더의 RsaEchoClient_<시각>_Profiler.txt)로 저장하고 화면에도 보여 준다
// 사용법: RsaEchoClient.exe [연결 수=1] [연결마다 메시지 수=1000] [평문 바이트=100] [IP=127.0.0.1] [포트=32100] [공개 키 파일=..\Server\rsa_echo_public.cfg]

enum : int32_t
{
	BENCH_KEYGEN_CNT = 20, // 키 쌍 만들기는 수십 ms라 적게
	BENCH_OP_CNT = 500,
	RECV_TIMEOUT_MS = 10000,
};

struct ClientArgs
{
	int32_t ConnectionCnt = 1;
	int32_t MessageCnt = 1000;
	int32_t PlainLen = 100;
	std::string IP = "127.0.0.1";
	int32_t Port = RSA_ECHO_PORT;
	std::string KeyFile = std::string("..\\Server\\") + RSA_ECHO_PUBLIC_CFG; // 작업 폴더가 RsaEcho\Client일 때
};

struct EchoResult
{
	std::atomic<int64_t> OkCnt = 0;
	std::atomic<int64_t> FailCnt = 0;       // 접속·Hello·송수신 실패로 끝난 연결
	std::atomic<int64_t> MismatchCnt = 0;   // 돌아온 평문이 보낸 것과 다름
};

static bool sendAll(const SOCKET sock, const char* data, const int size)
{
	int sent = 0;
	while (sent < size)
	{
		const int ret = send(sock, data + sent, size - sent, 0);
		if (ret <= 0)
		{
			return false;
		}
		sent += ret;
	}
	return true;
}

static bool recvAll(const SOCKET sock, char* outData, const int size)
{
	int received = 0;
	while (received < size)
	{
		const int ret = recv(sock, outData + received, size - received, 0);
		if (ret <= 0)
		{
			return false;
		}
		received += ret;
	}
	return true;
}

// 라이브러리 헤더(길이 2바이트)를 붙여 보낸다
static bool sendPacket(const SOCKET sock, const void* data, const size_t size)
{
	if (size > MAX_PAYLOAD_SIZE)
	{
		return false;
	}
	char buf[PACKET_HEADER_SIZE + MAX_PAYLOAD_SIZE];
	const PacketHeader header{ static_cast<uint16_t>(size) };
	MemCpy(buf, &header, PACKET_HEADER_SIZE);
	MemCpy(buf + PACKET_HEADER_SIZE, data, size);
	return sendAll(sock, buf, PACKET_HEADER_SIZE + static_cast<int>(size));
}

static bool recvPacket(const SOCKET sock, std::string* outPayload)
{
	PacketHeader header{};
	if (!recvAll(sock, reinterpret_cast<char*>(&header), PACKET_HEADER_SIZE) || header.Len > MAX_PAYLOAD_SIZE)
	{
		return false;
	}
	outPayload->resize(header.Len);
	return recvAll(sock, outPayload->data(), header.Len);
}

static SOCKET connectTo(const ClientArgs& args)
{
	SOCKADDR_IN addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(static_cast<u_short>(args.Port));
	if (inet_pton(AF_INET, args.IP.c_str(), &addr.sin_addr) != 1)
	{
		return INVALID_SOCKET;
	}
	const SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (sock == INVALID_SOCKET)
	{
		return INVALID_SOCKET;
	}
	const DWORD timeout = RECV_TIMEOUT_MS;
	setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
	const BOOL bNoDelay = TRUE; // 512바이트 패킷 하나씩 주고받는다 - Nagle이 응답을 붙잡아 시간이 늘지 않게
	setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&bNoDelay), sizeof(bNoDelay));
	if (connect(sock, reinterpret_cast<const SOCKADDR*>(&addr), sizeof(addr)) == SOCKET_ERROR)
	{
		closesocket(sock);
		return INVALID_SOCKET;
	}
	return sock;
}

// rsa_echo_public.cfg의 serverPublicKey = <16진수 512글자>; - 로그인 클라의 LoadServerPublicKey와 같은 형식
// "//" 주석 줄은 건너뛰고, 줄 맨 앞이 serverPublicKey인 줄만 본다(주석 속 같은 이름에 걸리지 않게)
static bool loadPublicKey(const std::string& path, uint8_t (&outKey)[RSA_MODULUS_LEN])
{
	std::ifstream file(path);
	std::string line;
	bool bFound = false;
	while (!bFound && std::getline(file, line))
	{
		const size_t start = line.find_first_not_of(" \t\xEF\xBB\xBF");
		if (start == std::string::npos || line.compare(start, 2, "//") == 0 || line.compare(start, 15, "serverPublicKey") != 0)
		{
			continue;
		}
		const size_t eqPos = line.find('=', start);
		const size_t endPos = (eqPos == std::string::npos) ? std::string::npos : line.find(';', eqPos);
		if (endPos == std::string::npos)
		{
			break;
		}
		std::string hex = line.substr(eqPos + 1, endPos - eqPos - 1);
		hex.erase(0, hex.find_first_not_of(" \t"));
		hex.erase(hex.find_last_not_of(" \t") + 1);
		bFound = FromHex(hex, outKey, sizeof(outKey));
		break;
	}
	return bFound;
}

// ① RSA 연산 하나씩 - 네트워크 없이 같은 키로 반복
static bool runPrimitiveBench(const int32_t plainLen)
{
	Profile::SetThreadName("1_bench");
	RsaKey mine;
	for (int32_t i = 0; i < BENCH_KEYGEN_CNT; ++i)
	{
		ProfileScope scope("RSA keygen(2048)");
		if (!mine.Generate())
		{
			return false;
		}
	}
	uint8_t modulus[RSA_MODULUS_LEN];
	RsaKey peer; // 상대가 가진 것처럼 공개 키만
	if (!mine.ExportPublic(modulus) || !peer.ImportPublic(modulus))
	{
		return false;
	}
	std::string plain(static_cast<size_t>(plainLen), 'a');
	uint8_t locked[RSA_MODULUS_LEN];
	uint8_t sign[RSA_SIGN_LEN];
	std::string unlocked;
	bool bOk = true;
	for (int32_t i = 0; i < BENCH_OP_CNT && bOk; ++i)
	{
		{
			ProfileScope scope("RSA lock(pub,OAEP)");
			bOk = peer.Lock(plain.data(), plain.size(), locked);
		}
		{
			ProfileScope scope("RSA unlock(priv,OAEP)");
			bOk = bOk && mine.Unlock(locked, &unlocked);
		}
		{
			ProfileScope scope("RSA sign(priv,PSS)");
			bOk = bOk && mine.Sign(locked, sizeof(locked), sign);
		}
		{
			ProfileScope scope("RSA verify(pub,PSS)");
			bOk = bOk && peer.Verify(locked, sizeof(locked), sign);
		}
		bOk = bOk && unlocked == plain;
	}
	return bOk;
}

// ② 연결 하나 - 접속 → Hello → 메시지 MessageCnt번 왕복
static void runConnection(const int32_t index, const ClientArgs& args, const uint8_t (&serverSignKey)[RSA_MODULUS_LEN], EchoResult* outResult)
{
	char threadName[MAX_THREAD_NAME_LEN];
	sprintf_s(threadName, "2_conn_%03d", index);
	Profile::SetThreadName(threadName);

	const SOCKET sock = connectTo(args);
	if (sock == INVALID_SOCKET)
	{
		++outResult->FailCnt;
		return;
	}
	LoginChannel channel;
	ClientHello hello{};
	std::string payload;
	bool bOk = false;
	{
		ProfileScope scope("C keygen+ClientHello");
		bOk = channel.StartClient(&hello);
	}
	bOk = bOk && sendPacket(sock, &hello, sizeof(hello)) && recvPacket(sock, &payload) && payload.size() == sizeof(ServerHello);
	if (bOk)
	{
		ServerHello reply;
		MemCpy(&reply, payload.data(), sizeof(reply));
		ProfileScope scope("C verify ServerHello");
		bOk = channel.FinishClient(reply, serverSignKey);
	}

	std::mt19937 random(static_cast<uint32_t>(index) * 7919u + 1u);
	std::string plain(static_cast<size_t>(args.PlainLen), '\0');
	std::string sealed;
	std::string reply;
	std::string echoed;
	for (int32_t i = 0; i < args.MessageCnt && bOk; ++i)
	{
		for (char& c : plain)
		{
			c = static_cast<char>(random() & 0xFF);
		}
		ProfileScope roundTrip("C round trip");
		{
			ProfileScope scope("C Seal(lock+sign)");
			bOk = channel.Seal(plain.data(), plain.size(), &sealed);
		}
		bOk = bOk && sendPacket(sock, sealed.data(), sealed.size());
		{
			ProfileScope scope("C wait reply");
			bOk = bOk && recvPacket(sock, &reply);
		}
		{
			ProfileScope scope("C Open(verify+unlock)");
			bOk = bOk && channel.Open(reply.data(), reply.size(), &echoed);
		}
		if (bOk && echoed != plain)
		{
			++outResult->MismatchCnt;
			bOk = false;
		}
		if (bOk)
		{
			++outResult->OkCnt;
		}
	}
	if (!bOk)
	{
		++outResult->FailCnt;
	}
	closesocket(sock);
}

static void printLatestProfile()
{
	WIN32_FIND_DATAA data;
	const HANDLE find = FindFirstFileA("RsaEchoClient_*_Profiler.txt", &data);
	if (find == INVALID_HANDLE_VALUE)
	{
		return;
	}
	std::string latest = data.cFileName;
	FILETIME latestTime = data.ftLastWriteTime;
	while (FindNextFileA(find, &data))
	{
		if (CompareFileTime(&data.ftLastWriteTime, &latestTime) > 0)
		{
			latest = data.cFileName;
			latestTime = data.ftLastWriteTime;
		}
	}
	FindClose(find);
	std::ifstream file(latest);
	std::string line;
	printf("\n===== %s =====\n", latest.c_str());
	printf("RSA keygen = 키 쌍 만들기, lock = 공개 키로 잠그기, unlock = 비밀 키로 풀기, sign = 비밀 키로 서명, verify = 공개 키로 서명 확인\n");
	printf("C Seal = 서버 임시 키로 잠그기 + 클라 임시 키로 서명, C Open = 서버 고정 키 서명 확인 + 클라 임시 키로 풀기\n");
	printf("C wait reply = 보낸 뒤 응답이 다 올 때까지(서버 Open + Seal + 네트워크), C round trip = Seal부터 Open까지 전체\n\n");
	while (std::getline(file, line))
	{
		printf("%s\n", line.c_str());
	}
}

int main(int argc, char* argv[])
{
	ClientArgs args;
	if (argc > 1)
	{
		args.ConnectionCnt = atoi(argv[1]);
	}
	if (argc > 2)
	{
		args.MessageCnt = atoi(argv[2]);
	}
	if (argc > 3)
	{
		args.PlainLen = atoi(argv[3]);
	}
	if (argc > 4)
	{
		args.IP = argv[4];
	}
	if (argc > 5)
	{
		args.Port = atoi(argv[5]);
	}
	if (argc > 6)
	{
		args.KeyFile = argv[6];
	}
	if (args.ConnectionCnt < 1 || args.MessageCnt < 0 || args.PlainLen < 1 || args.PlainLen > RSA_MAX_PLAIN_LEN)
	{
		fprintf(stderr, "사용법: RsaEchoClient.exe [연결 수>=1] [메시지 수] [평문 1-%d] [IP] [포트] [공개 키 파일]\n", static_cast<int>(RSA_MAX_PLAIN_LEN));
		return 1;
	}

	uint8_t serverSignKey[RSA_MODULUS_LEN];
	if (!loadPublicKey(args.KeyFile, serverSignKey))
	{
		fprintf(stderr, "서버 고정 공개 키를 읽지 못함: %s - serverPublicKey 줄이 있어야 한다(RsaEchoServer.exe --genkey, rsa_echo_public.cfg.example 참고)\n", args.KeyFile.c_str());
		return 1;
	}

	printf("[1] RSA 연산 하나씩 - 키 쌍 %d번, 나머지 %d번씩 (평문 %d바이트)\n", static_cast<int>(BENCH_KEYGEN_CNT), static_cast<int>(BENCH_OP_CNT), static_cast<int>(args.PlainLen));
	if (!runPrimitiveBench(args.PlainLen))
	{
		fprintf(stderr, "RSA 연산 실패(CNG)\n");
		return 1;
	}

	WSADATA wsa;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
	{
		return 1;
	}
	printf("[2] 에코 - 연결 %d개 × 메시지 %d번, %s:%d\n", static_cast<int>(args.ConnectionCnt), static_cast<int>(args.MessageCnt), args.IP.c_str(), static_cast<int>(args.Port));
	EchoResult result;
	const auto start = std::chrono::steady_clock::now();
	std::vector<std::thread> threads;
	threads.reserve(static_cast<size_t>(args.ConnectionCnt));
	for (int32_t i = 0; i < args.ConnectionCnt; ++i)
	{
		threads.emplace_back(runConnection, i, std::cref(args), std::cref(serverSignKey), &result);
	}
	for (std::thread& thread : threads)
	{
		thread.join();
	}
	const double elapsedSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
	WSACleanup();

	printf("    에코 성공 %lld번, 실패한 연결 %lld개, 평문 불일치 %lld번, %.2f초 (%.0f번/s)\n", static_cast<long long>(result.OkCnt.load()),
		static_cast<long long>(result.FailCnt.load()), static_cast<long long>(result.MismatchCnt.load()), elapsedSec,
		elapsedSec > 0.0 ? static_cast<double>(result.OkCnt.load()) / elapsedSec : 0.0);
	if (!Profile::Save())
	{
		fprintf(stderr, "측정 파일을 쓰지 못함\n");
		return 1;
	}
	printLatestProfile();
	return (result.FailCnt == 0 && result.MismatchCnt == 0) ? 0 : 1;
}
