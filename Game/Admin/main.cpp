#include <WinSock2.h>
#include <WS2tcpip.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include "AdminProtocol.h"
#include "PacketHeader.h"
#include "ParserLib.h"

#pragma comment(lib, "ws2_32.lib")

// 운영 툴(설계 3-5) - 차단·해제를 게임 서버의 운영자 포트로 명령한다(메모리와 DB를 서버가 함께 고친다)
// 명령 하나를 보내고 답 하나를 받는 단순한 구조(받는 스레드 없음). 2026-11-20 범위 축소로 회수·로그 조회는 지웠다

// 고정 길이 칸에 넣는다 - 넘치면 잘라 널로 끝나게
template <size_t N>
static void copyText(char (&outText)[N], const std::string& text)
{
	memset(outText, 0, N);
	memcpy(outText, text.data(), (text.size() < N) ? text.size() : N - 1);
}

static bool sendAll(const SOCKET sock, const char* data, const int len)
{
	for (int sent = 0; sent < len;)
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
	for (int received = 0; received < len;)
	{
		const int ret = recv(sock, buf + received, len - received, 0);
		if (ret <= 0)
		{
			return false;
		}
		received += ret;
	}
	return true;
}

template <typename T>
static bool sendPacket(const SOCKET sock, const T& body)
{
	char buf[PACKET_HEADER_SIZE + sizeof(T)];
	const PacketHeader header{ static_cast<uint16_t>(sizeof(T)) };
	memcpy(buf, &header, PACKET_HEADER_SIZE);
	memcpy(buf + PACKET_HEADER_SIZE, &body, sizeof(T));
	return sendAll(sock, buf, sizeof(buf));
}

// 답 하나를 받아 T로 꺼낸다 - 길이·종류가 맞지 않거나 끊기면 false
template <typename T>
static bool recvPacket(const SOCKET sock, const eAdminType type, T* outPacket)
{
	PacketHeader header;
	char payload[MAX_PAYLOAD_SIZE];
	if (!recvAll(sock, reinterpret_cast<char*>(&header), PACKET_HEADER_SIZE) || header.Len != sizeof(T) || !recvAll(sock, payload, header.Len))
	{
		return false;
	}
	memcpy(outPacket, payload, sizeof(T));
	return outPacket->Type == type;
}

static const char* toString(const eBanResult result)
{
	switch (result)
	{
	case eBanResult::Ok:
		return "적용함";
	case eBanResult::NotFound:
		return "그런 계정이 없음";
	case eBanResult::BadValue:
		return "분이 범위 밖";
	case eBanResult::DbError:
		return "DB 오류 또는 서버 종료 중";
	default:
		return "알 수 없는 결과";
	}
}

static void printHelp()
{
	printf("명령: ban <아이디> <분> | unban <아이디> | help | quit\n");
}

int main(int argc, char* argv[])
{
	const char* ip = (argc > 1) ? argv[1] : "127.0.0.1";
	const int port = (argc > 2) ? atoi(argv[2]) : 33001;

	// 서버와 같은 db.cfg에서 운영자 키만 읽는다(DB에는 접속하지 않는다)
	utility::stDebug debug;
	const std::unique_ptr<utility::Parser> parser = utility::Parser::CreateParser("db.cfg", debug);
	std::string adminKey;
	if (parser == nullptr || !parser->Get("adminKey", adminKey) || adminKey.empty())
	{
		printf("db.cfg를 읽지 못함 - adminKey가 있어야 한다(작업 폴더에 db.cfg)\n");
		return 1;
	}

	WSADATA wsa;
	WSAStartup(MAKEWORD(2, 2), &wsa);
	const SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	SOCKADDR_IN addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(static_cast<u_short>(port));
	AdminLoginReq loginReq{};
	loginReq.Type = eAdminType::LoginReq;
	copyText(loginReq.Key, adminKey);
	AdminLoginRes loginRes{};
	if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1 || connect(sock, reinterpret_cast<SOCKADDR*>(&addr), sizeof(addr)) == SOCKET_ERROR
		|| !sendPacket(sock, loginReq) || !recvPacket(sock, eAdminType::LoginRes, &loginRes) || loginRes.bOk == 0)
	{
		printf("운영자 포트 %s:%d 접속·인증 실패 err=%d\n", ip, port, WSAGetLastError());
		closesocket(sock);
		WSACleanup();
		return 1;
	}
	printf("운영자 포트 %s:%d 접속\n", ip, port);
	printHelp();

	std::string line;
	bool bConnected = true;
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
		if (cmd == "ban" || cmd == "unban")
		{
			AdminBanReq req{};
			req.Type = eAdminType::BanReq;
			std::string accountID;
			in >> accountID;
			// 분을 못 읽으면 0(= 해제)이 되어 차단이 풀린다 - 차단은 숫자로만 된 1분 이상만("1h"처럼 섞이면 거절), 해제는 unban으로만
			if (cmd == "ban")
			{
				std::string minutes;
				in >> minutes;
				req.Minutes = (!minutes.empty() && minutes.size() <= 6 && minutes.find_first_not_of("0123456789") == std::string::npos) ? atoi(minutes.c_str()) : 0;
				if (req.Minutes <= 0)
				{
					printf("[제재] 분은 1 이상의 숫자 - 해제는 unban <아이디>\n");
					continue;
				}
			}
			copyText(req.AccountID, accountID);
			AdminBanRes res{};
			bConnected = sendPacket(sock, req) && recvPacket(sock, eAdminType::BanRes, &res);
			if (bConnected)
			{
				printf("[%s] %s %s%s\n", (req.Minutes > 0) ? "제재" : "해제", accountID.c_str(), toString(res.Result), res.bKicked ? " - 접속 중이라 끊음" : "");
			}
		}
		else
		{
			printHelp();
		}
	}
	if (!bConnected)
	{
		printf("[연결 끊김] 서버가 연결을 끊었습니다\n");
	}
	closesocket(sock);
	WSACleanup();
	return 0;
}
