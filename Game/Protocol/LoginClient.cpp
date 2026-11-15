#include <WinSock2.h>
#include <WS2tcpip.h>
#include "LoginClient.h"
#include "Memory.h"
#include "PacketHeader.h"

enum : int32_t
{
	LOGIN_TIMEOUT_MS = 10000, // 해시가 수십 ms라 넉넉히 - 로그인 서버가 바쁘면 대기열에서 기다린다
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

// 요청 하나 보내고, 응답 하나(길이가 resSize와 같아야 함)를 받는다
static bool exchange(const char* ip, const int port, const void* req, const int reqSize, void* outRes, const int resSize)
{
	SOCKADDR_IN addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(static_cast<u_short>(port));
	if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1)
	{
		return false;
	}
	const SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (sock == INVALID_SOCKET)
	{
		return false;
	}
	const DWORD timeout = LOGIN_TIMEOUT_MS;
	setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
	setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
	bool bOk = connect(sock, reinterpret_cast<const SOCKADDR*>(&addr), sizeof(addr)) != SOCKET_ERROR;
	if (bOk)
	{
		char buf[PACKET_HEADER_SIZE + MAX_PAYLOAD_SIZE];
		const PacketHeader header{ static_cast<uint16_t>(reqSize) };
		MemCpy(buf, &header, PACKET_HEADER_SIZE);
		MemCpy(buf + PACKET_HEADER_SIZE, req, static_cast<size_t>(reqSize));
		bOk = sendAll(sock, buf, PACKET_HEADER_SIZE + reqSize);
		SecureZeroMemory(buf, sizeof(buf)); // 비밀번호가 들어 있었다
	}
	PacketHeader resHeader{};
	bOk = bOk && recvAll(sock, reinterpret_cast<char*>(&resHeader), PACKET_HEADER_SIZE) && resHeader.Len == resSize
		&& recvAll(sock, static_cast<char*>(outRes), resSize);
	closesocket(sock);
	return bOk;
}

template <size_t N>
static void copyField(char (&outField)[N], const std::string& value)
{
	// 칸보다 길면 잘라 넣는다(서버가 길이를 검사해 거절한다). 0으로 시작해 짧으면 널로 끝난다
	MemCpy(outField, value.data(), (value.size() < N) ? value.size() : N);
}

bool RequestRegister(const char* ip, const int port, const std::string& accountID, const std::string& password, eRegisterResult* outResult)
{
	Assert(outResult != nullptr, "RequestRegister: outResult가 null");
	RegisterReq req{};
	req.Type = eLoginType::RegisterReq;
	copyField(req.AccountID, accountID);
	copyField(req.Password, password);
	RegisterRes res{};
	const bool bOk = exchange(ip, port, &req, sizeof(req), &res, sizeof(res)) && res.Type == eLoginType::RegisterRes;
	SecureZeroMemory(req.Password, sizeof(req.Password));
	*outResult = res.Result;
	return bOk;
}

bool RequestAuth(const char* ip, const int port, const std::string& accountID, const std::string& password, AuthRes* outRes)
{
	Assert(outRes != nullptr, "RequestAuth: outRes가 null");
	AuthReq req{};
	req.Type = eLoginType::AuthReq;
	copyField(req.AccountID, accountID);
	copyField(req.Password, password);
	const bool bOk = exchange(ip, port, &req, sizeof(req), outRes, sizeof(*outRes)) && outRes->Type == eLoginType::AuthRes;
	SecureZeroMemory(req.Password, sizeof(req.Password));
	return bOk;
}
