#pragma once
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>
#include "DbThread.h"
#include "Memory.h"
#include "RedisClient.h"

// 워커가 쓰는 DB·Redis 연결 한 쌍 - DbConn·RedisClient는 한 번에 한 스레드만 쓰게 만들어져 있다
struct LoginStore
{
	DbConn Db;
	RedisClient Redis;
};

// 로그인 서버는 가입·로그인을 워커가 처리한다(2026-11-19 사용자 결정 - 인증 스레드 없음)
// 워커 수만큼 연결 쌍을 만들어 두고, 요청 하나를 처리하는 동안 빌린다. 워커만 빌리므로 정상이면 모자라지 않는다
class LoginStorePool final
{
public:
	LoginStorePool() = default;
	LoginStorePool(const LoginStorePool&) = delete;
	LoginStorePool& operator=(const LoginStorePool&) = delete;

	// count쌍을 연결한다. 첫 DB 연결만 데이터베이스가 없으면 만든다(서버 시작 때 한 번)
	bool Connect(const int32_t count, const DbConfig& dbConfig, const RedisConfig& redisConfig, std::string* outError);
	DbConn& GetStartupDb(); // 서버를 열기 전에만 - 테이블 만들기
	LoginStore* Take();     // 비었으면 nullptr
	void Return(LoginStore* store);

private:
	std::vector<UniquePtr<LoginStore>> mStores;
	std::vector<LoginStore*> mFree; // mLock 안에서
	std::mutex mLock;
};

// 요청 하나를 처리하는 동안 연결 쌍을 빌리고, 범위를 나가면 돌려준다
class StoreLease final
{
public:
	explicit StoreLease(LoginStorePool& pool);
	~StoreLease();
	StoreLease(const StoreLease&) = delete;
	StoreLease& operator=(const StoreLease&) = delete;

	LoginStore* Get() const
	{
		return mStore;
	}

private:
	LoginStorePool& mPool;
	LoginStore* mStore;
};
