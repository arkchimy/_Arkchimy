#include <WinSock2.h>
#include <WS2tcpip.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>
#include "GameProtocol.h"
#include "LoginClient.h"
#include "PacketHeader.h"
#include "SessionAuth.h"

#pragma comment(lib, "ws2_32.lib")

// 장시간 시험용 봇 - 계정마다 스레드 하나로 접속을 유지하며 던전·경매·장비·접속 끊기를 반복한다
// 사용: GameBot.exe <봇 수> <시간(초)> [아이디 접두어] [IP] [포트] [로그인 서버 포트]
// 처음에 한 번 가입하고(이미 있으면 그대로), 접속할 때마다 로그인 서버에서 세션 키를 받아 게임 서버에 들어간다
// 판정에 쓰는 값: 예상밖끊김(봇이 끊지 않았는데 서버가 끊음)과 프로토콜오류(모르는 종류·길이) - 둘 다 0이어야 한다
// 서버를 일부러 괴롭히지 않는다 - 정상 클라가 할 수 있는 행동만 한다(거절되는 요청도 정상 범위: 늦은 입찰, 던전 중 등록 등)

using Clock = std::chrono::steady_clock;

enum : int32_t
{
	GROUP_SIZE = 3,          // 파티 한 조 - 첫 번째 봇이 방장
	MAX_BOT = 600,
	SELECT_WAIT_MS = 50,
	SKIP_TURN_PERCENT = 5,   // 차례에 행동하지 않아 턴 마감 타이머가 자동 방어하게
	RECONNECT_MIN_SEC = 120, // 스스로 끊는 간격
	RECONNECT_MAX_SEC = 360,
};

struct Stats
{
	std::atomic<int64_t> Logins{ 0 };
	std::atomic<int64_t> DungeonWin{ 0 };
	std::atomic<int64_t> DungeonLose{ 0 };
	std::atomic<int64_t> Actions{ 0 };
	std::atomic<int64_t> SkippedTurns{ 0 };
	std::atomic<int64_t> AutoDefends{ 0 };
	std::atomic<int64_t> Rejected{ 0 };      // ActionRejected
	std::atomic<int64_t> AuctionRegister{ 0 };
	std::atomic<int64_t> Bids{ 0 };
	std::atomic<int64_t> Won{ 0 };
	std::atomic<int64_t> Sold{ 0 };
	std::atomic<int64_t> Unsold{ 0 };
	std::atomic<int64_t> Equips{ 0 };
	std::atomic<int64_t> SelfDisconnects{ 0 };
	std::atomic<int64_t> UnexpectedClose{ 0 }; // 판정
	std::atomic<int64_t> ProtocolErrors{ 0 };  // 판정
	std::atomic<int64_t> AuthFails{ 0 };       // 판정 - 가입·로그인 서버 실패, 게임 서버 입장 확인 실패(AuthFail)
	std::atomic<int64_t> Connected{ 0 };
};

static Stats sStats;
static std::atomic<uint64_t> sGroupParty[MAX_BOT / GROUP_SIZE + 1]; // 조마다 방장이 만든 파티 번호 - 조원이 이 번호로 참가한다
static std::atomic<bool> sbStop = false;
static std::string sIP = "127.0.0.1";
static int sPort = 33000;
static int sLoginPort = 33002;
static const char BOT_PASSWORD[] = "bot-password"; // 봇 계정의 비밀번호(시험용)

template <typename T>
static bool sendPacket(const SOCKET sock, const T& body)
{
	char buf[PACKET_HEADER_SIZE + sizeof(T)];
	const PacketHeader header{ static_cast<uint16_t>(sizeof(T)) };
	memcpy(buf, &header, PACKET_HEADER_SIZE);
	memcpy(buf + PACKET_HEADER_SIZE, &body, sizeof(T));
	int sent = 0;
	while (sent < static_cast<int>(sizeof(buf)))
	{
		const int ret = send(sock, buf + sent, static_cast<int>(sizeof(buf)) - sent, 0);
		if (ret <= 0)
		{
			return false;
		}
		sent += ret;
	}
	return true;
}

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

class Bot
{
public:
	Bot(const int idx, const std::string& prefix)
		: mIdx(idx)
		, mID(prefix + std::to_string(idx))
		, mRng(static_cast<uint32_t>(idx * 7919 + 17))
	{
	}

	void Run()
	{
		eRegisterResult registerResult = eRegisterResult::ServerError;
		if (!RequestRegister(sIP.c_str(), sLoginPort, mID, BOT_PASSWORD, &registerResult)
			|| (registerResult != eRegisterResult::Ok && registerResult != eRegisterResult::Exists))
		{
			++sStats.AuthFails;
			fprintf(stderr, "[bot] %s 가입 실패(결과 %d)\n", mID.c_str(), static_cast<int>(registerResult));
		}
		while (!sbStop)
		{
			if (mSock == INVALID_SOCKET && Clock::now() >= mReconnectAt)
			{
				connectAndLogin();
			}
			if (mSock == INVALID_SOCKET)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
				continue;
			}
			pump();
			if (mSock != INVALID_SOCKET)
			{
				act();
			}
		}
		closeSocket(true);
	}

private:
	int randInt(const int lo, const int hi)
	{
		return std::uniform_int_distribution<int>(lo, hi)(mRng);
	}

	void connectAndLogin()
	{
		// 로그인 서버에서 세션 키를 받는다(별도 접속) - 30초 안에 게임 서버에 들어가야 한다
		AuthRes auth{};
		if (!RequestAuth(sIP.c_str(), sLoginPort, mID, BOT_PASSWORD, &auth) || auth.Result != eAuthResult::Ok)
		{
			++sStats.AuthFails;
			fprintf(stderr, "[bot] %s 로그인 서버 실패(결과 %d)\n", mID.c_str(), static_cast<int>(auth.Result));
			mReconnectAt = Clock::now() + std::chrono::seconds(1);
			return;
		}
		mSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		SOCKADDR_IN addr{};
		addr.sin_family = AF_INET;
		addr.sin_port = htons(static_cast<u_short>(sPort));
		inet_pton(AF_INET, sIP.c_str(), &addr.sin_addr);
		if (connect(mSock, reinterpret_cast<SOCKADDR*>(&addr), sizeof(addr)) == SOCKET_ERROR)
		{
			closesocket(mSock);
			mSock = INVALID_SOCKET;
			mReconnectAt = Clock::now() + std::chrono::seconds(1);
			return;
		}
		++sStats.Connected;
		mRecvBuf.clear();
		mbLoggedIn = false;
		mPartyID = 0;
		mbInDungeon = false;
		mSelfDisconnectAt = Clock::now() + std::chrono::seconds(randInt(RECONNECT_MIN_SEC, RECONNECT_MAX_SEC));
		LoginReq req{};
		req.Type = eGameType::LoginReq;
		req.AccountNo = auth.AccountNo;
		MakeEnterProof(auth.SessionKey, auth.AccountNo, req.Proof);
		SecureZeroMemory(auth.SessionKey, sizeof(auth.SessionKey));
		sendPacket(mSock, req);
	}

	// bSelf: 봇이 스스로 끊음(아니면 서버가 끊은 것 - 예상밖끊김)
	void closeSocket(const bool bSelf)
	{
		if (mSock == INVALID_SOCKET)
		{
			return;
		}
		if (bSelf && randInt(0, 1) == 0)
		{
			const LINGER linger{ 1, 0 }; // 절반은 RST로 끊는다(클라 강제 종료와 같다)
			setsockopt(mSock, SOL_SOCKET, SO_LINGER, reinterpret_cast<const char*>(&linger), sizeof(linger));
		}
		closesocket(mSock);
		mSock = INVALID_SOCKET;
		--sStats.Connected;
		mReconnectAt = Clock::now() + std::chrono::milliseconds(randInt(500, 2000));
	}

	// 받은 것을 모두 꺼내 처리한다(최대 SELECT_WAIT_MS 기다림)
	void pump()
	{
		fd_set readSet;
		FD_ZERO(&readSet);
		FD_SET(mSock, &readSet);
		timeval wait{ 0, SELECT_WAIT_MS * 1000 };
		if (select(0, &readSet, nullptr, nullptr, &wait) <= 0)
		{
			return;
		}
		char buf[16384];
		const int ret = recv(mSock, buf, sizeof(buf), 0);
		if (ret <= 0)
		{
			++sStats.UnexpectedClose; // 봇이 끊지 않았는데 끊김
			fprintf(stderr, "[bot] %s 서버가 끊음(로그인 %d, 파티 %llu, 던전 %d)\n", mID.c_str(), mbLoggedIn ? 1 : 0,
				static_cast<unsigned long long>(mPartyID), mbInDungeon ? 1 : 0);
			closeSocket(false);
			return;
		}
		mRecvBuf.append(buf, static_cast<size_t>(ret));
		size_t pos = 0;
		while (mRecvBuf.size() - pos >= PACKET_HEADER_SIZE)
		{
			PacketHeader header;
			memcpy(&header, mRecvBuf.data() + pos, PACKET_HEADER_SIZE);
			if (header.Len > MAX_PAYLOAD_SIZE)
			{
				++sStats.ProtocolErrors;
				closeSocket(true);
				return;
			}
			if (mRecvBuf.size() - pos < PACKET_HEADER_SIZE + header.Len)
			{
				break;
			}
			onPacket(mRecvBuf.data() + pos + PACKET_HEADER_SIZE, header.Len);
			pos += PACKET_HEADER_SIZE + header.Len;
			if (mSock == INVALID_SOCKET)
			{
				return;
			}
		}
		mRecvBuf.erase(0, pos);
	}

	void onPacket(const char* payload, const int len)
	{
		eGameType type;
		memcpy(&type, payload, sizeof(type));
		bool bOk = true;
		switch (type)
		{
		case eGameType::LoginRes:
		{
			LoginRes res;
			bOk = readAs(payload, len, &res);
			if (bOk && res.Result == eLoginResult::Ok)
			{
				mbLoggedIn = true;
				++sStats.Logins;
				const InventoryReq inv{ eGameType::InventoryReq };
				sendPacket(mSock, inv);
			}
			else if (bOk)
			{
				// 스스로 끊은 직후라 이전 접속이 아직 정리 중이면 AlreadyLoggedIn(서버가 이전 접속을 끊는다) - 이 접속을 닫고 잠시 뒤 새로 접속한다
				if (res.Result == eLoginResult::AuthFail)
				{
					++sStats.AuthFails; // 방금 받은 세션 키가 통하지 않음 - 정상이면 0
					fprintf(stderr, "[bot] %s 게임 서버 입장 확인 실패\n", mID.c_str());
				}
				closeSocket(true);
			}
			break;
		}
		case eGameType::InventoryRes:
		{
			InventoryRes res;
			bOk = readAs(payload, len, &res);
			if (bOk)
			{
				mInventory = res;
			}
			break;
		}
		case eGameType::PartyCreateRes:
		{
			PartyCreateRes res;
			bOk = readAs(payload, len, &res);
			if (bOk && res.Result == eCreatePartyResult::Ok)
			{
				sGroupParty[mIdx / GROUP_SIZE] = res.PartyID;
			}
			break;
		}
		case eGameType::PartyJoinRes:
		case eGameType::PartyStartRes:
		case eGameType::EquipRes:
		case eGameType::AuctionRegisterRes:
		case eGameType::AuctionBidRes:
		case eGameType::AuctionNotify:
			bOk = len >= 2; // 결과는 거절이어도 정상 범위
			break;
		case eGameType::PartyListRes:
			bOk = len == static_cast<int>(sizeof(PartyListRes));
			break;
		case eGameType::PartyInfoNotify:
		{
			PartyInfoNotify notify;
			bOk = readAs(payload, len, &notify);
			if (bOk)
			{
				mPartyID = (notify.State == ePartyState::None) ? 0 : notify.PartyID;
				mPartyMemberCnt = notify.MemberCnt;
				mbInDungeon = notify.State == ePartyState::InDungeon;
				mMyIdx = -1;
				for (int i = 0; i < notify.MemberCnt && i < PARTY_MEMBER_MAX; ++i)
				{
					if (strncmp(notify.MemberIDs[i], mID.c_str(), ACCOUNT_ID_LEN) == 0)
					{
						mMyIdx = i;
					}
				}
				mPartyChangedAt = Clock::now();
			}
			break;
		}
		case eGameType::DungeonTurnNotify:
		{
			DungeonTurnNotify turn;
			bOk = readAs(payload, len, &turn);
			if (bOk)
			{
				mbInDungeon = true;
				onTurn(turn);
			}
			break;
		}
		case eGameType::DungeonEventNotify:
		{
			DungeonEventNotify e;
			bOk = readAs(payload, len, &e);
			if (bOk && e.Event == eDungeonEvent::ActionRejected && e.Actor == mMyIdx)
			{
				++sStats.Rejected;
				const DungeonActionReq req{ eGameType::DungeonActionReq, mLastTurn.TurnNo, eDungeonAction::Defend, 0 }; // 할 수 없는 행동이면 방어로
				sendPacket(mSock, req);
			}
			else if (bOk && e.Event == eDungeonEvent::AutoDefend && e.Actor == mMyIdx)
			{
				++sStats.AutoDefends;
			}
			break;
		}
		case eGameType::DungeonEndNotify:
		{
			DungeonEndNotify end;
			bOk = readAs(payload, len, &end);
			if (bOk)
			{
				(end.bSuccess ? sStats.DungeonWin : sStats.DungeonLose)++;
				mbInDungeon = false;
				const InventoryReq inv{ eGameType::InventoryReq };
				sendPacket(mSock, inv);
			}
			break;
		}
		case eGameType::AuctionListRes:
		{
			AuctionListRes res;
			bOk = readAs(payload, len, &res);
			if (bOk)
			{
				bidOnSomething(res);
			}
			break;
		}
		case eGameType::AuctionResultNotify:
		{
			AuctionResultNotify notify;
			bOk = readAs(payload, len, &notify);
			if (bOk)
			{
				if (notify.Result == eAuctionResult::Won)
				{
					++sStats.Won;
				}
				else if (notify.Result == eAuctionResult::Sold)
				{
					++sStats.Sold;
				}
				else if (notify.Result == eAuctionResult::Unsold || notify.Result == eAuctionResult::Discarded)
				{
					++sStats.Unsold;
				}
				const InventoryReq inv{ eGameType::InventoryReq };
				sendPacket(mSock, inv);
			}
			break;
		}
		case eGameType::RecallNotify:
			bOk = len == static_cast<int>(sizeof(RecallNotify));
			break;
		default:
			bOk = false;
			break;
		}
		if (!bOk)
		{
			++sStats.ProtocolErrors;
			fprintf(stderr, "[bot] %s 모르는 패킷 type=%u len=%d\n", mID.c_str(), static_cast<unsigned>(type), len);
		}
	}

	void onTurn(const DungeonTurnNotify& turn)
	{
		mLastTurn = turn;
		if (turn.ActorIdx != mMyIdx || mMyIdx < 0)
		{
			return;
		}
		if (randInt(1, 100) <= SKIP_TURN_PERCENT)
		{
			++sStats.SkippedTurns; // 행동하지 않는다 - 10초 뒤 턴 마감 타이머가 자동 방어
			return;
		}
		DungeonActionReq req{ eGameType::DungeonActionReq, turn.TurnNo, eDungeonAction::Attack, 0 };
		int target = -1;
		for (int i = 0; i < turn.MonsterCnt && i < DUNGEON_MONSTER_MAX; ++i)
		{
			if (turn.MonsterHp[i] > 0 && target < 0)
			{
				target = i;
			}
		}
		const int myHp = turn.MemberHp[mMyIdx];
		if (myHp > 0 && myHp < 8 && randInt(0, 1) == 0)
		{
			req.Action = eDungeonAction::UsePotion;
		}
		else if (target >= 0)
		{
			req.Target = static_cast<uint8_t>(target);
		}
		else
		{
			req.Action = eDungeonAction::Investigate;
		}
		++sStats.Actions;
		sendPacket(mSock, req);
	}

	void bidOnSomething(const AuctionListRes& res)
	{
		if (res.Cnt == 0)
		{
			return;
		}
		const AuctionEntry& entry = res.Entries[randInt(0, res.Cnt - 1)];
		if (strncmp(entry.SellerID, mID.c_str(), ACCOUNT_ID_LEN) == 0)
		{
			return;
		}
		const int32_t price = entry.bHasBid ? entry.Price + entry.Price / 10 + 1 : entry.Price;
		if (price > mInventory.Gold)
		{
			return;
		}
		const AuctionBidReq req{ eGameType::AuctionBidReq, entry.AuctionID, price };
		++sStats.Bids;
		sendPacket(mSock, req);
	}

	// 로비 행동 - 0.5–3초마다 하나
	void act()
	{
		const Clock::time_point now = Clock::now();
		if (now >= mSelfDisconnectAt)
		{
			++sStats.SelfDisconnects;
			closeSocket(true);
			return;
		}
		if (!mbLoggedIn || now < mNextActAt)
		{
			return;
		}
		mNextActAt = now + std::chrono::milliseconds(randInt(500, 3000));
		const bool bLeader = (mIdx % GROUP_SIZE) == 0;
		std::atomic<uint64_t>& groupParty = sGroupParty[mIdx / GROUP_SIZE];

		if (mPartyID == 0)
		{
			if (bLeader)
			{
				const PartyCreateReq req{ eGameType::PartyCreateReq };
				sendPacket(mSock, req);
			}
			else if (groupParty != 0)
			{
				const PartyJoinReq req{ eGameType::PartyJoinReq, groupParty.load() };
				sendPacket(mSock, req);
			}
		}
		else if (!mbInDungeon && bLeader && mPartyMemberCnt >= 2 && now - mPartyChangedAt > std::chrono::seconds(2))
		{
			const PartyStartReq req{ eGameType::PartyStartReq };
			sendPacket(mSock, req);
			return;
		}

		const int roll = randInt(1, 100);
		if (roll <= 15)
		{
			sellSomething();
		}
		else if (roll <= 35)
		{
			const AuctionListReq req{ eGameType::AuctionListReq, 0, 0 };
			sendPacket(mSock, req);
		}
		else if (roll <= 42)
		{
			equipSomething();
		}
		else if (roll <= 45)
		{
			const AuctionWatchReq req{ eGameType::AuctionWatchReq, static_cast<uint8_t>(randInt(0, 1)) };
			sendPacket(mSock, req);
		}
		else if (roll <= 47 && mPartyID != 0 && !bLeader && !mbInDungeon)
		{
			const PartyLeaveReq req{ eGameType::PartyLeaveReq }; // 가끔 파티를 나갔다가 다시 들어온다
			sendPacket(mSock, req);
		}
	}

	bool isEquipped(const uint64_t uid) const
	{
		for (const uint64_t equipped : mInventory.Equipped)
		{
			if (equipped == uid)
			{
				return true;
			}
		}
		return false;
	}

	void sellSomething()
	{
		if (mInventory.ItemCnt == 0)
		{
			return;
		}
		const ItemInfo& item = mInventory.Items[randInt(0, mInventory.ItemCnt - 1)];
		if (isEquipped(item.ItemUID))
		{
			return;
		}
		const AuctionRegisterReq req{ eGameType::AuctionRegisterReq, item.ItemUID, randInt(1, 40) };
		++sStats.AuctionRegister;
		sendPacket(mSock, req);
		const InventoryReq inv{ eGameType::InventoryReq };
		sendPacket(mSock, inv);
	}

	void equipSomething()
	{
		if (mInventory.ItemCnt == 0)
		{
			return;
		}
		const ItemInfo& item = mInventory.Items[randInt(0, mInventory.ItemCnt - 1)];
		const EquipReq req{ eGameType::EquipReq, item.ItemUID }; // 장비가 아니면 거절 - 정상 범위
		++sStats.Equips;
		sendPacket(mSock, req);
	}

	const int mIdx;
	const std::string mID;
	std::mt19937 mRng;
	SOCKET mSock = INVALID_SOCKET;
	std::string mRecvBuf;
	bool mbLoggedIn = false;
	uint64_t mPartyID = 0;
	int mPartyMemberCnt = 0;
	int mMyIdx = -1;
	bool mbInDungeon = false;
	DungeonTurnNotify mLastTurn{};
	InventoryRes mInventory{};
	Clock::time_point mPartyChangedAt = Clock::now();
	Clock::time_point mNextActAt = Clock::now();
	Clock::time_point mReconnectAt = Clock::now();
	Clock::time_point mSelfDisconnectAt = Clock::now();
};

static void printStats(const char* tag)
{
	printf("%s 접속=%lld 로그인=%lld 던전성공=%lld 던전실패=%lld 행동=%lld 일부러쉼=%lld 자동방어=%lld 거절=%lld 경매등록=%lld 입찰=%lld 낙찰=%lld 판매=%lld 유찰=%lld 장비=%lld 스스로끊음=%lld 예상밖끊김=%lld 프로토콜오류=%lld 인증실패=%lld\n",
		tag, sStats.Connected.load(), sStats.Logins.load(), sStats.DungeonWin.load(), sStats.DungeonLose.load(), sStats.Actions.load(),
		sStats.SkippedTurns.load(), sStats.AutoDefends.load(), sStats.Rejected.load(), sStats.AuctionRegister.load(), sStats.Bids.load(),
		sStats.Won.load(), sStats.Sold.load(), sStats.Unsold.load(), sStats.Equips.load(), sStats.SelfDisconnects.load(),
		sStats.UnexpectedClose.load(), sStats.ProtocolErrors.load(), sStats.AuthFails.load());
	fflush(stdout);
}

int main(int argc, char* argv[])
{
	if (argc < 3)
	{
		printf("사용: GameBot.exe <봇 수> <시간(초)> [아이디 접두어] [IP] [포트] [로그인 서버 포트]\n");
		return 1;
	}
	const int botCnt = atoi(argv[1]);
	const int durationSec = atoi(argv[2]);
	const std::string prefix = (argc > 3) ? argv[3] : "bot";
	sIP = (argc > 4) ? argv[4] : sIP;
	sPort = (argc > 5) ? atoi(argv[5]) : sPort;
	sLoginPort = (argc > 6) ? atoi(argv[6]) : sLoginPort;
	if (!InitCrypto())
	{
		printf("libsodium 초기화 실패\n");
		return 1;
	}
	if (botCnt < 1 || botCnt > MAX_BOT || durationSec < 1 || prefix.size() + 3 >= ACCOUNT_ID_LEN)
	{
		printf("봇 수 1~%d, 시간 1초 이상, 접두어는 12바이트 이하\n", MAX_BOT);
		return 1;
	}
	WSADATA wsa;
	WSAStartup(MAKEWORD(2, 2), &wsa);
	printf("[bot] %d개, %d초, 접두어 %s, %s:%d\n", botCnt, durationSec, prefix.c_str(), sIP.c_str(), sPort);

	std::vector<std::thread> threads;
	for (int i = 0; i < botCnt; ++i)
	{
		threads.emplace_back([i, &prefix]
			{
				Bot bot(i, prefix);
				bot.Run();
			});
		std::this_thread::sleep_for(std::chrono::milliseconds(20)); // 한꺼번에 몰리지 않게
	}
	const Clock::time_point end = Clock::now() + std::chrono::seconds(durationSec);
	while (Clock::now() < end)
	{
		std::this_thread::sleep_for(std::chrono::seconds(10));
		printStats("[bot]");
	}
	sbStop = true;
	for (std::thread& t : threads)
	{
		t.join();
	}
	printStats("[bot-result]");
	const bool bPass = sStats.UnexpectedClose == 0 && sStats.ProtocolErrors == 0 && sStats.AuthFails == 0;
	printf("[bot-result] %s\n", bPass ? "PASS" : "FAIL");
	WSACleanup();
	return bPass ? 0 : 2;
}
