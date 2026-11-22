#pragma once
#include <atomic>
#include <functional>
#include <mutex>
#include <ostream>
#include <vector>
#include "NetworkLib.h" // 소켓 헤더를 다른 프로젝트 헤더보다 먼저(README_ERRORS 1-26)
#include "AccountTable.h"
#include "AdminProtocol.h"
#include "DbThread.h"
#include "GameDB.h"
#include "GameProtocol.h"
#include "GameRule.h"
#include "IntervalThread.h"
#include "Player.h"
#include "RedisClient.h"

// 게임 서버 - 로그인 서버가 준 세션 키로 입장(인증 분산)하고, 계정 행(MySQL)을 읽거나 만든다. 컨텐츠는 방 하나의 채팅
// (2026-11-20 사용자 결정: 입장·DB를 시험으로 먼저 다지고 컨텐츠는 그 뒤에 붙인다)
class GameServer final : public NetworkLib
{
public:
	// Player 배열을 maxSession개 만든다 - Start에 넘길 MaxSession과 같은 값. gameDB·redis는 서버보다 오래 살아야 한다
	// gameDB의 DB 스레드는 이미 돌고 있어야 한다. redis: 로그인 서버가 넣은 세션 키를 꺼낸다 - DB 스레드만 쓴다(연결은 DB 스레드를 띄우기 전에 main이)
	GameServer(const int32_t maxSession, const GameRule& rule, GameDB* gameDB, RedisClient* redis);
	~GameServer() override;

	// 서버를 멈출 때 라이브러리 Stop() 대신 이것을 부른다 - 입장 기한 감시 스레드를 멈추고, 새 요청을 받지 않게 한 뒤
	// DB 스레드가 남은 일을 끝내게 하고, 마지막에 라이브러리를 멈춘다(DB 스레드는 입장 결과를 보내고 UnholdSession을 부르므로 라이브러리보다 먼저 멈춘다)
	void Shutdown();

	int64_t GetBadPacketCnt() const // 컨텐츠가 잘못된 패킷(짧음, 모르는 종류, 종류와 맞지 않는 길이)으로 끊은 수 (누적)
	{
		return mBadPacketCnt;
	}
	int64_t GetEntryTimeoutCnt() const // 접속한 뒤 입장 기한(GameRule::EntryLimitSec) 안에 입장하지 않아 끊은 수 (누적)
	{
		return mEntryTimeoutCnt;
	}
	int64_t GetIdleTimeoutCnt() const // 입장한 뒤 GameRule::IdleLimitSec 동안 아무것도(하트비트 포함) 받지 못해 끊은 수 (누적)
	{
		return mIdleTimeoutCnt;
	}
	int64_t GetEntryAuthFailCnt() const // 입장 요청의 키가 없거나 틀려 응답 없이 끊은 수 (누적)
	{
		return mEntryAuthFailCnt;
	}
	int64_t GetEntryRejectCnt() const // 키는 맞았지만 이미 접속 중·차단·DB 오류로 응답 없이 끊은 수 (누적)
	{
		return mEntryRejectCnt;
	}
	int64_t GetChatCnt() const // 방송한 채팅 수 (누적)
	{
		return mChatCnt;
	}
	int64_t GetOnlineCnt() const;  // 통계(모니터 스레드) - 각자의 락으로 잠깐 읽는다
	int64_t GetAccountCnt() const;
	int64_t GetRoomMemberCnt() const;
	int64_t GetDbPendingCnt() const;
	int64_t GetDbFailCnt() const;

	// 운영 명령(운영자 서버의 워커에서) - 실제 일은 DB 스레드가 하고, 결과를 reply로 돌려준다(DB 스레드에서 불림)
	// DB 스레드에서 하는 이유: 이번 실행에서 읽지 않은 계정은 DB에만 있는데, 계정을 읽는 입장도 DB 스레드라 그동안 그 계정이 메모리에 올라오지 않는다
	void AdminBan(const std::string& accountID, const int32_t minutes, const std::function<void(const AdminBanRes&)>& reply);

protected:
	void OnAccept(const uint64_t id, const SOCKADDR_IN& remoteAddr) override;
	void OnRecv(const uint64_t id, const char* payload, const int32_t len) override;
	void OnRelease(const uint64_t id) override;

private:
	Player& getPlayer(const uint64_t id);         // 세션 ID의 하위 32비트(인덱스)로 같은 칸을 찾는다
	void onHello(const uint64_t id, const char* payload, const int32_t len);    // 워커, Hello 대기 - AccountNo를 적고 SeqNumber 시작값을 Challenge로 보낸다
	void onLoginReq(const uint64_t id, const char* payload, const int32_t len); // 워커, 입장 요청 대기 - 모양·SeqNumber를 보고 DB 스레드에 넘긴다
	void finishEntryOnDbThread(DbConn& conn, const uint64_t id, const std::string& packet); // 입장 처리 전체(DB 스레드) - 결과를 보내거나 끊고 UnholdSession
	// DB 스레드: Hello의 AccountNo로 Redis에서 세션 키를 꺼내 입장 요청을 풀어(키를 가졌는지) 확인하고, 통과하면 키를 지운다(한 번만).
	// 통과하면 *outAccountID에 아이디, 이 칸의 통로 키가 그 세션 키로 정해진다(입장 상태는 결과를 보낼 때)
	eLoginResult verifyEntryOnDbThread(const uint64_t id, const std::string& packet, std::string* outAccountID);
	eLoginResult loginAccountOnDbThread(DbConn& conn, const uint64_t id, const std::string& accountID); // 계정을 읽거나 만들어 접속 표시
	void onChatReq(const uint64_t id, const std::string& text); // 입장한 세션의 워커 - 본문 = 글자
	// 모든 게임 메시지는 이것으로 보낸다 - 입장 뒤에는 세션 통로로 암호화(칸의 송신 락 안에서 순번·암호화·SendPacket). 아무 스레드에서나
	void sendGame(const uint64_t id, const eGameType type, const void* body, const size_t bodyLen);
	bool postExternal(DbThread::Job job); // 운영 명령을 DB 스레드에 넣는다 - 종료 중이면 false(OnRecv와 같은 처리 중 수로 Shutdown과 맞춘다)
	void banJobOnDbThread(DbConn& conn, const std::string& accountID, const int32_t minutes, const std::function<void(const AdminBanRes&)>& reply); // 결과를 reply로
	void banOnDbThread(DbConn& conn, const std::string& accountID, const int32_t minutes, AdminBanRes* outRes);

	// 채팅방(하나) - 입장에 성공한 세션은 모두 들어간다. 방 락 안에서 구성원 모두에게 보내 받는 사람마다 같은 순서가 된다(락 순서: 방 → 송신 → 라이브러리)
	// 들어가기는 finishEntryOnDbThread가 방 락 안에서 입장 응답을 넣으며 함께 한다
	void leaveRoom(const uint64_t id); // OnRelease

	void checkEntryDeadline(); // 감시 스레드(1초마다) - 입장 기한이 지난 세션, 입장한 뒤 유휴 기한 동안 아무것도 보내지 않은 세션을 끊는다
	void disconnectBadPacket(const uint64_t id); // 잘못된 패킷 - 세고 끊는다

	std::vector<Player> mPlayers;           // 세션과 같은 인덱스 - 같은 인덱스는 이전 세션의 OnRelease가 끝난 뒤에만 다시 쓰인다
	AccountTable mAccountTable;
	const GameRule mRule;
	GameDB* mGameDB;           // 계정 읽기·쓰기를 DB 스레드로 넘긴다
	RedisClient* mRedis;       // DB 스레드만
	mutable std::mutex mRoomLock;
	std::vector<uint64_t> mRoomMembers; // 방에 있는 세션 ID - 순서는 상관없다(나갈 때 맨 끝과 바꿔 지운다)
	std::atomic<bool> mbClosing = false;          // Shutdown 중 - 새 패킷을 처리하지 않아 DB 요청이 더 생기지 않는다
	std::atomic<int32_t> mRecvInProgressCnt = 0;  // 지금 패킷을 처리 중인 워커 수 - Shutdown이 0이 되기를 기다린 뒤 DB 스레드를 멈춘다
	std::atomic<int64_t> mBadPacketCnt = 0; // 서로 다른 세션의 OnRecv가 여러 워커에서 동시에 올린다
	std::atomic<int64_t> mEntryTimeoutCnt = 0;
	std::atomic<int64_t> mIdleTimeoutCnt = 0;
	std::atomic<int64_t> mEntryAuthFailCnt = 0;
	std::atomic<int64_t> mEntryRejectCnt = 0;
	std::atomic<int64_t> mChatCnt = 0;
	IntervalThread mEntryWatch; // 워커 밖 스레드 - 입장 기한 감시. 라이브러리 Stop()보다 먼저 멈춘다
};

// 모니터링 출력 - std::cout << server; 한 줄(줄바꿈은 부르는 쪽이 붙인다)
std::ostream& operator<<(std::ostream& out, const GameServer& server);
