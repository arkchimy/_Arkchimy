#pragma once
#include <WinSock2.h>
#include <cstdint>
#include <string>

namespace utility
{
	class Parser;
}

struct RedisConfig
{
	std::string Host = "127.0.0.1";
	int32_t Port = 6379;
	std::string Password; // db.cfg에만 둔다 - 출력하지 않는다
};

// db.cfg의 redisHost·redisPort·redisPassword - 하나라도 없으면 false
bool LoadRedisConfig(const utility::Parser& parser, RedisConfig* outConfig);

// Redis 클라이언트 - 필요한 명령(AUTH·SET EX·GET·조건부 DEL)만 직접 구현(설계 login_server.docx 3장)
// 블록 소켓 연결 하나, 명령 하나 보내고 응답 하나 읽기. 한 번에 한 스레드만 쓴다 - 게임 서버는 DB 스레드, 로그인 서버는 연결을 빌린 워커(LoginStorePool)
// 블록되므로 부르는 동안 그 스레드는 다른 일을 못 한다(연결 1초·응답 1초 제한)
// 연결·응답은 1초 시간 제한. 연결 실패·시간 초과 뒤 5초는 Redis를 부르지 않고 바로 실패한다 - 멈춘 Redis가 스레드(게임 서버는 저장도 하는 DB 스레드)를 계속 붙잡지 않게
// 끊기면 다음 명령 전에 다시 붙는다. SET·GET은 끊기면 다시 붙어 한 번 더, 지우기는 다시 하지 않는다
// (첫 지우기가 지우고 응답만 잃었으면 두 번째는 0을 돌려 진짜 사용자를 거절하므로 - 실패로 돌려 클라가 다시 로그인하게)
class RedisClient final
{
public:
	~RedisClient();

	bool Connect(const RedisConfig& config, std::string* outError);
	void Close();

	bool SetEx(const std::string& key, const std::string& value, const int32_t ttlSec);
	// 값이 없으면 true + *outbFound = false
	bool Get(const std::string& key, std::string* outValue, bool* outbFound);
	// 키의 값이 value와 같을 때만 지운다(EVAL로 한 번에) - 지운 키 수(0 또는 1). GET과 지우기 사이에 다른 쪽이 덮어쓴 값은 지우지 않는다
	bool DelIfEquals(const std::string& key, const std::string& value, int64_t* outRemovedCnt);

private:
	enum class eReply
	{
		Status,  // +OK
		Error,   // -ERR ...
		Integer, // :1
		Bulk,    // $n 뒤 n바이트
		Nil,     // $-1
	};

	bool connectSocket(std::string* outError);
	bool ensureConnected();
	// args를 RESP 배열로 보내고 응답 하나를 읽는다. 실패(끊김·형식 오류)면 false
	bool command(const std::string* args, const int32_t argCnt, eReply* outType, std::string* outValue, int64_t* outInteger);
	bool sendAll(const std::string& data);
	bool readLine(std::string* outLine); // \r\n 앞까지
	bool readExact(const size_t size, std::string* outData);

	SOCKET mSock = INVALID_SOCKET;
	RedisConfig mConfig;
	std::string mRecvBuf; // 받았지만 아직 읽지 않은 바이트
	size_t mRecvPos = 0;
	uint64_t mRetryAfterTick = 0; // 이 시각(GetTickCount64) 전에는 다시 붙지 않는다
};
