#pragma once
#include <cstdint>
#include <mutex>
#include <string>

// 계정 하나 - DB accounts 행과 같다(아이디, 차단 시각). 처음 입장할 때 DB에서 읽거나 만들고, 서버가 끝날 때까지 내리지 않는다
// 차단 시각은 운영 명령(DB 스레드)이 바꾸고 입장(DB 스레드)이 읽는다 - 계정 락으로 지킨다
class Account final
{
public:
	explicit Account(const std::string& id);

	const std::string& GetID() const; // 만든 뒤 바뀌지 않아 락 없이 읽는다
	std::mutex& GetLock();            // 계정 락

	// 아래는 계정 락을 잡은 채로 부른다
	int64_t GetBanUntil() const; // 유닉스 시각(초) - 이 시각까지 입장 차단, 0이면 없음
	void SetBanUntil(const int64_t banUntil);

private:
	std::mutex mLock;
	const std::string mID;
	int64_t mBanUntil = 0;
};
