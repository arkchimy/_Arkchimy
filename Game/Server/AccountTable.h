#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>
#include "Account.h"
#include "GameProtocol.h"
#include "Memory.h"

// 아이디 → 계정. 로그인·로그아웃과 중복 로그인 막기
class AccountTable
{
public:
	// 계정 표 락 안에서: 처음 보는 아이디면 계정을 만들어 시작 골드·아이템을 주고, 이미 접속 중이면 거절, 아니면 접속 표시
	eLoginResult Login(const std::string& id, const uint64_t sessionID, Account** outAccount);
	void Logout(const std::string& id, const uint64_t sessionID); // 접속 표시를 지운다 - 계정 데이터는 남는다

private:
	uint64_t newItemUID(); // 아이템 고유 번호 - DB를 붙이면 DB에서 받는다

	std::mutex mLock;                                              // 계정 표 락 - 서로 다른 세션이 같은 아이디로 동시에 로그인할 수 있다
	std::unordered_map<std::string, UniquePtr<Account>> mAccounts; // UniquePtr - Account는 mutex가 있어 옮길 수 없고, Player가 주소를 들고 있어 주소가 바뀌면 안 된다
	std::unordered_map<std::string, uint64_t> mOnline;             // 접속 중인 아이디 → 세션 ID
	std::atomic<uint64_t> mNextItemUID = 1;                        // 0은 "비어 있음"(장비 칸)이라 1부터
};
