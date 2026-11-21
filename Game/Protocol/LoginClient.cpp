#include <WinSock2.h>
#include <WS2tcpip.h>
#include <fstream>
#include "LoginClient.h"
#include "Memory.h"
#include "PacketHeader.h"
#include "LoginCrypto.h"
#include "SecureChannel.h"

enum : int32_t
{
	LOGIN_TIMEOUT_MS = 10000, // 해시·RSA 키 쌍 만들기가 수십 ms씩이라 넉넉히 - 로그인 서버가 바쁘면 대기열에서 기다린다
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

// 임시 공개 키 주고받기 - 서버 서명이 미리 가진 공개 키와 맞아야 통로가 준비된다
static eLoginCall handshake(const SOCKET sock, const LoginServerAddr& addr, LoginChannel* outChannel)
{
	Assert(outChannel != nullptr, "handshake: outChannel이 null");
	HelloServer hello{};
	if (!outChannel->StartClient(&hello))
	{
		return eLoginCall::Broken;
	}
	std::string payload;
	eLoginCall result = eLoginCall::Broken;
	if (sendPacket(sock, &hello, sizeof(hello)) && recvPacket(sock, &payload) && payload.size() == sizeof(HelloClient))
	{
		HelloClient reply;
		MemCpy(&reply, payload.data(), sizeof(reply));
		result = outChannel->FinishClient(reply, addr.SignPublicKey) ? eLoginCall::Ok : eLoginCall::BadServer;
	}
	return result; // 임시 키는 outChannel이 없어질 때 지운다
}

static bool sendSecure(const SOCKET sock, LoginChannel& channel, const void* data, const size_t size)
{
	std::string sealed;
	return channel.Seal(data, size, &sealed) && sendPacket(sock, sealed.data(), sealed.size());
}

// 응답 하나를 받아 풀고, 길이가 size와 같으면 outRes에 담는다
static bool recvSecure(const SOCKET sock, LoginChannel& channel, void* outRes, const size_t size)
{
	std::string sealed;
	std::string plain;
	const bool bOk = recvPacket(sock, &sealed) && channel.Open(sealed.data(), sealed.size(), &plain) && plain.size() == size;
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

// 가입 응답은 확인을 보내지 않는다
static void sendAckIfNeeded(const SOCKET, LoginChannel&, const RegisterRes&)
{
}

// 로그인 성공 응답(세션 키)을 받았다고 알린다 - 서버는 이것을 받거나 loginAckSec이 지나면 끊는다.
// 보내기가 실패해도 세션 키는 이미 받았다(서버가 기한으로 끊는다)
static void sendAckIfNeeded(const SOCKET sock, LoginChannel& channel, const AuthRes& res)
{
	if (res.Type == eLoginType::AuthRes && res.Result == eAuthResult::Ok)
	{
		const AuthAck ack{ eLoginType::AuthAck };
		sendSecure(sock, channel, &ack, sizeof(ack));
	}
}

// 접속 → 키 나누기 → 요청 하나 → 응답 하나 → (로그인 성공이면 확인) → 끊기
template <typename Req, typename Res>
static eLoginCall exchange(const LoginServerAddr& addr, Req& req, Res* outRes)
{
	const SOCKET sock = connectTo(addr);
	if (sock == INVALID_SOCKET)
	{
		SecureZeroMemory(req.Password, sizeof(req.Password));
		return eLoginCall::NoConnection;
	}
	LoginChannel channel;
	eLoginCall result = handshake(sock, addr, &channel);
	if (result == eLoginCall::Ok && !(sendSecure(sock, channel, &req, sizeof(req)) && recvSecure(sock, channel, outRes, sizeof(*outRes))))
	{
		result = eLoginCall::Broken;
	}
	if (result == eLoginCall::Ok)
	{
		sendAckIfNeeded(sock, channel, *outRes);
	}
	SecureZeroMemory(req.Password, sizeof(req.Password));
	closesocket(sock);
	return result;
}

bool LoadServerPublicKey(const char* path, uint8_t (&outKey)[RSA_MODULUS_LEN])
{
	// 줄마다 읽어 "//" 주석 줄은 건너뛰고, 줄 맨 앞이 serverPublicKey인 줄만 본다(주석 속 같은 이름에 걸리지 않게)
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
		bFound = HexToBytes(hex, outKey, sizeof(outKey));
		break;
	}
	return bFound;
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
	LoginChannel channel;
	int answered = 0;
	if (handshake(sock, addr, &channel) == eLoginCall::Ok)
	{
		for (int i = 0; i < count; ++i)
		{
			AuthReq req{};
			req.Type = eLoginType::AuthReq;
			copyField(req.AccountID, accountID);
			copyField(req.Password, password);
			AuthRes res{};
			const bool bOk = sendSecure(sock, channel, &req, sizeof(req)) && recvSecure(sock, channel, &res, sizeof(res));
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
	LoginChannel channel;
	bool bClosedWithoutAnswer = false;
	if (handshake(sock, addr, &channel) == eLoginCall::Ok)
	{
		AuthReq req{};
		req.Type = eLoginType::AuthReq;
		copyField(req.AccountID, accountID);
		copyField(req.Password, password);
		std::string sealed;
		const bool bSealed = channel.Seal(&req, sizeof(req), &sealed);
		SecureZeroMemory(req.Password, sizeof(req.Password));
		if (bSealed)
		{
			sealed[RSA_MODULUS_LEN / 2] ^= 1; // 잠근 요청 가운데 한 바이트를 바꾼다 - 클라 서명이 맞지 않아야 한다
			std::string reply;
			bClosedWithoutAnswer = sendPacket(sock, sealed.data(), sealed.size()) && !recvPacket(sock, &reply);
		}
	}
	closesocket(sock);
	return bClosedWithoutAnswer;
}

// 서버가 끊을 때까지 기다린다 - 끊기면 걸린 ms, 무엇이든 받았거나 시간 초과(LOGIN_TIMEOUT_MS)면 -1
static int32_t waitServerClose(const SOCKET sock)
{
	const ULONGLONG start = GetTickCount64();
	char byte = 0;
	const int ret = recv(sock, &byte, 1, 0);
	const bool bClosed = (ret == 0) || (ret == SOCKET_ERROR && WSAGetLastError() != WSAETIMEDOUT); // FIN 또는 RST
	return bClosed ? static_cast<int32_t>(GetTickCount64() - start) : -1;
}

static bool sendRegisterReq(const SOCKET sock, LoginChannel& channel, const std::string& accountID, const std::string& password)
{
	RegisterReq req{};
	req.Type = eLoginType::RegisterReq;
	copyField(req.AccountID, accountID);
	copyField(req.Password, password);
	const bool bSent = sendSecure(sock, channel, &req, sizeof(req));
	SecureZeroMemory(req.Password, sizeof(req.Password));
	return bSent;
}

static bool sendAuthReq(const SOCKET sock, LoginChannel& channel, const std::string& accountID, const std::string& password)
{
	AuthReq req{};
	req.Type = eLoginType::AuthReq;
	copyField(req.AccountID, accountID);
	copyField(req.Password, password);
	const bool bSent = sendSecure(sock, channel, &req, sizeof(req));
	SecureZeroMemory(req.Password, sizeof(req.Password));
	return bSent;
}

// 첫 요청(가입 또는 로그인)을 보내고 성공 응답을 받았는지
static bool succeedFirstRequest(const SOCKET sock, LoginChannel& channel, const std::string& accountID, const std::string& password, const eStateProbe probe)
{
	bool bOk = false;
	if (probe == eStateProbe::RegisterTwice)
	{
		RegisterRes res{};
		bOk = sendRegisterReq(sock, channel, accountID, password) && recvSecure(sock, channel, &res, sizeof(res)) && res.Type == eLoginType::RegisterRes &&
			res.Result == eRegisterResult::Ok;
	}
	else
	{
		AuthRes res{};
		bOk = sendAuthReq(sock, channel, accountID, password) && recvSecure(sock, channel, &res, sizeof(res)) && res.Type == eLoginType::AuthRes && res.Result == eAuthResult::Ok;
		SecureZeroMemory(res.SessionKey, sizeof(res.SessionKey));
	}
	return bOk;
}

// 첫 요청이 성공한 뒤 보낼 것 - 아무것도 안 보내는 경우는 true
static bool sendProbe(const SOCKET sock, LoginChannel& channel, const std::string& accountID, const std::string& password, const eStateProbe probe)
{
	bool bSent = true;
	switch (probe)
	{
	case eStateProbe::AuthThenAck:
	{
		const AuthAck ack{ eLoginType::AuthAck };
		bSent = sendSecure(sock, channel, &ack, sizeof(ack));
		break;
	}
	case eStateProbe::AuthThenNothing:
		break;
	case eStateProbe::AuthThenAuth:
		bSent = sendAuthReq(sock, channel, accountID, password);
		break;
	case eStateProbe::RegisterTwice:
		bSent = sendRegisterReq(sock, channel, accountID + "x", password);
		break;
	default:
		Assert(false, "sendProbe: 모르는 eStateProbe");
		break;
	}
	return bSent;
}

int32_t ProbeLoginState(const LoginServerAddr& addr, const std::string& accountID, const std::string& password, const eStateProbe probe)
{
	const SOCKET sock = connectTo(addr);
	int32_t closedMs = -1;
	if (sock != INVALID_SOCKET)
	{
		LoginChannel channel;
		if (handshake(sock, addr, &channel) == eLoginCall::Ok && succeedFirstRequest(sock, channel, accountID, password, probe) &&
			sendProbe(sock, channel, accountID, password, probe))
		{
			closedMs = waitServerClose(sock);
		}
		closesocket(sock);
	}
	return closedMs;
}

// HelloServer를 보내고 HelloClient를 받기만 한다(확인은 부르는 쪽이)
static bool exchangeHello(const SOCKET sock, LoginChannel* outChannel, HelloClient* outHello)
{
	Assert(outChannel != nullptr && outHello != nullptr, "exchangeHello: out 매개변수가 null");
	HelloServer hello{};
	std::string payload;
	const bool bOk = outChannel->StartClient(&hello) && sendPacket(sock, &hello, sizeof(hello)) && recvPacket(sock, &payload) && payload.size() == sizeof(HelloClient);
	if (bOk)
	{
		MemCpy(outHello, payload.data(), sizeof(*outHello));
	}
	return bOk;
}

LoginSignatureCheck CheckLoginSignatures(const LoginServerAddr& addr, const std::string& accountID)
{
	LoginSignatureCheck result;
	const SOCKET sockA = connectTo(addr);
	const SOCKET sockB = connectTo(addr);
	LoginChannel channelA;
	LoginChannel channelB;
	HelloClient helloA{};
	HelloClient helloB{};
	if (sockA != INVALID_SOCKET && sockB != INVALID_SOCKET && exchangeHello(sockA, &channelA, &helloA) && exchangeHello(sockB, &channelB, &helloB))
	{
		HelloClient tampered = helloA;
		tampered.Signature[RSA_SIGN_LEN / 2] ^= 1;
		result.bTamperedHelloRejected = !channelA.FinishClient(tampered, addr.SignPublicKey);
		result.bOtherHelloRejected = !channelB.FinishClient(helloA, addr.SignPublicKey);
		if (channelA.FinishClient(helloA, addr.SignPublicKey))
		{
			// 요청 둘을 보내고 응답 둘을 잠근 채로 받는다 - 비밀번호가 짧은 가입 요청(거절 응답, 연결은 Ready 그대로)
			// 로그인 요청은 틀리면 그 연결에서 다음 요청을 받지 않아(FailSent) 응답 둘을 받을 수 없다
			std::string responses[2];
			bool bGot = true;
			for (std::string& response : responses)
			{
				RegisterReq req{};
				req.Type = eLoginType::RegisterReq;
				copyField(req.AccountID, accountID);
				copyField(req.Password, std::string("x")); // PASSWORD_MIN_LEN보다 짧다 - 가입되지 않는다
				bGot = bGot && sendSecure(sockA, channelA, &req, sizeof(req)) && recvPacket(sockA, &response);
				SecureZeroMemory(req.Password, sizeof(req.Password));
			}
			std::string plain;
			if (bGot && channelA.Open(responses[0].data(), responses[0].size(), &plain))
			{
				result.bConnected = true;
				result.bReplayRejected = !channelA.Open(responses[0].data(), responses[0].size(), &plain); // 1번 자리에 0번 응답
				result.bNextAccepted = channelA.Open(responses[1].data(), responses[1].size(), &plain);
			}
			SecureZeroMemory(plain.data(), plain.size());
		}
	}
	if (sockA != INVALID_SOCKET)
	{
		closesocket(sockA);
	}
	if (sockB != INVALID_SOCKET)
	{
		closesocket(sockB);
	}
	return result;
}
