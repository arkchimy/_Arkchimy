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
#include "GameData.h"
#include "GameProtocol.h"
#include "PacketHeader.h"

#pragma comment(lib, "ws2_32.lib")

// 사람이 직접 하는 콘솔 클라 - 수신 스레드가 서버 메시지를 언제든 출력하고, main은 명령을 입력받아 보낸다
// (파티원 행동·경매 입찰 알림처럼 내가 요청하지 않은 메시지도 오므로 입력과 수신을 나눈다)

static std::mutex sPrintLock; // 수신 스레드와 main이 함께 화면에 쓴다 - 줄이 섞이지 않게
static GameData sGameData;    // 아이템·몬스터 이름 표시용 - 서버와 같은 Data 폴더의 CSV. 시작할 때 읽고 그 뒤로는 읽기만
static bool sbHasGameData = false;

// CSV의 이름(UTF-8)을 콘솔 코드 페이지 문자열로 - 그대로 printf하면 한글 콘솔에서 깨진다
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

static std::string itemName(const uint16_t kind)
{
	const ItemData* item = sbHasGameData ? sGameData.FindItem(kind) : nullptr;
	return (item != nullptr) ? toConsole(item->Name) : ("종류 " + std::to_string(kind));
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
		return "잘못된 아이디(비었거나 16바이트 이상)";
	case eLoginResult::AlreadyLoggedIn:
		return "이미 접속 중인 아이디 - 서버가 이전 접속을 끊었으니 잠시 뒤 다시 로그인하세요";
	default:
		return "알 수 없는 결과";
	}
}

static void printInventory(const InventoryRes& res)
{
	static const char* SLOT_NAMES[] = { "무기", "머리", "장신구", "갑옷" };
	static_assert(sizeof(SLOT_NAMES) / sizeof(SLOT_NAMES[0]) == static_cast<size_t>(eEquipSlot::Count), "장비 칸 이름 수가 eEquipSlot과 맞아야 한다");
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
		printf("  #%llu %s\n", static_cast<unsigned long long>(res.Items[i].ItemUID), itemName(res.Items[i].ItemKind).c_str());
	}
}

// 고정 길이 아이디 칸 → 문자열 (널이 없어도 칸 밖을 읽지 않게)
static std::string toID(const char (&id)[ACCOUNT_ID_LEN])
{
	return std::string(id, strnlen(id, ACCOUNT_ID_LEN));
}

static const char* toString(const ePartyState state)
{
	switch (state)
	{
	case ePartyState::None:
		return "파티 없음";
	case ePartyState::Recruiting:
		return "모집 중";
	case ePartyState::InDungeon:
		return "던전 진행 중";
	default:
		return "알 수 없음";
	}
}

static void printPartyInfo(const PartyInfoNotify& notify)
{
	if (notify.State == ePartyState::None)
	{
		printf("[파티] 파티에서 나왔습니다\n");
		return;
	}
	printf("[파티 #%llu] %s, %u명:", static_cast<unsigned long long>(notify.PartyID), toString(notify.State), notify.MemberCnt);
	for (int32_t i = 0; i < notify.MemberCnt && i < PARTY_MEMBER_MAX; ++i)
	{
		printf(" %s%s", toID(notify.MemberIDs[i]).c_str(), (i == notify.LeaderIdx) ? "(방장)" : "");
	}
	printf("\n");
}

static void printPartyList(const PartyListRes& res)
{
	printf("[파티 목록] 모집 중 %u개\n", res.PartyCnt);
	for (int32_t i = 0; i < res.PartyCnt && i < PARTY_LIST_MAX; ++i)
	{
		const PartyListEntry& entry = res.Entries[i];
		printf("  #%llu 방장 %s, %u/%d명\n", static_cast<unsigned long long>(entry.PartyID), toID(entry.LeaderID).c_str(), entry.MemberCnt, PARTY_MEMBER_MAX);
	}
}

static const char* toString(const eCreatePartyResult result)
{
	return (result == eCreatePartyResult::Ok) ? "성공" : "이미 파티에 있음";
}

static const char* toString(const eJoinPartyResult result)
{
	switch (result)
	{
	case eJoinPartyResult::Ok:
		return "성공";
	case eJoinPartyResult::NotFound:
		return "없는 파티";
	case eJoinPartyResult::Full:
		return "인원이 가득 참";
	case eJoinPartyResult::AlreadyStarted:
		return "이미 던전을 시작한 파티";
	case eJoinPartyResult::AlreadyInParty:
		return "이미 파티에 있음";
	default:
		return "알 수 없는 결과";
	}
}

static const char* toString(const eStartPartyResult result)
{
	switch (result)
	{
	case eStartPartyResult::Ok:
		return "성공";
	case eStartPartyResult::NotInParty:
		return "파티에 없음";
	case eStartPartyResult::NotLeader:
		return "방장만 시작할 수 있음";
	case eStartPartyResult::NotEnoughMembers:
		return "2명 이상이어야 시작할 수 있음";
	case eStartPartyResult::AlreadyStarted:
		return "이미 시작함";
	default:
		return "알 수 없는 결과";
	}
}

// 종류별로 길이가 맞는지 보고 구조체로 꺼낸다 - 맞지 않으면 false
template <typename T>
static bool readAs(const char* payload, const int len, T* outPacket)
{
	if (len != static_cast<int>(sizeof(T)))
	{
		return false;
	}
	memcpy(outPacket, payload, sizeof(T));
	return true;
}

static void printPacket(const char* payload, const int len)
{
	std::lock_guard<std::mutex> lock(sPrintLock); // 모든 출력을 락 안에서 - main의 출력과 줄이 섞이지 않게
	eGameType type;
	if (len < static_cast<int>(sizeof(type)))
	{
		printf("[수신] 종류도 없는 패킷 len=%d\n", len);
		return;
	}
	memcpy(&type, payload, sizeof(type));

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
	case eGameType::PartyCreateRes:
	{
		PartyCreateRes res;
		if (readAs(payload, len, &res))
		{
			printf("[파티 만들기] %s\n", toString(res.Result));
			return;
		}
		break;
	}
	case eGameType::PartyListRes:
	{
		PartyListRes res;
		if (readAs(payload, len, &res))
		{
			printPartyList(res);
			return;
		}
		break;
	}
	case eGameType::PartyJoinRes:
	{
		PartyJoinRes res;
		if (readAs(payload, len, &res))
		{
			printf("[파티 참가] %s\n", toString(res.Result));
			return;
		}
		break;
	}
	case eGameType::PartyStartRes:
	{
		PartyStartRes res;
		if (readAs(payload, len, &res))
		{
			printf("[던전 시작] %s\n", toString(res.Result));
			return;
		}
		break;
	}
	case eGameType::PartyInfoNotify:
	{
		PartyInfoNotify notify;
		if (readAs(payload, len, &notify))
		{
			printPartyInfo(notify);
			return;
		}
		break;
	}
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
	printf("명령: login <아이디> | inv | party create | party list | party join <번호> | party leave | party start | help | quit\n");
}

int main(int argc, char* argv[])
{
	const char* ip = (argc > 1) ? argv[1] : "127.0.0.1";
	const int port = (argc > 2) ? atoi(argv[2]) : 33000;

	std::string dataError;
	sbHasGameData = sGameData.Load("Data", &dataError);
	if (!sbHasGameData)
	{
		printf("[주의] 시나리오 데이터를 읽지 못해 아이템을 번호로 보여 줍니다 - %s\n", dataError.c_str());
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
	printf("접속 %s:%d - 먼저 login <아이디>로 로그인하세요(처음 쓰는 아이디면 계정이 만들어집니다)\n", ip, port);
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
			// 16바이트까지 그대로 넣는다 - 16바이트 이상이면 널 없이 가서 서버의 BadID 검사를 시험할 수 있다(req는 0으로 시작해 짧으면 널로 끝난다)
			memcpy(req.AccountID, id.data(), (id.size() < sizeof(req.AccountID)) ? id.size() : sizeof(req.AccountID));
			sendPacket(sock, req);
		}
		else if (cmd == "inv")
		{
			InventoryReq req;
			req.Type = eGameType::InventoryReq;
			sendPacket(sock, req);
		}
		else if (cmd == "party")
		{
			std::string sub;
			in >> sub;
			if (sub == "create")
			{
				const PartyCreateReq req{ eGameType::PartyCreateReq };
				sendPacket(sock, req);
			}
			else if (sub == "list")
			{
				const PartyListReq req{ eGameType::PartyListReq };
				sendPacket(sock, req);
			}
			else if (sub == "join")
			{
				uint64_t partyID = 0;
				in >> partyID;
				const PartyJoinReq req{ eGameType::PartyJoinReq, partyID };
				sendPacket(sock, req);
			}
			else if (sub == "leave")
			{
				const PartyLeaveReq req{ eGameType::PartyLeaveReq };
				sendPacket(sock, req);
			}
			else if (sub == "start")
			{
				const PartyStartReq req{ eGameType::PartyStartReq };
				sendPacket(sock, req);
			}
			else
			{
				printHelp();
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
