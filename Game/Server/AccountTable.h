#pragma once
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include "Account.h"
#include "GameProtocol.h"
#include "Memory.h"

// 아이디 → 계정. 로그인·로그아웃과 중복 로그인 막기
// 계정은 처음 로그인할 때 DB에서 읽어 넣고, 서버가 끝날 때까지 내리지 않는다 - 파티·던전·경매가 계정 주소를 들고 있어도 된다
class AccountTable final
{
public:
	bool Has(const std::string& id); // 이미 읽은 계정 - 있으면 DB를 거치지 않고 Login(id, ..., nullptr)

	// 계정 표 락 안에서 접속 표시. loaded: DB에서 막 읽은 계정(이미 표에 있으면 버리고 표의 것을 쓴다 - 같은 아이디가 동시에 읽힌 경우)
	// 표에 없는데 loaded도 없으면 부르는 쪽의 실수(Has가 true일 때만 nullptr로 부른다)
	// 이미 접속 중이면 AlreadyLoggedIn과 그 세션 ID(outOnlineSessionID)를 돌려준다 - 부르는 쪽이 그 세션을 끊고, 새 접속은 잠시 뒤 다시 로그인한다
	// 차단 기간이 남았으면 Banned
	eLoginResult Login(const std::string& id, const uint64_t sessionID, UniquePtr<Account> loaded, Account** outAccount, uint64_t* outOnlineSessionID);
	void Logout(const std::string& id, const uint64_t sessionID); // 접속 표시를 지운다 - 계정 데이터는 남는다
	Account* Find(const std::string& id);                         // 운영 명령용 - 읽은 적 없는 계정이면 nullptr
	// 운영자 회수: 메모리의 모든 계정에서 그 아이템을 찾아 꺼낸다(계정 표 락 → 계정마다 플레이어 락). 찾으면 그 플레이어 락 안에서 onTaken을 부른다(DB 저장을 같은 락 안에서)
	bool TakeItemFromAnyone(const uint64_t itemUID, const std::function<void(Account& owner, const ItemInfo& item)>& onTaken);
	uint64_t GetOnlineSessionID(const std::string& id);           // 접속 중이 아니면 0
	int64_t GetOnlineCnt() const;                                 // 통계 - 로그인한 아이디 수
	int64_t GetAccountCnt() const;                                // 통계 - 메모리에 올린 계정 수(내리지 않으므로 계속 는다)

private:
	mutable std::mutex mLock;                                      // 계정 표 락(통계 읽기도 잡는다) - 서로 다른 세션이 같은 아이디로 동시에 로그인할 수 있다
	std::unordered_map<std::string, UniquePtr<Account>> mAccounts; // UniquePtr - Account는 mutex가 있어 옮길 수 없고, Player가 주소를 들고 있어 주소가 바뀌면 안 된다
	std::unordered_map<std::string, uint64_t> mOnline;             // 접속 중인 아이디 → 세션 ID
};
