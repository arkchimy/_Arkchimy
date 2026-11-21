#include <WinSock2.h>
#include <WS2tcpip.h>
#include <atomic>
#include <conio.h>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include "GameProtocol.h"
#include "LoginClient.h"
#include "PacketHeader.h"
#include "LoginCrypto.h"
#include "SecureChannel.h"
#include "SessionAuth.h"

#pragma comment(lib, "ws2_32.lib")

// 사람이 직접 하는 콘솔 클라 - 수신 스레드가 서버 메시지를 언제든 출력하고, main은 명령을 입력받아 보낸다
// (다른 사람의 채팅처럼 내가 요청하지 않은 메시지도 오므로 입력과 수신을 나눈다)

static std::mutex sPrintLock; // 수신 스레드와 main이 함께 화면에 쓴다 - 줄이 섞이지 않게
// 채팅(UTF-8)을 콘솔 코드 페이지 문자열로 - 그대로 printf하면 한글 콘솔에서 깨진다
static std::string toConsole(const std::string& utf8)
{
	const int wideLen = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
	std::wstring wide(static_cast<size_t>(wideLen), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, wide.data(), wideLen);
	const int len = WideCharToMultiByte(CP_ACP, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
	std::string out(static_cast<size_t>(len), '\0');
	WideCharToMultiByte(CP_ACP, 0, wide.c_str(), -1, out.data(), len, nullptr, nullptr);
	out.resize(static_cast<size_t>(len > 0 ? len - 1 : 0)); // 끝의 널 문자 제외
	return out;
}

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

// 게임 서버와의 통일 모양 패킷(설계 게임서버_입장_변경.docx 2판) - ① Hello(평문) ② Challenge(평문, SeqNumber 시작값) ③ 입장 요청부터 모두 통일 모양
// 보내는 키(sTxCipher)와 받는 키(sRxCipher)는 같은 세션 키·같은 시작값 - 각자 한 방향의 SeqNumber만 센다. 소켓 송신은 sSendLock으로 묶는다
// (보내는 쪽: main의 명령과, Challenge를 받은 수신 스레드의 입장 요청)
enum class eClientEntry : uint8_t
{
	None = 0,          // 입장 시도 전
	WaitChallenge = 1, // Hello를 보냄
	WaitRes = 2,       // 입장 요청을 보냄
	Entered = 3,
};
static std::mutex sSendLock;
static std::mutex sEnterLock;                  // 아래 입장 준비 값 - main(login)이 쓰고 수신 스레드(Challenge)가 읽는다
static std::atomic<eClientEntry> sEntry = eClientEntry::None;
static uint8_t sPendingKey[SESSION_KEY_LEN] = {};
static uint64_t sPendingAccountNo = 0;
static std::string sPendingMode;               // 시험용 입장 요청 변형(enter 명령)
static ChannelCipher sTxCipher;
static ChannelCipher sRxCipher;
static std::string sLastSent;                  // 시험용 - 마지막으로 보낸 통일 모양 패킷(replaylast)
static std::string sLastReceived;              // 시험용 - 마지막으로 받은 서버 패킷(reflect)

// 라이브러리 헤더(길이 2바이트)를 붙여 그대로 보낸다 - Hello, 입장 요청, 시험용 바이트
static bool sendRaw(const SOCKET sock, const void* payload, const size_t len)
{
	if (len > MAX_PAYLOAD_SIZE)
	{
		return false;
	}
	char buf[PACKET_HEADER_SIZE + MAX_PAYLOAD_SIZE];
	const PacketHeader header{ static_cast<uint16_t>(len) };
	memcpy(buf, &header, PACKET_HEADER_SIZE);
	memcpy(buf + PACKET_HEADER_SIZE, payload, len);
	std::lock_guard<std::mutex> lock(sSendLock);
	return sendAll(sock, buf, static_cast<int>(PACKET_HEADER_SIZE + len));
}

// 입장 뒤 메시지 - 통일 모양으로 잠가 보낸다(보내는 SeqNumber +1). 시험용으로 마지막 패킷을 남긴다
static bool sendSealed(const SOCKET sock, const eGameType type, const void* body, const size_t bodyLen)
{
	std::string packet;
	{
		std::lock_guard<std::mutex> lock(sEnterLock); // 보내는 키는 main만 쓰지만, 입장 요청을 만드는 수신 스레드와 키를 정하는 순간이 겹치지 않게
		sTxCipher.SealPacket(type, body, bodyLen, &packet);
		sLastSent = packet;
	}
	return sendRaw(sock, packet.data(), packet.size());
}

// 채팅 한 줄 - 본문 = 글자. textLen은 시험용으로 범위 밖(0, CHAT_TEXT_MAX + 1)도 보낼 수 있다(서버가 끊어야 한다)
// 입장 전이면 [ChatReq | 글자]를 평문으로 그대로 보낸다 - 서버가 잘못된 패킷으로 끊는지 보는 시험(login.ps1)
static bool sendChat(const SOCKET sock, const std::string& text, const size_t textLen)
{
	const size_t cnt = (textLen < CHAT_TEXT_MAX + 1) ? textLen : CHAT_TEXT_MAX + 1;
	const std::string body = text.substr(0, cnt) + std::string(cnt - ((text.size() < cnt) ? text.size() : cnt), 'a');
	if (sEntry == eClientEntry::Entered)
	{
		return sendSealed(sock, eGameType::ChatReq, body.data(), body.size());
	}
	char plain[sizeof(eGameType) + CHAT_TEXT_MAX + 1];
	const eGameType type = eGameType::ChatReq;
	memcpy(plain, &type, sizeof(type));
	memcpy(plain + sizeof(type), body.data(), body.size());
	return sendRaw(sock, plain, sizeof(type) + body.size());
}

// 입장 요청 패킷을 만든다(키는 cipher에 정해진다). mode는 시험용 변형:
// bad = 태그 한 비트 바꿈, seqbad = SeqNumber 칸을 +1(받은 값과 다름), bodybad = 본문 AccountNo를 Hello와 다르게
static void makeEnterPacket(const uint8_t (&key)[SESSION_KEY_LEN], const uint64_t startSeq, const uint64_t accountNo, const std::string& mode,
	ChannelCipher* outCipher, std::string* outPacket)
{
	MakeEnterRequest(key, startSeq, (mode == "bodybad") ? accountNo + 1 : accountNo, outCipher, outPacket);
	if (mode == "bad")
	{
		(*outPacket)[outPacket->size() - 1] ^= 1;
	}
	else if (mode == "seqbad")
	{
		PacketHead head;
		memcpy(&head, outPacket->data(), sizeof(head));
		++head.Seq;
		memcpy(&(*outPacket)[0], &head, sizeof(head));
	}
}

// ① 입장 시작 - 세션 키와 AccountNo를 적어 두고 Hello를 보낸다. 입장 요청은 수신 스레드가 Challenge를 받은 뒤 보낸다(onChallenge)
static void beginEnter(const SOCKET sock, const uint8_t (&sessionKey)[SESSION_KEY_LEN], const uint64_t accountNo, const std::string& mode)
{
	{
		std::lock_guard<std::mutex> lock(sEnterLock);
		memcpy(sPendingKey, sessionKey, sizeof(sPendingKey));
		sPendingAccountNo = accountNo;
		sPendingMode = mode;
		sEntry = eClientEntry::WaitChallenge;
	}
	const Hello hello{ eGameType::Hello, accountNo };
	sendRaw(sock, &hello, sizeof(hello));
}

// ③ 수신 스레드: Challenge를 받으면 두 키를 시작값으로 정하고 입장 요청을 보낸다(mode가 twice면 같은 패킷을 두 번 - 입장 처리 중 또 보내기 시험)
static void onChallenge(const SOCKET sock, const Challenge& challenge)
{
	std::string packet;
	std::string mode;
	{
		std::lock_guard<std::mutex> lock(sEnterLock);
		makeEnterPacket(sPendingKey, challenge.StartSeq, sPendingAccountNo, sPendingMode, &sTxCipher, &packet);
		sRxCipher.SetKey(sPendingKey, false, challenge.StartSeq);
		SecureZeroMemory(sPendingKey, sizeof(sPendingKey)); // 키는 두 통로 키에만 남긴다
		mode = sPendingMode;
		sEntry = eClientEntry::WaitRes;
	}
	sendRaw(sock, packet.data(), packet.size());
	if (mode == "twice")
	{
		sendRaw(sock, packet.data(), packet.size());
	}
}
static const char* toString(const eLoginResult result)
{
	switch (result)
	{
	case eLoginResult::Ok:
		return "성공";
	case eLoginResult::AuthFail:
		return "입장 확인 실패(세션 키가 없거나 30초 지남) - 다시 login 하세요";
	case eLoginResult::Banned:
		return "운영자가 로그인을 막은 아이디입니다";
	case eLoginResult::DbError:
		return "계정을 읽지 못했습니다(DB 오류) - 잠시 뒤 다시 로그인하세요";
	default:
		return "알 수 없는 결과";
	}
}

static const char* toString(const eRegisterResult result)
{
	switch (result)
	{
	case eRegisterResult::Ok:
		return "가입했습니다 - 이제 login <아이디> <비밀번호>";
	case eRegisterResult::BadID:
		return "잘못된 아이디(비었거나 16바이트 이상)";
	case eRegisterResult::BadPassword:
		return "비밀번호는 8~64바이트";
	case eRegisterResult::Exists:
		return "이미 있는 아이디";
	case eRegisterResult::ServerError:
		return "서버 오류 - 잠시 뒤 다시";
	default:
		return "알 수 없는 결과";
	}
}

static const char* toString(const eAuthResult result)
{
	switch (result)
	{
	case eAuthResult::Ok:
		return "성공";
	case eAuthResult::Fail:
		return "아이디 또는 비밀번호가 틀립니다";
	case eAuthResult::ServerError:
		return "서버 오류 - 잠시 뒤 다시";
	default:
		return "알 수 없는 결과";
	}
}

static const char* toString(const eLoginCall call)
{
	switch (call)
	{
	case eLoginCall::Ok:
		return "성공";
	case eLoginCall::NoConnection:
		return "로그인 서버에 접속하지 못했습니다";
	case eLoginCall::BadServer:
		return "서버 확인 실패 - 서명이 서버 공개 키와 맞지 않아 비밀번호를 보내지 않았습니다";
	case eLoginCall::Broken:
		return "로그인 서버와 주고받다 끊겼습니다";
	default:
		return "알 수 없는 결과";
	}
}

// 고정 길이 아이디 칸 → 문자열 (널이 없어도 칸 밖을 읽지 않게)
static std::string toID(const char (&id)[ACCOUNT_ID_LEN])
{
	return std::string(id, strnlen(id, ACCOUNT_ID_LEN));
}

// 콘솔 입력 → UTF-8. 콘솔이 UTF-8이면(리다이렉트 등) 이미 UTF-8이라 그대로 - 다시 바꾸면 두 번 인코딩돼 깨진다
// CP949 한글 바이트열은 거의 항상 올바른 UTF-8이 아니어서 이것으로 가린다(운영 툴과 같은 방식)
static std::string toUtf8(const std::string& text)
{
	const bool bValidUtf8 = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0) > 0;
	if (text.empty() || bValidUtf8)
	{
		return text;
	}
	const int wideLen = MultiByteToWideChar(CP_ACP, 0, text.c_str(), -1, nullptr, 0);
	std::wstring wide(static_cast<size_t>(wideLen), L'\0');
	MultiByteToWideChar(CP_ACP, 0, text.c_str(), -1, wide.data(), wideLen);
	const int len = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
	std::string out(static_cast<size_t>(len), '\0');
	WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, out.data(), len, nullptr, nullptr);
	out.resize(static_cast<size_t>(len > 0 ? len - 1 : 0));
	return out;
}

static void printPacket(const eGameType type, const std::string& body)
{
	std::lock_guard<std::mutex> lock(sPrintLock); // 모든 출력을 락 안에서 - main의 출력과 줄이 섞이지 않게
	const int chatTextLen = static_cast<int>(body.size()) - static_cast<int>(offsetof(ChatNotifyBody, Text));
	if (type == eGameType::ChatNotify && chatTextLen >= 1 && chatTextLen <= CHAT_TEXT_MAX)
	{
		ChatNotifyBody notify;
		memcpy(&notify, body.data(), body.size());
		printf("[채팅] %s: %s\n", toID(notify.SenderID).c_str(), toConsole(std::string(notify.Text, static_cast<size_t>(chatTextLen))).c_str());
	}
	else
	{
		printf("[수신] 모르는 패킷 type=%u len=%zu\n", static_cast<unsigned>(type), body.size());
	}
}

static std::atomic<bool> sbQuitting = false; // quit으로 내가 끊는 중 - 이때는 "연결 끊김"을 출력하지 않는다

// 받은 패킷 하나를 처리한다 - false면 연결을 끝낸다
static bool handleServerPacket(const SOCKET sock, const char* payload, const uint16_t len)
{
	const eClientEntry entry = sEntry;
	if (entry == eClientEntry::WaitChallenge && len == sizeof(Challenge))
	{
		Challenge challenge;
		memcpy(&challenge, payload, sizeof(challenge));
		if (challenge.Type == eGameType::Challenge)
		{
			onChallenge(sock, challenge);
			return true;
		}
	}
	else if (entry == eClientEntry::WaitRes || entry == eClientEntry::Entered)
	{
		eGameType type = eGameType::Hello;
		std::string body;
		const bool bOpened = sRxCipher.OpenPacket(payload, len, &type, &body); // 받는 키는 이 스레드만 - SeqNumber·태그 확인
		if (!bOpened)
		{
			std::lock_guard<std::mutex> lock(sPrintLock);
			printf("[암호] 서버 메시지를 풀지 못했습니다(SeqNumber 또는 태그가 틀림)\n");
			return false;
		}
		{
			std::lock_guard<std::mutex> lock(sEnterLock);
			sLastReceived.assign(payload, len);
		}
		if (entry == eClientEntry::WaitRes && type == eGameType::LoginRes && body.size() == sizeof(LoginResBody))
		{
			LoginResBody res;
			memcpy(&res, body.data(), sizeof(res));
			sEntry = eClientEntry::Entered; // main은 Entered를 본 뒤에만 보내는 키로 잠근다
			std::lock_guard<std::mutex> lock(sPrintLock);
			printf("[로그인] %s\n", toString(res.Result));
			return true;
		}
		if (entry == eClientEntry::Entered)
		{
			printPacket(type, body);
			return true;
		}
	}
	std::lock_guard<std::mutex> lock(sPrintLock);
	printf("[암호] 지금 상태에서 올 수 없는 패킷(길이 %u)을 버렸습니다\n", static_cast<unsigned>(len));
	return true;
}

static void runRecvLoop(const SOCKET sock, std::atomic<bool>* outConnected)
{
	char payload[MAX_PAYLOAD_SIZE];
	while (true)
	{
		PacketHeader header;
		if (!recvAll(sock, reinterpret_cast<char*>(&header), PACKET_HEADER_SIZE) || header.Len > MAX_PAYLOAD_SIZE
			|| !recvAll(sock, payload, header.Len) || !handleServerPacket(sock, payload, header.Len))
		{
			break;
		}
	}
	*outConnected = false;
	const eClientEntry entry = sEntry;
	if ((entry == eClientEntry::WaitChallenge || entry == eClientEntry::WaitRes) && !sbQuitting)
	{
		// 입장에 실패하면 서버는 응답 없이 끊는다(세션 키가 없거나 틀림·SeqNumber가 다름·차단·DB 오류)
		std::lock_guard<std::mutex> lock(sPrintLock);
		printf("[로그인] 입장 실패 - 서버가 연결을 끊었습니다(세션 키가 없거나 30초 지남, 확인 값이 틀림, 차단). 다시 실행해 login 하세요\n");
	}
	if (!sbQuitting)
	{
		std::lock_guard<std::mutex> lock(sPrintLock);
		printf("[연결 끊김] 서버가 연결을 끊었습니다. Enter를 누르면 종료합니다\n");
	}
}
static void printHelp()
{
	std::lock_guard<std::mutex> lock(sPrintLock);
	printf("명령: register <아이디> [비밀번호] | login <아이디> [비밀번호] (비밀번호를 빼면 시험용 기본값)\n");
	printf("      chat <말> (방 하나 - 입장한 모두에게) | help | quit\n");
}
int main(int argc, char* argv[])
{
	const char* ip = (argc > 1) ? argv[1] : "127.0.0.1";
	const int port = (argc > 2) ? atoi(argv[2]) : 33000;
	LoginServerAddr loginAddr; // 로그인 서버 - 같은 IP
	loginAddr.IP = ip;
	loginAddr.Port = (argc > 3) ? atoi(argv[3]) : 33002;
	if (!InitCrypto())
	{
		printf("libsodium 초기화 실패\n");
		return 1;
	}
	// 서버 고정 공개 키 - 클라 배포물에 들어 있는 값(로그인 서버가 진짜인지 확인한다). 없으면 로그인할 수 없다
	if (!LoadServerPublicKey("server_public.cfg", loginAddr.SignPublicKey))
	{
		printf("[주의] server_public.cfg(serverPublicKey)를 읽지 못해 로그인 서버를 확인할 수 없습니다 - 로그인이 실패합니다\n");
	}

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
	printf("접속 %s:%d - 처음이면 register <아이디> <비밀번호>, 그다음 login <아이디> <비밀번호>(로그인 서버 %d)\n", ip, port, loginAddr.Port);
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
		else if (cmd == "auth")
		{
			// 시험용: 로그인 서버에서 세션 키만 받아 보여 준다(게임 서버에는 보내지 않음) - enter로 다른 클라가 같은 키를 써 보는 시험
			std::string id;
			std::string password;
			in >> id >> password;
			AuthRes auth{};
			const bool bSent = RequestAuth(loginAddr, id, password.empty() ? "test-password" : password, &auth) == eLoginCall::Ok;
			char keyHex[SESSION_KEY_LEN * 2 + 1] = {};
			for (int32_t i = 0; i < SESSION_KEY_LEN; ++i)
			{
				snprintf(keyHex + i * 2, 3, "%02x", auth.SessionKey[i]);
			}
			std::lock_guard<std::mutex> lock(sPrintLock);
			printf(bSent && auth.Result == eAuthResult::Ok ? "[세션 키] no=%llu key=%s\n" : "[로그인 서버] 실패\n", static_cast<unsigned long long>(auth.AccountNo), keyHex);
		}
		else if (cmd == "authrepeat" || cmd == "authbad")
		{
			// 시험용: authrepeat <아이디> <비밀번호> <횟수> - 한 연결에서 여러 번 로그인해 받은 응답 수(5번 틀리면 끊김)
			//         authbad <아이디> - 암호문 한 바이트를 바꿔 보낸다(서버가 응답 없이 끊어야 함)
			std::string id;
			std::string password;
			int count = 0;
			in >> id >> password >> count;
			std::lock_guard<std::mutex> lock(sPrintLock);
			if (cmd == "authrepeat")
			{
				printf("[반복 로그인] 응답 %d번\n", RequestAuthRepeat(loginAddr, id, password, count));
			}
			else
			{
				printf("[변조] %s\n", RequestAuthTampered(loginAddr, id, password.empty() ? "test-password" : password) ? "서버가 응답 없이 끊음" : "응답을 받음");
			}
		}
		else if (cmd == "stateprobe")
		{
			// 시험용: stateprobe <ack|nothing|authagain|regtwice> <아이디> [비밀번호] - 로그인 서버 연결 상태(로그인 성공 뒤 확인·다시 요청, 가입 두 번)
			// 서버가 끊을 때까지 걸린 시간을 찍는다. regtwice는 처음 보는 아이디여야 한다(가입이 성공해야 하므로)
			std::string mode;
			std::string id;
			std::string password;
			in >> mode >> id >> password;
			const std::string pw = password.empty() ? "test-password" : password;
			eStateProbe probe = eStateProbe::AuthThenAck;
			bool bKnown = true;
			if (mode == "nothing")
			{
				probe = eStateProbe::AuthThenNothing;
			}
			else if (mode == "authagain")
			{
				probe = eStateProbe::AuthThenAuth;
			}
			else if (mode == "regtwice")
			{
				probe = eStateProbe::RegisterTwice;
			}
			else if (mode != "ack")
			{
				bKnown = false;
			}
			const int32_t closedMs = bKnown ? ProbeLoginState(loginAddr, id, pw, probe) : -1;
			std::lock_guard<std::mutex> lock(sPrintLock);
			if (closedMs >= 0)
			{
				printf("[상태 시험] %s 서버가 끊음 %dms\n", mode.c_str(), closedMs);
			}
			else
			{
				printf("[상태 시험] %s 실패(첫 요청 실패, 응답을 받음, 또는 끊지 않음)\n", mode.c_str());
			}
		}
		else if (cmd == "rsacheck")
		{
			// 시험용: rsacheck <아이디> [비밀번호] - 클라가 서명을 제대로 확인하는지 4가지(설계 login_rsa_change.docx 2-6)
			std::string id;
			std::string password;
			in >> id >> password;
			const LoginSignatureCheck check = CheckLoginSignatures(loginAddr, id, password.empty() ? "test-password" : password);
			std::lock_guard<std::mutex> lock(sPrintLock);
			printf("[서명 확인] 연결=%s 바꾼ServerHello=%s 다른연결ServerHello=%s 앞응답다시=%s 다음응답=%s\n", check.bConnected ? "됨" : "실패",
				check.bTamperedHelloRejected ? "거절" : "통과", check.bOtherHelloRejected ? "거절" : "통과", check.bReplayRejected ? "거절" : "통과",
				check.bNextAccepted ? "통과" : "거절");
		}
		else if (cmd == "tamper" || cmd == "replaylast" || cmd == "reflect" || cmd == "typeflip" || cmd == "seqskip" || cmd == "plain" || cmd == "loginagain")
		{
			// 시험용(입장 뒤 메시지, 테스트 케이스 3판 3-5·R-04) - 서버가 모두 끊어야 한다
			// tamper: 채팅 암호문 한 비트 바꿈 / replaylast: 마지막으로 보낸 패킷을 그대로 다시 / reflect: 마지막으로 받은 서버 패킷을 되돌려 보냄
			// typeflip: 채팅의 평문 type만 바꿈 / seqskip: SeqNumber 하나를 건너뛰고 보냄 / plain: 평문 2바이트 / loginagain: 입장 뒤 입장 요청을 통로로
			if (sEntry != eClientEntry::Entered)
			{
				std::lock_guard<std::mutex> lock(sPrintLock);
				printf("[시험] 입장 전이라 할 수 없습니다\n");
				continue;
			}
			std::string packet;
			if (cmd == "replaylast" || cmd == "reflect")
			{
				std::lock_guard<std::mutex> lock(sEnterLock);
				packet = (cmd == "replaylast") ? sLastSent : sLastReceived;
			}
			else if (cmd == "plain")
			{
				packet = std::string("\x05\x00", 2);
			}
			else if (cmd == "loginagain")
			{
				const LoginReqBody body{ 1 };
				sendSealed(sock, eGameType::LoginReq, &body, sizeof(body));
				continue;
			}
			else
			{
				std::lock_guard<std::mutex> lock(sEnterLock);
				if (cmd == "seqskip")
				{
					std::string skipped;
					sTxCipher.SealPacket(eGameType::ChatReq, "x", 1, &skipped); // 보내지 않는다 - 서버가 기다리는 번호를 건너뛴다
				}
				sTxCipher.SealPacket(eGameType::ChatReq, "x", 1, &packet);
				if (cmd == "tamper")
				{
					packet[PACKET_HEAD_LEN] ^= 1; // 암호문 첫 바이트
				}
				else if (cmd == "typeflip")
				{
					const eGameType other = eGameType::ChatNotify;
					memcpy(&packet[0], &other, sizeof(other)); // 평문 type(AD)만 바꾼다
				}
			}
			sendRaw(sock, packet.data(), packet.size());
		}		else if (cmd == "pinkey")
		{
			// 시험용: pinkey <16진수 512글자 | random> - 미리 가진 서버 공개 키를 바꾼다(틀린 키면 로그인 서버를 가짜로 보고 비밀번호를 보내지 않아야 함)
			// random은 새로 만든 진짜 RSA 공개 키 - 서명 확인 자체가 틀리는 경우를 시험한다
			std::string hex;
			in >> hex;
			bool bOk = false;
			if (hex == "random")
			{
				RsaKey other;
				bOk = other.Generate() && other.ExportPublic(loginAddr.SignPublicKey);
			}
			else
			{
				bOk = HexToBytes(hex, loginAddr.SignPublicKey, sizeof(loginAddr.SignPublicKey));
			}
			std::lock_guard<std::mutex> lock(sPrintLock);
			printf("[서버 키] %s\n", bOk ? "바꿈" : "16진수 512글자가 아님");
		}
		else if (cmd == "enter")
		{
			// 시험용: enter <AccountNo> <키 16진수> [bad|seqbad|bodybad|twice] - 받은 키로 Hello → Challenge → 입장 요청. 변형은 makeEnterPacket·onChallenge
			uint64_t accountNo = 0;
			std::string keyHex;
			std::string mode;
			in >> accountNo >> keyHex >> mode;
			uint8_t key[SESSION_KEY_LEN] = {};
			HexToBytes(keyHex, key, sizeof(key));
			beginEnter(sock, key, accountNo, mode);
			SecureZeroMemory(key, sizeof(key));
		}
		else if (cmd == "enterdump")
		{
			// 시험용: enterdump <AccountNo> <키 16진수> <SeqNumber 시작값(10진수)> [bad|seqbad|bodybad] - 이 값으로 만든 입장 요청(헤더 포함)을 보내지 않고 16진수로
			// 시험 스크립트가 직접 연 연결에서 받은 시작값으로 입장 요청을 만들고, 다른 연결에서 다시 보내는 리플레이 시험(테스트 케이스 3판 3-3)에 쓴다
			uint64_t accountNo = 0;
			std::string keyHex;
			uint64_t startSeq = 0;
			std::string mode;
			in >> accountNo >> keyHex >> startSeq >> mode;
			uint8_t key[SESSION_KEY_LEN] = {};
			HexToBytes(keyHex, key, sizeof(key));
			ChannelCipher dumpCipher; // 이 연결의 통로와 따로 - 출력만 한다
			std::string packet;
			makeEnterPacket(key, startSeq, accountNo, mode, &dumpCipher, &packet);
			SecureZeroMemory(key, sizeof(key));
			const PacketHeader header{ static_cast<uint16_t>(packet.size()) };
			std::string hex;
			char byteHex[3];
			for (size_t i = 0; i < PACKET_HEADER_SIZE + packet.size(); ++i)
			{
				const uint8_t b = (i < PACKET_HEADER_SIZE) ? reinterpret_cast<const uint8_t*>(&header)[i] : static_cast<uint8_t>(packet[i - PACKET_HEADER_SIZE]);
				snprintf(byteHex, sizeof(byteHex), "%02x", b);
				hex += byteHex;
			}
			std::lock_guard<std::mutex> lock(sPrintLock);
			printf("[입장 패킷] %s\n", hex.c_str());
		}		else if (cmd == "authflood")
		{
			// 시험용: authflood <개수> <아이디> [비밀번호] - 스레드 n개가 동시에 로그인 서버에 로그인해 결과별 개수와 걸린 시간을 출력한다(TC-L10 인증 대기열 상한)
			int count = 0;
			std::string id;
			std::string password;
			in >> count >> id >> password;
			if (password.empty())
			{
				password = "test-password";
			}
			std::atomic<int> okCnt = 0;
			std::atomic<int> failCnt = 0;
			std::atomic<int> serverErrorCnt = 0;
			std::atomic<int> brokenCnt = 0;
			std::atomic<int> fastServerErrorCnt = 0; // 1초 안에 온 서버 오류 - 대기열 상한으로 해시 없이 거절된 몫
			std::vector<std::thread> threads;
			const ULONGLONG start = GetTickCount64();
			for (int i = 0; i < count; ++i)
			{
				threads.emplace_back([&]()
				{
					const ULONGLONG begin = GetTickCount64();
					AuthRes auth{};
					const eLoginCall call = RequestAuth(loginAddr, id, password, &auth);
					SecureZeroMemory(auth.SessionKey, sizeof(auth.SessionKey));
					if (call != eLoginCall::Ok)
					{
						++brokenCnt;
					}
					else if (auth.Result == eAuthResult::Ok)
					{
						++okCnt;
					}
					else if (auth.Result == eAuthResult::Fail)
					{
						++failCnt;
					}
					else
					{
						++serverErrorCnt;
						if (GetTickCount64() - begin < 1000)
						{
							++fastServerErrorCnt;
						}
					}
				});
			}
			for (std::thread& t : threads)
			{
				t.join();
			}
			std::lock_guard<std::mutex> lock(sPrintLock);
			printf("[동시 로그인] 성공=%d 실패=%d 서버오류=%d 1초안서버오류=%d 끊김=%d 걸린시간=%llums\n", okCnt.load(), failCnt.load(), serverErrorCnt.load(), fastServerErrorCnt.load(),
				brokenCnt.load(), static_cast<unsigned long long>(GetTickCount64() - start));
		}
		else if (cmd == "helloflood")
		{
			// 시험용: helloflood <개수> <유지 초> - ClientHello만 보내고 아무것도 하지 않는 접속 n개를 연다(설계 login_rsa_change.docx 2-7 접속 폭주)
			// 공격자는 키 쌍을 하나만 만들어 모든 접속에 같은 공개 키를 보낸다 - 서버만 접속마다 키 쌍(수십 ms)을 만든다
			// 바로 돌아오고, 유지 시간이 지나면 ServerHello를 받은 접속·끊긴 접속 수를 출력한다(그사이 다른 클라로 정상 로그인 시간을 잰다)
			int count = 0;
			int holdSec = 0;
			in >> count >> holdSec;
			std::thread([count, holdSec, loginAddr]()
				{
					RsaKey key;
					ClientHello hello{};
					hello.Type = eLoginType::ClientHello;
					if (!key.Generate() || !key.ExportPublic(hello.ClientPublicKey))
					{
						return;
					}
					char buf[PACKET_HEADER_SIZE + sizeof(ClientHello)];
					const PacketHeader header{ static_cast<uint16_t>(sizeof(ClientHello)) };
					memcpy(buf, &header, PACKET_HEADER_SIZE);
					memcpy(buf + PACKET_HEADER_SIZE, &hello, sizeof(hello));
					SOCKADDR_IN addr{};
					addr.sin_family = AF_INET;
					addr.sin_port = htons(static_cast<u_short>(loginAddr.Port));
					inet_pton(AF_INET, loginAddr.IP.c_str(), &addr.sin_addr);
					std::vector<SOCKET> socks;
					const ULONGLONG start = GetTickCount64();
					for (int i = 0; i < count; ++i)
					{
						const SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
						if (connect(sock, reinterpret_cast<SOCKADDR*>(&addr), sizeof(addr)) == SOCKET_ERROR || !sendAll(sock, buf, sizeof(buf)))
						{
							closesocket(sock);
							continue;
						}
						socks.push_back(sock);
					}
					const ULONGLONG sendMs = GetTickCount64() - start;
					Sleep(static_cast<DWORD>(holdSec) * 1000);
					int answered = 0;
					int closed = 0;
					for (const SOCKET sock : socks)
					{
						u_long nonBlocking = 1;
						ioctlsocket(sock, FIONBIO, &nonBlocking);
						char reply[PACKET_HEADER_SIZE + sizeof(ServerHello)];
						const int ret = recv(sock, reply, sizeof(reply), MSG_PEEK);
						if (ret >= static_cast<int>(sizeof(reply)))
						{
							++answered; // 끊겼더라도 ServerHello가 먼저 와 있으면 받은 것으로 센다
						}
						else if (ret == 0 || (ret == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK))
						{
							++closed;
						}
						closesocket(sock);
					}
					std::lock_guard<std::mutex> lock(sPrintLock);
					printf("[접속 폭주] 접속=%zu ServerHello받음=%d 받기전끊김=%d 보내는데=%llums\n", socks.size(), answered, closed, static_cast<unsigned long long>(sendMs));
				}).detach();
		}
		else if (cmd == "register" || cmd == "login")
		{
			std::string id;
			std::string password;
			in >> id >> password;
			if (password.empty())
			{
				password = "test-password"; // 시험 스크립트용 기본값(시험 계정은 이 비밀번호로 만든다) - 사람이 쓸 때는 직접 적는다
			}
			if (cmd == "register")
			{
				eRegisterResult result = eRegisterResult::ServerError;
				const eLoginCall call = RequestRegister(loginAddr, id, password, &result);
				std::lock_guard<std::mutex> lock(sPrintLock);
				printf("[가입] %s\n", call == eLoginCall::Ok ? toString(result) : toString(call));
				continue;
			}
			// 로그인 서버에서 세션 키를 받고(별도 접속, 암호문), 그 키로 잠근 입장 요청을 게임 서버에 보낸다 - 키 자체는 게임 서버로 보내지 않는다
			AuthRes auth{};
			const eLoginCall call = RequestAuth(loginAddr, id, password, &auth);
			if (call != eLoginCall::Ok || auth.Result != eAuthResult::Ok)
			{
				std::lock_guard<std::mutex> lock(sPrintLock);
				printf("[로그인 서버] %s\n", call == eLoginCall::Ok ? toString(auth.Result) : toString(call));
				continue;
			}
			beginEnter(sock, auth.SessionKey, auth.AccountNo, ""); // Hello를 보내고, Challenge를 받으면 수신 스레드가 입장 요청을 보낸다. 여기의 사본은 지운다
			SecureZeroMemory(auth.SessionKey, sizeof(auth.SessionKey));
		}
		else if (cmd == "chat")
		{
			std::string text;
			std::getline(in, text);
			const size_t start = text.find_first_not_of(' ');
			text = (start == std::string::npos) ? std::string() : toUtf8(text.substr(start));
			if (text.empty() || text.size() > CHAT_TEXT_MAX)
			{
				std::lock_guard<std::mutex> lock(sPrintLock);
				printf("[채팅] 1~%d바이트(UTF-8)\n", CHAT_TEXT_MAX);
				continue;
			}
			sendChat(sock, text, text.size());
		}
		else if (cmd == "chatraw")
		{
			// 시험용: chatraw <바이트 수> - 'a'로 채운 채팅을 길이 검사 없이 보낸다(0이나 CHAT_TEXT_MAX + 1이면 서버가 잘못된 패킷으로 끊어야 한다)
			int cnt = 0;
			in >> cnt;
			sendChat(sock, std::string(CHAT_TEXT_MAX + 1, 'a'), static_cast<size_t>((cnt > 0) ? cnt : 0));
		}
		else if (cmd == "chatburst")
		{
			// 시험용: chatburst <개수> <접두어> - "<접두어> 0", "<접두어> 1" … 을 쉬지 않고 보낸다(받는 쪽에서 순서를 본다)
			int cnt = 0;
			std::string prefix;
			in >> cnt >> prefix;
			for (int i = 0; i < cnt; ++i)
			{
				const std::string text = prefix + " " + std::to_string(i);
				sendChat(sock, text, text.size());
			}
		}
		else
		{
			printHelp();
		}
	}
	sbQuitting = true;
	shutdown(sock, SD_BOTH); // 수신 스레드의 recv를 깨운다
	recvThread.join();       // 수신 스레드가 끝난 뒤에 닫는다 - 먼저 닫으면 수신 스레드가 닫힌(재사용될 수 있는) 핸들로 recv를 부를 수 있다
	closesocket(sock);
	WSACleanup();
	return 0;
}
