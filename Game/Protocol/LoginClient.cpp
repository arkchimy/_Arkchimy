#include <WinSock2.h>
#include <WS2tcpip.h>
#include <fstream>
#include <sstream>
#include "LoginClient.h"
#include "Memory.h"
#include "PacketHeader.h"
#include "SecureChannel.h"

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
	const bool bOk = sendAll(sock, buf, PACKET_HEADER_SIZE + static_cast<int>(size));
	SecureZeroMemory(buf, sizeof(buf));
	return bOk;
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

static SOCKET connectTo(const LoginServerAddr& addr)
{
	SOCKADDR_IN sockAddr{};
	sockAddr.sin_family = AF_INET;
	sockAddr.sin_port = htons(static_cast<u_short>(addr.Port));
	if (inet_pton(AF_INET, addr.IP.c_str(), &sockAddr.sin_addr) != 1)
	{
		return INVALID_SOCKET;
	}
	const SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (sock == INVALID_SOCKET)
	{
		return INVALID_SOCKET;
	}
	const DWORD timeout = LOGIN_TIMEOUT_MS;
	setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
	setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
	if (connect(sock, reinterpret_cast<const SOCKADDR*>(&sockAddr), sizeof(sockAddr)) == SOCKET_ERROR)
	{
		closesocket(sock);
		return INVALID_SOCKET;
	}
	return sock;
}

// 키 나누기 - 서버 서명이 미리 가진 공개 키와 맞아야 통로가 준비된다
static eLoginCall handshake(const SOCKET sock, const LoginServerAddr& addr, ChannelCipher* outCipher)
{
	KxKeyPair mine;
	MakeKxKeyPair(&mine);
	ClientHello hello{};
	hello.Type = eLoginType::ClientHello;
	MemCpy(hello.ClientPublicKey, mine.PublicKey, sizeof(hello.ClientPublicKey));
	std::string payload;
	eLoginCall result = eLoginCall::Broken;
	if (sendPacket(sock, &hello, sizeof(hello)) && recvPacket(sock, &payload) && payload.size() == sizeof(ServerHello))
	{
		ServerHello reply;
		MemCpy(&reply, payload.data(), sizeof(reply));
		result = FinishClientHandshake(mine, reply, addr.SignPublicKey, outCipher) ? eLoginCall::Ok : eLoginCall::BadServer;
	}
	WipeKxKeyPair(&mine); // 임시 키는 이 연결에서만 - 버린다
	return result;
}

static bool sendSecure(const SOCKET sock, ChannelCipher& cipher, const void* data, const size_t size)
{
	std::string sealed;
	cipher.Seal(data, size, &sealed);
	return sendPacket(sock, sealed.data(), sealed.size());
}

// 응답 하나를 받아 풀고, 길이가 size와 같으면 outRes에 담는다
static bool recvSecure(const SOCKET sock, ChannelCipher& cipher, void* outRes, const size_t size)
{
	std::string sealed;
	std::string plain;
	const bool bOk = recvPacket(sock, &sealed) && cipher.Open(sealed.data(), sealed.size(), &plain) && plain.size() == size;
	if (bOk)
	{
		MemCpy(outRes, plain.data(), size);
	}
	SecureZeroMemory(plain.data(), plain.size()); // 세션 키가 들어 있을 수 있다
	return bOk;
}

template <size_t N>
static void copyField(char (&outField)[N], const std::string& value)
{
	// 칸보다 길면 잘라 넣는다(서버가 길이를 검사해 거절한다). 0으로 시작해 짧으면 널로 끝난다
	MemCpy(outField, value.data(), (value.size() < N) ? value.size() : N);
}

// 접속 → 키 나누기 → 요청 하나 → 응답 하나 → 끊기
template <typename Req, typename Res>
static eLoginCall exchange(const LoginServerAddr& addr, Req& req, Res* outRes)
{
	const SOCKET sock = connectTo(addr);
	if (sock == INVALID_SOCKET)
	{
		SecureZeroMemory(req.Password, sizeof(req.Password));
		return eLoginCall::NoConnection;
	}
	ChannelCipher cipher;
	eLoginCall result = handshake(sock, addr, &cipher);
	if (result == eLoginCall::Ok && !(sendSecure(sock, cipher, &req, sizeof(req)) && recvSecure(sock, cipher, outRes, sizeof(*outRes))))
	{
		result = eLoginCall::Broken;
	}
	SecureZeroMemory(req.Password, sizeof(req.Password));
	closesocket(sock);
	return result;
}

bool LoadServerPublicKey(const char* path, uint8_t (&outKey)[SIGN_PUBLIC_KEY_LEN])
{
	std::ifstream file(path);
	std::stringstream text;
	text << file.rdbuf();
	const std::string content = text.str();
	const size_t keyPos = content.find("serverPublicKey");
	const size_t eqPos = (keyPos == std::string::npos) ? std::string::npos : content.find('=', keyPos);
	const size_t endPos = (eqPos == std::string::npos) ? std::string::npos : content.find(';', eqPos);
	if (endPos == std::string::npos)
	{
		return false;
	}
	std::string hex = content.substr(eqPos + 1, endPos - eqPos - 1);
	hex.erase(0, hex.find_first_not_of(" \t"));
	hex.erase(hex.find_last_not_of(" \t") + 1);
	return HexToBytes(hex, outKey, sizeof(outKey));
}

eLoginCall RequestRegister(const LoginServerAddr& addr, const std::string& accountID, const std::string& password, eRegisterResult* outResult)
{
	Assert(outResult != nullptr, "RequestRegister: outResult가 null");
	RegisterReq req{};
	req.Type = eLoginType::RegisterReq;
	copyField(req.AccountID, accountID);
	copyField(req.Password, password);
	RegisterRes res{};
	const eLoginCall result = exchange(addr, req, &res);
	*outResult = res.Result;
	return (result == eLoginCall::Ok && res.Type != eLoginType::RegisterRes) ? eLoginCall::Broken : result;
}

eLoginCall RequestAuth(const LoginServerAddr& addr, const std::string& accountID, const std::string& password, AuthRes* outRes)
{
	Assert(outRes != nullptr, "RequestAuth: outRes가 null");
	AuthReq req{};
	req.Type = eLoginType::AuthReq;
	copyField(req.AccountID, accountID);
	copyField(req.Password, password);
	const eLoginCall result = exchange(addr, req, outRes);
	return (result == eLoginCall::Ok && outRes->Type != eLoginType::AuthRes) ? eLoginCall::Broken : result;
}

int RequestAuthRepeat(const LoginServerAddr& addr, const std::string& accountID, const std::string& password, const int count)
{
	const SOCKET sock = connectTo(addr);
	if (sock == INVALID_SOCKET)
	{
		return 0;
	}
	ChannelCipher cipher;
	int answered = 0;
	if (handshake(sock, addr, &cipher) == eLoginCall::Ok)
	{
		for (int i = 0; i < count; ++i)
		{
			AuthReq req{};
			req.Type = eLoginType::AuthReq;
			copyField(req.AccountID, accountID);
			copyField(req.Password, password);
			AuthRes res{};
			const bool bOk = sendSecure(sock, cipher, &req, sizeof(req)) && recvSecure(sock, cipher, &res, sizeof(res));
			SecureZeroMemory(req.Password, sizeof(req.Password));
			if (!bOk)
			{
				break;
			}
			++answered;
		}
	}
	closesocket(sock);
	return answered;
}

bool RequestAuthTampered(const LoginServerAddr& addr, const std::string& accountID, const std::string& password)
{
	const SOCKET sock = connectTo(addr);
	if (sock == INVALID_SOCKET)
	{
		return false;
	}
	ChannelCipher cipher;
	bool bClosedWithoutAnswer = false;
	if (handshake(sock, addr, &cipher) == eLoginCall::Ok)
	{
		AuthReq req{};
		req.Type = eLoginType::AuthReq;
		copyField(req.AccountID, accountID);
		copyField(req.Password, password);
		std::string sealed;
		cipher.Seal(&req, sizeof(req), &sealed);
		SecureZeroMemory(req.Password, sizeof(req.Password));
		sealed[sealed.size() / 2] ^= 1; // 중간 한 바이트를 바꾼다 - 확인 값이 맞지 않아야 한다
		std::string reply;
		bClosedWithoutAnswer = sendPacket(sock, sealed.data(), sealed.size()) && !recvPacket(sock, &reply);
	}
	closesocket(sock);
	return bClosedWithoutAnswer;
}
