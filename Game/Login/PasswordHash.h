#pragma once
#include <cstdint>
#include <string>

// 비밀번호 저장 - Argon2id + 솔트 + 페퍼(설계 login_server.docx 4장)
// 해시는 일부러 느리다(수십 ms, 큰 메모리) - 워커가 아니라 인증 스레드에서만 부른다
struct PasswordHashConfig
{
	std::string Pepper;          // 32바이트(login.cfg에 16진수) - DB 밖의 서버 비밀. 출력하지 않는다
	uint64_t OpsLimit = 2;       // Argon2id 반복 횟수
	size_t MemLimit = 64ull << 20; // Argon2id 메모리(바이트)
};

class PasswordHasher final
{
public:
	explicit PasswordHasher(const PasswordHashConfig& config);

	// 결과 문자열에 알고리즘·버전·설정·솔트(해시할 때마다 16바이트 무작위)·해시가 다 들어 있다 - DB에는 이 문자열 하나만
	bool Hash(const std::string& password, std::string* outHash) const;
	bool Verify(const std::string& password, const std::string& hash) const;
	bool NeedsRehash(const std::string& hash) const; // 지금 설정보다 옛 설정으로 만든 해시 - 로그인 성공 때 다시 해시해 저장
	void VerifyDummy(const std::string& password) const; // 없는 아이디도 해시 한 번만큼 걸리게(지금 설정으로 만든 가짜 해시라 옛 설정 계정과는 시간이 다를 수 있다)

private:
	void pepperPassword(const std::string& password, unsigned char* outMac) const; // HMAC-SHA-256(페퍼, 비밀번호)

	PasswordHashConfig mConfig;
	std::string mDummyHash; // 시작 때 한 번 만든다
};
