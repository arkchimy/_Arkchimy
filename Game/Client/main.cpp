#include <WinSock2.h>
#include <WS2tcpip.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include "GameProtocol.h"
#include "PacketHeader.h"

#pragma comment(lib, "ws2_32.lib")

// 사람이 직접 하는 콘솔 클라 - 수신 스레드가 서버 메시지를 언제든 출력하고, main은 명령을 입력받아 보낸다
// (파티원 행동·경매 입찰 알림처럼 내가 요청하지 않은 메시지도 오므로 입력과 수신을 나눈다)

static std::mutex sPrintLock; // 수신 스레드와 main이 함께 화면에 쓴다 - 줄이 섞이지 않게

static bool sendAll(const SOCKET sock, const char* data, const int len)
{
	int sent = 0;
	while (sent < len)
	{
		const int ret = send(sock, data + sent, len - sent, 0);
		if (ret <= 0)
		{
			return false;
		}
		sent += ret;
	}
	return true;
}

static bool recvAll(const SOCKET sock, char* buf, const int len)
{
	int received = 0;
	while (received < len)
	{
		const int ret = recv(sock, buf + received, len - received, 0);
		if (ret <= 0)
		{
			return false; // 서버가 끊었거나 오류
		}
		received += ret;
	}
	return true;
}

// 라이브러리 헤더(길이 2바이트)를 붙여 구조체 하나를 보낸다
template <typename T>
static bool sendPacket(const SOCKET sock, const T& body)
{
	static_assert(sizeof(T) <= MAX_PAYLOAD_SIZE, "패킷 하나에 들어가야 한다");
	char buf[PACKET_HEADER_SIZE + sizeof(T)];
	const PacketHeader header{ static_cast<uint16_t>(sizeof(T)) };
	memcpy(buf, &header, PACKET_HEADER_SIZE);
	memcpy(buf + PACKET_HEADER_SIZE, &body, sizeof(T));
	return sendAll(sock, buf, sizeof(buf));
}

static const char* toString(const eLoginResult result)
{
	switch (result)
	{
	case eLoginResult::Ok:
		return "성공";
	case eLoginResult::BadID:
		return "잘못된 아이디(비었거나 15자 넘음)";
	case eLoginResult::AlreadyLoggedIn:
		return "이미 접속 중인 아이디";
	default:
		return "알 수 없는 결과";
	}
}

static void printInventory(const InventoryRes& res)
{
	static const char* SLOT_NAMES[] = { "무기", "머리", "장신구", "갑옷" };
	printf("[인벤토리] 골드 %lld, 아이템 %u개\n", static_cast<long long>(res.Gold), res.ItemCnt);
	for (int32_t slot = 0; slot < static_cast<int32_t>(eEquipSlot::Count); ++slot)
	{
		if (res.Equipped[slot] != 0)
		{
			printf("  착용 %s: 아이템 #%llu\n", SLOT_NAMES[slot], static_cast<unsigned long long>(res.Equipped[slot]));
		}
	}
	for (int32_t i = 0; i < res.ItemCnt && i < INVENTORY_MAX; ++i)
	{
		printf("  #%llu 종류 %u\n", static_cast<unsigned long long>(res.Items[i].ItemUID), res.Items[i].ItemKind); // 이름은 아이템 데이터 파일을 만든 뒤에
	}
}

static void printPacket(const char* payload, const int len)
{
	eGameType type;
	if (len < static_cast<int>(sizeof(type)))
	{
		printf("[수신] 종류도 없는 패킷 len=%d\n", len);
		return;
	}
	memcpy(&type, payload, sizeof(type));

	std::lock_guard<std::mutex> lock(sPrintLock);
	switch (type)
	{
	case eGameType::LoginRes:
		if (len == static_cast<int>(sizeof(LoginRes)))
		{
			LoginRes res;
			memcpy(&res, payload, sizeof(res));
			printf("[로그인] %s\n", toString(res.Result));
			return;
		}
		break;
	case eGameType::InventoryRes:
		if (len == static_cast<int>(sizeof(InventoryRes)))
		{
			InventoryRes res;
			memcpy(&res, payload, sizeof(res));
			printInventory(res);
			return;
		}
		break;
	default:
		break;
	}
	printf("[수신] 모르는 패킷 type=%u len=%d\n", static_cast<unsigned>(type), len);
}

static std::atomic<bool> sbQuitting = false; // quit으로 내가 끊는 중 - 이때는 "연결 끊김"을 출력하지 않는다

static void runRecvLoop(const SOCKET sock, std::atomic<bool>* outConnected)
{
	char payload[MAX_PAYLOAD_SIZE];
	while (true)
	{
		PacketHeader header;
		if (!recvAll(sock, reinterpret_cast<char*>(&header), PACKET_HEADER_SIZE) || header.Len > MAX_PAYLOAD_SIZE
			|| !recvAll(sock, payload, header.Len))
		{
			break;
		}
		printPacket(payload, header.Len);
	}
	*outConnected = false;
	if (!sbQuitting)
	{
		std::lock_guard<std::mutex> lock(sPrintLock);
		printf("[연결 끊김] 서버가 연결을 끊었습니다. Enter를 누르면 종료합니다\n");
	}
}

static void printHelp()
{
	std::lock_guard<std::mutex> lock(sPrintLock);
	printf("명령: login <아이디> | inv | help | quit\n");
}

int main(int argc, char* argv[])
{
	const char* ip = (argc > 1) ? argv[1] : "127.0.0.1";
	const int port = (argc > 2) ? atoi(argv[2]) : 33000;

	WSADATA wsa;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
	{
		printf("WSAStartup 실패\n");
		return 1;
	}
	const SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	SOCKADDR_IN addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(static_cast<u_short>(port));
	if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1 || connect(sock, reinterpret_cast<SOCKADDR*>(&addr), sizeof(addr)) == SOCKET_ERROR)
	{
		printf("접속 실패 %s:%d err=%d\n", ip, port, WSAGetLastError());
		closesocket(sock);
		WSACleanup();
		return 1;
	}
	printf("접속 %s:%d\n", ip, port);
	printHelp();

	std::atomic<bool> bConnected = true;
	std::thread recvThread(runRecvLoop, sock, &bConnected);

	std::string line;
	while (bConnected && std::getline(std::cin, line))
	{
		std::istringstream in(line);
		std::string cmd;
		in >> cmd;
		if (cmd.empty())
		{
			continue;
		}
		if (cmd == "quit")
		{
			break;
		}
		if (cmd == "help")
		{
			printHelp();
		}
		else if (cmd == "login")
		{
			std::string id;
			in >> id;
			LoginReq req{};
			req.Type = eGameType::LoginReq;
			strncpy_s(req.AccountID, id.c_str(), _TRUNCATE); // 15바이트를 넘으면 잘린다(널 자리 1바이트)
			sendPacket(sock, req);
		}
		else if (cmd == "inv")
		{
			InventoryReq req;
			req.Type = eGameType::InventoryReq;
			sendPacket(sock, req);
		}
		else
		{
			printHelp();
		}
	}

	sbQuitting = true;
	shutdown(sock, SD_BOTH); // 수신 스레드의 recv를 깨운다
	closesocket(sock);
	recvThread.join();
	WSACleanup();
	return 0;
}
