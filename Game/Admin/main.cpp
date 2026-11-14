#include <WinSock2.h>
#include <WS2tcpip.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>
#include "AdminProtocol.h"
#include "DbThread.h"
#include "GameData.h"
#include "PacketHeader.h"
#include "ParserLib.h"

#pragma comment(lib, "ws2_32.lib")

// 운영 툴(설계 3-5) - 회수·제재는 게임 서버의 운영자 포트로 명령하고(메모리와 DB를 서버가 함께 고친다),
// 로그 조회는 읽기만 하므로 DB를 직접 읽는다. 명령 하나를 보내고 답 하나를 받는 단순한 구조(받는 스레드 없음)

static GameData sGameData;
static bool sbHasGameData = false;

static std::string convert(const std::string& text, const UINT from, const UINT to)
{
	const int wideLen = MultiByteToWideChar(from, 0, text.c_str(), -1, nullptr, 0);
	std::wstring wide(static_cast<size_t>(wideLen), L'\0');
	MultiByteToWideChar(from, 0, text.c_str(), -1, wide.data(), wideLen);
	const int len = WideCharToMultiByte(to, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
	std::string out(static_cast<size_t>(len), '\0');
	WideCharToMultiByte(to, 0, wide.c_str(), -1, out.data(), len, nullptr, nullptr);
	out.resize(static_cast<size_t>(len > 0 ? len - 1 : 0));
	return out;
}

static std::string toConsole(const std::string& utf8)
{
	return convert(utf8, CP_UTF8, CP_ACP);
}

// 콘솔 입력 → UTF-8. 콘솔이 UTF-8이면(Windows Terminal 설정, 리다이렉트) 이미 UTF-8이라 그대로 - 다시 바꾸면 두 번 인코딩돼 깨진다
// CP949 한글 바이트열은 거의 항상 올바른 UTF-8이 아니어서 이것으로 가린다
static std::string toUtf8(const std::string& console)
{
	const bool bValidUtf8 = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, console.data(), static_cast<int>(console.size()), nullptr, 0) > 0;
	return (console.empty() || bValidUtf8) ? console : convert(console, GetConsoleCP(), CP_UTF8);
}

static std::string itemName(const uint16_t kind)
{
	const ItemData* item = sbHasGameData ? sGameData.FindItem(kind) : nullptr;
	return (item != nullptr) ? toConsole(item->Name) : ("종류 " + std::to_string(kind));
}

// 고정 길이 칸에 넣는다 - 넘치면 잘라 널로 끝나게
template <size_t N>
static void copyText(char (&outText)[N], const std::string& text)
{
	memset(outText, 0, N);
	memcpy(outText, text.data(), (text.size() < N) ? text.size() : N - 1);
}

template <size_t N>
static std::string toText(const char (&text)[N])
{
	return std::string(text, strnlen(text, N));
}

static std::string hex(const std::string& bytes)
{
	static const char DIGITS[] = "0123456789ABCDEF";
	std::string out = "X'";
	for (const char c : bytes)
	{
		out.push_back(DIGITS[static_cast<uint8_t>(c) >> 4]);
		out.push_back(DIGITS[static_cast<uint8_t>(c) & 0x0F]);
	}
	return out + "'";
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

static const char* toString(const eRecallResult result)
{
	switch (result)
	{
	case eRecallResult::Ok:
		return "회수함";
	case eRecallResult::NotFound:
		return "그런 아이템이 없음(이미 쓰였거나 버려짐)";
	case eRecallResult::Moved:
		return "회수하는 사이 아이템이 옮겨졌거나 쓰임 - 다시 시도";
	case eRecallResult::DbError:
		return "DB 오류 또는 서버 종료 중";
	default:
		return "알 수 없는 결과";
	}
}

static const char* toString(const eRecallFrom from)
{
	switch (from)
	{
	case eRecallFrom::Inventory:
		return "접속 기록이 있는 계정의 인벤토리";
	case eRecallFrom::Auction:
		return "경매(경매 취소, 입찰자 환불)";
	case eRecallFrom::OfflineAccount:
		return "이번 실행에서 로그인하지 않은 계정(DB)";
	default:
		return "?";
	}
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

static void printLogRows(const std::vector<std::vector<std::string>>& rows)
{
	// at, account, kind, item_uid, item_kind, gold_delta, auction_id, detail
	for (const std::vector<std::string>& row : rows)
	{
		const std::string item = row[3].empty() ? std::string("-") : (itemName(static_cast<uint16_t>(atoi(row[4].c_str()))) + " #" + row[3]);
		printf("  %s  %-10s %-16s %-24s 골드 %6s  경매 %-4s %s\n", row[0].c_str(), row[1].c_str(), row[2].c_str(), item.c_str(), row[5].c_str(),
			row[6].empty() ? "-" : row[6].c_str(), toConsole(row[7]).c_str());
	}
	if (rows.empty())
	{
		printf("  (없음)\n");
	}
}

static const char* const LOG_COLUMNS = "SELECT DATE_FORMAT(at, '%m-%d %H:%i:%s'), account_id, kind, IFNULL(item_uid, ''), IFNULL(item_kind, ''), gold_delta,"
	" IFNULL(auction_id, ''), IFNULL(detail, '') FROM logs ";

static void showAccountLog(DbConn& db, const std::string& accountID, const int count)
{
	std::vector<std::vector<std::string>> rows;
	if (!db.Query("SELECT gold, IFNULL(ban_until, ''), (SELECT COUNT(*) FROM items WHERE account_id = " + hex(accountID) + " AND auction_id IS NULL)"
		" FROM accounts WHERE account_id = " + hex(accountID), &rows))
	{
		return;
	}
	if (rows.empty())
	{
		printf("[로그] 그런 계정이 없음\n");
		return;
	}
	printf("[계정] %s - 골드 %s, 아이템 %s개%s%s (DB 기준 - 접속 중이면 저장 대기분만큼 늦을 수 있다)\n", accountID.c_str(), rows[0][0].c_str(), rows[0][2].c_str(),
		rows[0][1].empty() ? "" : ", 차단 ~ ", rows[0][1].c_str());
	if (db.Query(std::string(LOG_COLUMNS) + "WHERE account_id = " + hex(accountID) + " ORDER BY log_id DESC LIMIT " + std::to_string(count), &rows))
	{
		printf("[로그] 최근 %zu건(최신 먼저)\n", rows.size());
		printLogRows(rows);
	}
}

static void showItem(DbConn& db, const uint64_t itemUID)
{
	std::vector<std::vector<std::string>> rows;
	if (!db.Query("SELECT account_id, item_kind, IFNULL(equip_slot, ''), IFNULL(auction_id, '') FROM items WHERE item_uid = " + std::to_string(itemUID), &rows))
	{
		return;
	}
	if (rows.empty())
	{
		printf("[아이템] #%llu - 지금 없음(쓰였거나 버려졌거나 회수됨, 이력은 아래)\n", static_cast<unsigned long long>(itemUID));
	}
	else
	{
		printf("[아이템] #%llu %s - 주인 %s%s%s\n", static_cast<unsigned long long>(itemUID), itemName(static_cast<uint16_t>(atoi(rows[0][1].c_str()))).c_str(),
			rows[0][0].c_str(), rows[0][2].empty() ? "" : ", 착용 중", rows[0][3].empty() ? "" : (", 경매 #" + rows[0][3]).c_str());
	}
	if (db.Query(std::string(LOG_COLUMNS) + "WHERE item_uid = " + std::to_string(itemUID) + " ORDER BY log_id", &rows))
	{
		printf("[이력] %zu건(오래된 것 먼저)\n", rows.size());
		printLogRows(rows);
	}
}

static void printHelp()
{
	printf("명령: recall <아이템 번호> [사유] | ban <아이디> <분> [사유] | unban <아이디> [사유]\n");
	printf("      log <아이디> [개수] | item <아이템 번호> | help | quit\n");
}

// 남은 단어를 사유로 - 앞 공백을 떼고 UTF-8로(로그 detail에 그대로 남는다)
static std::string readNote(std::istringstream& in)
{
	std::string note;
	std::getline(in, note);
	const size_t start = note.find_first_not_of(' ');
	return (start == std::string::npos) ? std::string() : toUtf8(note.substr(start));
}

int main(int argc, char* argv[])
{
	const char* ip = (argc > 1) ? argv[1] : "127.0.0.1";
	const int port = (argc > 2) ? atoi(argv[2]) : 33001;

	// 서버와 같은 db.cfg - DB 접속(로그 조회)과 운영자 키
	utility::stDebug debug;
	const std::unique_ptr<utility::Parser> parser = utility::Parser::CreateParser("db.cfg", debug);
	DbConfig dbConfig;
	std::string adminKey;
	if (parser == nullptr || !parser->Get("host", dbConfig.Host) || !parser->Get("port", dbConfig.Port) || !parser->Get("user", dbConfig.User)
		|| !parser->Get("password", dbConfig.Password) || !parser->Get("database", dbConfig.Database) || !parser->Get("adminKey", adminKey) || adminKey.empty())
	{
		printf("db.cfg를 읽지 못함 - host·port·user·password·database·adminKey가 있어야 한다(작업 폴더에 db.cfg)\n");
		return 1;
	}
	std::string dataError;
	sbHasGameData = sGameData.Load("Data", &dataError);

	DbConn db;
	std::string dbError;
	if (!db.Connect(dbConfig, false, &dbError)) // 읽기만 하므로 데이터베이스를 만들지 않는다(읽기 전용 계정도 되게)
	{
		printf("DB 연결 실패 - %s\n", dbError.c_str());
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
	printf("운영자 포트 %s:%d 접속, DB %s 연결\n", ip, port, dbConfig.Database.c_str());
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
		if (cmd == "recall")
		{
			AdminRecallReq req{};
			req.Type = eAdminType::RecallReq;
			in >> req.ItemUID;
			copyText(req.Note, readNote(in));
			AdminRecallRes res{};
			bConnected = sendPacket(sock, req) && recvPacket(sock, eAdminType::RecallRes, &res);
			if (bConnected)
			{
				printf("[회수] %s", toString(res.Result));
				if (res.Result == eRecallResult::Ok)
				{
					printf(" - %s #%llu, 주인 %s, %s", itemName(res.Item.ItemKind).c_str(), static_cast<unsigned long long>(res.Item.ItemUID),
						toText(res.OwnerID).c_str(), toString(res.From));
				}
				printf("\n");
			}
		}
		else if (cmd == "ban" || cmd == "unban")
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
			copyText(req.Note, readNote(in));
			AdminBanRes res{};
			bConnected = sendPacket(sock, req) && recvPacket(sock, eAdminType::BanRes, &res);
			if (bConnected)
			{
				printf("[%s] %s %s%s\n", (req.Minutes > 0) ? "제재" : "해제", accountID.c_str(), toString(res.Result), res.bKicked ? " - 접속 중이라 끊음" : "");
			}
		}
		else if (cmd == "log")
		{
			std::string accountID;
			int count = 20;
			in >> accountID >> count;
			showAccountLog(db, accountID, (count > 0 && count <= 500) ? count : 20);
		}
		else if (cmd == "item")
		{
			uint64_t itemUID = 0;
			in >> itemUID;
			showItem(db, itemUID);
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
