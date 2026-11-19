#include "LoginStorePool.h"
#include "Assert.h"

bool LoginStorePool::Connect(const int32_t count, const DbConfig& dbConfig, const RedisConfig& redisConfig, std::string* outError)
{
	Assert(count > 0 && outError != nullptr && mStores.empty(), "LoginStorePool::Connect: count가 0 이하, outError가 null, 또는 두 번 부름");
	bool bOk = true;
	for (int32_t i = 0; bOk && i < count; ++i)
	{
		UniquePtr<LoginStore> store = MakeUnique<LoginStore>();
		bOk = store->Db.Connect(dbConfig, i == 0, outError) && store->Redis.Connect(redisConfig, outError);
		if (bOk)
		{
			mFree.push_back(store.get());
			mStores.push_back(std::move(store));
		}
	}
	return bOk;
}

DbConn& LoginStorePool::GetStartupDb()
{
	Assert(!mStores.empty(), "LoginStorePool::GetStartupDb: 연결 전");
	return mStores[0]->Db;
}

LoginStore* LoginStorePool::Take()
{
	std::lock_guard<std::mutex> lock(mLock);
	LoginStore* store = nullptr;
	if (!mFree.empty())
	{
		store = mFree.back();
		mFree.pop_back();
	}
	return store;
}

void LoginStorePool::Return(LoginStore* store)
{
	Assert(store != nullptr, "LoginStorePool::Return: store가 null");
	std::lock_guard<std::mutex> lock(mLock);
	mFree.push_back(store);
}

StoreLease::StoreLease(LoginStorePool& pool)
	: mPool(pool)
	, mStore(pool.Take())
{
}

StoreLease::~StoreLease()
{
	if (mStore != nullptr)
	{
		mPool.Return(mStore);
	}
}
