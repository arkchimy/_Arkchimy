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

// 던전 표시용 - 수신 스레드가 출력 락 안에서만 쓰고, 턴 번호만 main이 읽는다
static std::atomic<uint32_t> sTurnNo = 0;                       // 마지막으로 받은 턴 번호 - 행동 요청에 싣는다
static std::string sMemberIDs[PARTY_MEMBER_MAX];                // 마지막 PartyInfoNotify의 파티원 순서
static uint16_t sMonsterKinds[DUNGEON_MONSTER_MAX] = {};         // 마지막 DungeonTurnNotify의 몬스터 순서
static int16_t sMonsterHp[DUNGEON_MONSTER_MAX] = {};
static int32_t sMonsterCnt = 0;

static std::string monsterName(const uint16_t kind)
{
	const MonsterData* monster = sbHasGameData ? sGameData.FindMonster(kind) : nullptr;
	return (monster != nullptr) ? toConsole(monster->Name) : ("몬스터 " + std::to_string(kind));
}

// 이벤트의 Actor·Target 값 → 이름 (몬스터는 "이름 번호")
static std::string actorName(const uint8_t actor)
{
	if (actor & ACTOR_MONSTER_FLAG)
	{
		const int32_t idx = actor & ~ACTOR_MONSTER_FLAG;
		const uint16_t kind = (idx < DUNGEON_MONSTER_MAX) ? sMonsterKinds[idx] : 0;
		return monsterName(kind) + " " + std::to_string(idx);
	}
	return (actor < PARTY_MEMBER_MAX && !sMemberIDs[actor].empty()) ? sMemberIDs[actor] : ("파티원 " + std::to_string(actor));
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
	for (int32_t i = 0; i < res.ItemCnt && i < INVENTORY_MAX; ++i)
	{
		const char* equippedSlot = "";
		for (int32_t slot = 0; slot < static_cast<int32_t>(eEquipSlot::Count); ++slot)
		{
			if (res.Equipped[slot] != 0 && res.Equipped[slot] == res.Items[i].ItemUID)
			{
				equippedSlot = SLOT_NAMES[slot];
			}
		}
		printf("  #%llu %s%s%s%s\n", static_cast<unsigned long long>(res.Items[i].ItemUID), itemName(res.Items[i].ItemKind).c_str(),
			equippedSlot[0] ? " (착용: " : "", equippedSlot, equippedSlot[0] ? ")" : "");
	}
}

static const char* toString(const eEquipResult result)
{
	switch (result)
	{
	case eEquipResult::Ok:
		return "성공";
	case eEquipResult::NotFound:
		return "인벤토리에 없는 아이템";
	case eEquipResult::NotEquippable:
		return "장비가 아님";
	case eEquipResult::InDungeon:
		return "던전 중에는 바꿀 수 없음";
	case eEquipResult::BadSlot:
		return "없는 장비 칸(0 무기, 1 머리, 2 장신구, 3 갑옷)";
	default:
		return "알 수 없는 결과";
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

static void printDungeonEvent(const DungeonEventNotify& e)
{
	const std::string actor = actorName(e.Actor);
	const std::string target = actorName(e.Target);
	switch (e.Event)
	{
	case eDungeonEvent::EnterRoom:
		printf("[던전] %u번 방에 들어갑니다\n", e.RoomNo);
		break;
	case eDungeonEvent::Attack:
		printf("[던전] %s → %s 공격: 주사위 %u + %d vs %u → %s", actor.c_str(), target.c_str(), e.Roll, e.Bonus, e.Difficulty, e.bSuccess ? "명중" : "빗나감");
		printf(e.bSuccess ? ", 피해 %d\n" : "\n", e.Damage);
		break;
	case eDungeonEvent::Defend:
		printf("[던전] %s 방어\n", actor.c_str());
		break;
	case eDungeonEvent::AutoDefend:
		printf("[던전] %s 자동 방어(시간 초과·끊김)\n", actor.c_str());
		break;
	case eDungeonEvent::Investigate:
		printf("[던전] %s 조사: 주사위 %u + %d vs %u → %s\n", actor.c_str(), e.Roll, e.Bonus, e.Difficulty, e.bSuccess ? "성공" : "실패");
		break;
	case eDungeonEvent::UsePotion:
		printf("[던전] %s 회복약 사용: 체력 +%d\n", actor.c_str(), e.Damage);
		break;
	case eDungeonEvent::Trap:
		printf("[던전] %s 함정 판정: 주사위 %u + %d vs %u → %s", actor.c_str(), e.Roll, e.Bonus, e.Difficulty, e.bSuccess ? "피함" : "걸림");
		printf(e.bSuccess ? "\n" : ", 피해 %d\n", e.Damage);
		break;
	case eDungeonEvent::Special:
		printf("[던전] %s 돌 부스러기 → %s 피해 %d\n", actor.c_str(), target.c_str(), e.Damage);
		break;
	case eDungeonEvent::Down:
		printf("[던전] %s 쓰러짐\n", actor.c_str());
		break;
	case eDungeonEvent::ActionRejected:
		printf("[던전] 할 수 없는 행동입니다(대상·조사할 것·회복약 확인)\n");
		break;
	default:
		printf("[던전] 모르는 이벤트 %u\n", static_cast<unsigned>(e.Event));
		break;
	}
}

static void printDungeonTurn(const DungeonTurnNotify& t)
{
	sTurnNo = t.TurnNo;
	printf("[던전 %u번 방, 턴 %u] 차례: %s (%u초) | 파티:", t.RoomNo, t.TurnNo, actorName(t.ActorIdx).c_str(), t.TimeLimitMs / 1000);
	for (int32_t i = 0; i < t.MemberCnt && i < PARTY_MEMBER_MAX; ++i)
	{
		printf(" %s %d", actorName(static_cast<uint8_t>(i)).c_str(), t.MemberHp[i]);
	}
	if (t.MonsterCnt > 0)
	{
		printf(" | 몬스터:");
	}
	for (int32_t i = 0; i < t.MonsterCnt && i < DUNGEON_MONSTER_MAX; ++i)
	{
		sMonsterKinds[i] = t.MonsterKind[i];
		sMonsterHp[i] = t.MonsterHp[i];
		printf(" [%d]%s %d", i, monsterName(t.MonsterKind[i]).c_str(), t.MonsterHp[i]);
	}
	sMonsterCnt = (t.MonsterCnt < DUNGEON_MONSTER_MAX) ? t.MonsterCnt : DUNGEON_MONSTER_MAX;
	printf("\n");
}

// 화살표 키로 공격 대상 고르기 - ← → 로 옮기고 Enter로 정한다, Esc는 취소(-1)
// 수신 스레드가 그사이 줄을 출력할 수 있어, 고르는 줄은 매번 출력 락 안에서 \r로 다시 그린다
static int selectTarget()
{
	int32_t cnt = 0;
	int32_t selected = -1;
	{
		std::lock_guard<std::mutex> lock(sPrintLock);
		cnt = sMonsterCnt;
		for (int32_t i = 0; i < cnt && selected < 0; ++i)
		{
			selected = (sMonsterHp[i] > 0) ? i : -1; // 살아 있는 첫 몬스터부터
		}
	}
	if (selected < 0)
	{
		std::lock_guard<std::mutex> lock(sPrintLock);
		printf("[던전] 공격할 몬스터가 없습니다\n");
		return -1;
	}
	while (true)
	{
		{
			std::lock_guard<std::mutex> lock(sPrintLock);
			printf("\r대상(←→ 이동, Enter 공격, Esc 취소):");
			for (int32_t i = 0; i < cnt; ++i)
			{
				const std::string label = monsterName(sMonsterKinds[i]) + " " + std::to_string(i) + "(" + std::to_string(sMonsterHp[i]) + ")";
				printf(i == selected ? " [%s]" : "  %s ", label.c_str());
			}
			printf("   ");
		}
		const int key = _getch();
		if (key == 0 || key == 224) // 화살표 키는 두 바이트
		{
			const int arrow = _getch();
			const int step = (arrow == 75) ? -1 : (arrow == 77) ? 1 : 0; // 75 ←, 77 →
			std::lock_guard<std::mutex> lock(sPrintLock);                // 체력은 수신 스레드가 바꾼다
			for (int32_t moved = 0; step != 0 && moved < cnt; ++moved)
			{
				selected = (selected + step + cnt) % cnt;
				if (sMonsterHp[selected] > 0)
				{
					break; // 쓰러진 몬스터는 건너뛴다
				}
			}
		}
		else if (key == '\r' || key == 27)
		{
			std::lock_guard<std::mutex> lock(sPrintLock);
			printf("\n");
			return (key == '\r') ? selected : -1;
		}
	}
}

static void printPartyInfo(const PartyInfoNotify& notify)
{
	for (int32_t i = 0; i < PARTY_MEMBER_MAX; ++i)
	{
		sMemberIDs[i] = (i < notify.MemberCnt) ? toID(notify.MemberIDs[i]) : std::string();
	}
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
	case eGameType::EquipRes:
	{
		EquipRes res;
		if (readAs(payload, len, &res))
		{
			printf("[장비] %s\n", toString(res.Result));
			return;
		}
		break;
	}
	case eGameType::DungeonEventNotify:
	{
		DungeonEventNotify notify;
		if (readAs(payload, len, &notify))
		{
			printDungeonEvent(notify);
			return;
		}
		break;
	}
	case eGameType::DungeonTurnNotify:
	{
		DungeonTurnNotify notify;
		if (readAs(payload, len, &notify))
		{
			printDungeonTurn(notify);
			return;
		}
		break;
	}
	case eGameType::DungeonEndNotify:
	{
		DungeonEndNotify notify;
		if (readAs(payload, len, &notify))
		{
			printf("[던전 끝] %s\n", notify.bSuccess ? "성공! 등불 골렘을 쓰러뜨렸습니다" : "실패 - 파티가 모두 쓰러졌습니다");
			if (notify.bSuccess)
			{
				printf("[보상] 골드 +%d", notify.Gold);
				for (int32_t i = 0; i < notify.ItemCnt && i < DUNGEON_REWARD_MAX; ++i)
				{
					printf(", %s(#%llu)", itemName(notify.Items[i].ItemKind).c_str(), static_cast<unsigned long long>(notify.Items[i].ItemUID));
				}
				printf(notify.DiscardedCnt > 0 ? " - 인벤토리가 가득 차 %u개를 버렸습니다\n" : "\n", notify.DiscardedCnt);
			}
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
	printf("명령: login <아이디> | inv | party create | party list | party join <번호> | party leave | party start\n");
	printf("      장비: equip <아이템 번호> | unequip <칸: 0 무기, 1 머리, 2 장신구, 3 갑옷>\n");
	printf("      던전: attack(화살표로 대상 선택) | attack <몬스터 번호> | defend | search | potion | help | quit\n");
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
		else if (cmd == "attack" || cmd == "defend" || cmd == "search" || cmd == "potion")
		{
			DungeonActionReq req{};
			req.Type = eGameType::DungeonActionReq;
			req.TurnNo = sTurnNo;
			req.Action = (cmd == "attack") ? eDungeonAction::Attack : (cmd == "defend") ? eDungeonAction::Defend
				: (cmd == "search") ? eDungeonAction::Investigate : eDungeonAction::UsePotion;
			int target = 0;
			if (cmd == "attack" && !(in >> target))
			{
				target = selectTarget(); // 번호 없이 attack이면 화살표 키로 고른다(번호를 붙이면 바로 - 시험 스크립트용)
				if (target < 0)
				{
					continue;
				}
			}
			req.Target = static_cast<uint8_t>(target);
			sendPacket(sock, req);
		}
		else if (cmd == "equip")
		{
			uint64_t itemUID = 0;
			in >> itemUID;
			const EquipReq req{ eGameType::EquipReq, itemUID };
			sendPacket(sock, req);
		}
		else if (cmd == "unequip")
		{
			int slot = -1;
			in >> slot;
			const UnequipReq req{ eGameType::UnequipReq, static_cast<eEquipSlot>(slot) };
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
