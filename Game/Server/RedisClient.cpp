#include <WinSock2.h>
#include <WS2tcpip.h>
#include <cstdio>
#include <cstdlib>
#include "RedisClient.h"
#include "Assert.h"
#include "ParserLib.h"

enum : int32_t
{
	// 연결·응답이 이보다 늦으면 실패로 본다. 같은 PC의 Redis는 1ms 안에 답하므로 짧게 - 이 스레드(DB·인증)의 다른 일이 그만큼 밀린다
	REDIS_CONNECT_TIMEOUT_MS = 1000,
	REDIS_TIMEOUT_MS = 1000,
	REDIS_RETRY_DELAY_MS = 5000, // 연결 실패·시간 초과 뒤 이 시간 동안은 Redis를 부르지 않고 바로 실패 - 멈춘 Redis가 스레드를 계속 붙잡지 않게
	REDIS_LINE_MAX = 1024,       // 상태·오류·숫자·길이 줄의 최대 길이 - 넘으면 형식 오류
	REDIS_BULK_MAX = 4096,       // 우리가 넣는 값(세션 키 + 아이디)보다 충분히 크게
};

// 읽은 값과 같을 때만 지운다(GET과 DEL 사이에 새 로그인이 키를 덮어써도 새 키를 지우지 않게) - 결과는 지운 수
static const char DEL_IF_EQUALS_SCRIPT[] = "if redis.call('GET', KEYS[1]) == ARGV[1] then return redis.call('DEL', KEYS[1]) else return 0 end";

bool LoadRedisConfig(const utility::Parser& parser, RedisConfig* outConfig)
{
	Assert(outConfig != nullptr, "LoadRedisConfig: outConfig가 null");
	return parser.Get("redisHost", outConfig->Host) && parser.Get("redisPort", outConfig->Port) && parser.Get("redisPassword", outConfig->Password);
}

RedisClient::~RedisClient()
{
	Close();
}

bool RedisClient::Connect(const RedisConfig& config, std::string* outError)
{
	Assert(outError != nullptr, "RedisClient::Connect: outError가 null");
	mConfig = config;
	return connectSocket(outError);
}

void RedisClient::Close()
{
	if (mSock != INVALID_SOCKET)
	{
		closesocket(mSock);
		mSock = INVALID_SOCKET;
	}
	mRecvBuf.clear();
	mRecvPos = 0;
}

bool RedisClient::connectSocket(std::string* outError)
{
	Close();
	SOCKADDR_IN addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(static_cast<u_short>(mConfig.Port));
	if (inet_pton(AF_INET, mConfig.Host.c_str(), &addr.sin_addr) != 1)
	{
		*outError = "redisHost는 IPv4 주소여야 한다";
		return false;
	}
	mSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	const DWORD timeout = REDIS_TIMEOUT_MS;
	setsockopt(mSock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
	setsockopt(mSock, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
	// connect에는 SO_SNDTIMEO가 걸리지 않는다(거절에 약 2초, 응답 없는 주소면 약 21초) - 비블록으로 걸고 select로 기다린다
	u_long bNonBlocking = 1;
	ioctlsocket(mSock, FIONBIO, &bNonBlocking);
	bool bConnected = connect(mSock, reinterpret_cast<const SOCKADDR*>(&addr), sizeof(addr)) != SOCKET_ERROR;
	int32_t err = bConnected ? 0 : WSAGetLastError();
	if (!bConnected && err == WSAEWOULDBLOCK)
	{
		fd_set writeSet;
		fd_set exceptSet;
		FD_ZERO(&writeSet);
		FD_ZERO(&exceptSet);
		FD_SET(mSock, &writeSet);
		FD_SET(mSock, &exceptSet);
		const timeval wait{ 0, REDIS_CONNECT_TIMEOUT_MS * 1000 };
		const int ready = select(0, nullptr, &writeSet, &exceptSet, &wait);
		bConnected = (ready > 0 && FD_ISSET(mSock, &writeSet));
		err = (ready == 0) ? WSAETIMEDOUT : WSAECONNREFUSED;
	}
	bNonBlocking = 0;
	ioctlsocket(mSock, FIONBIO, &bNonBlocking); // 명령은 블록 소켓 + 송수신 시간 제한으로
	if (!bConnected)
	{
		*outError = "connect err=" + std::to_string(err);
		Close();
		return false;
	}
	if (!mConfig.Password.empty())
	{
		const std::string args[] = { "AUTH", mConfig.Password };
		eReply type = eReply::Error;
		std::string value;
		int64_t integer = 0;
		if (!command(args, 2, &type, &value, &integer) || type != eReply::Status)
		{
			*outError = "AUTH 실패 " + value; // 값은 Redis의 오류 문장 - 비밀번호가 아니다
			Close();
			return false;
		}
	}
	return true;
}

bool RedisClient::ensureConnected()
{
	if (mSock != INVALID_SOCKET)
	{
		return true;
	}
	if (GetTickCount64() < mRetryAfterTick)
	{
		return false; // 방금 실패했다 - 기다리지 않고 바로 실패(클래스 주석)
	}
	std::string error;
	if (!connectSocket(&error))
	{
		mRetryAfterTick = GetTickCount64() + REDIS_RETRY_DELAY_MS;
		fprintf(stderr, "[redis] 다시 연결 실패 - %s\n", error.c_str());
		return false;
	}
	fprintf(stderr, "[redis] 다시 연결함\n");
	return true;
}

bool RedisClient::SetEx(const std::string& key, const std::string& value, const int32_t ttlSec)
{
	const std::string args[] = { "SET", key, value, "EX", std::to_string(ttlSec) };
	for (int32_t attempt = 0; attempt < 2; ++attempt) // 같은 값을 다시 넣어도 결과가 같다 - 끊기면 한 번 더
	{
		eReply type = eReply::Error;
		std::string reply;
		int64_t integer = 0;
		if (ensureConnected() && command(args, 5, &type, &reply, &integer))
		{
			return type == eReply::Status;
		}
	}
	return false;
}

bool RedisClient::Get(const std::string& key, std::string* outValue, bool* outbFound)
{
	Assert(outValue != nullptr && outbFound != nullptr, "RedisClient::Get: out 매개변수가 null");
	const std::string args[] = { "GET", key };
	for (int32_t attempt = 0; attempt < 2; ++attempt) // 읽기만 하므로 끊기면 한 번 더
	{
		eReply type = eReply::Error;
		int64_t integer = 0;
		if (ensureConnected() && command(args, 2, &type, outValue, &integer))
		{
			*outbFound = (type == eReply::Bulk);
			return type == eReply::Bulk || type == eReply::Nil;
		}
	}
	return false;
}

bool RedisClient::DelIfEquals(const std::string& key, const std::string& value, int64_t* outRemovedCnt)
{
	Assert(outRemovedCnt != nullptr, "RedisClient::DelIfEquals: outRemovedCnt가 null");
	const std::string args[] = { "EVAL", DEL_IF_EQUALS_SCRIPT, "1", key, value };
	eReply type = eReply::Error;
	std::string reply;
	if (!ensureConnected() || !command(args, 5, &type, &reply, outRemovedCnt)) // 다시 하지 않는다(클래스 주석)
	{
		return false;
	}
	return type == eReply::Integer;
}

bool RedisClient::command(const std::string* args, const int32_t argCnt, eReply* outType, std::string* outValue, int64_t* outInteger)
{
	// 보내기: *개수\r\n 뒤에 항목마다 $길이\r\n내용\r\n - 길이를 앞에 적어 어떤 바이트든 그대로 보낸다
	std::string out = "*" + std::to_string(argCnt) + "\r\n";
	for (int32_t i = 0; i < argCnt; ++i)
	{
		out += "$" + std::to_string(args[i].size()) + "\r\n";
		out += args[i];
		out += "\r\n";
	}
	std::string line;
	const bool bOk = sendAll(out) && readLine(&line) && !line.empty();
	SecureZeroMemory(out.data(), out.size()); // 값(세션 키)·AUTH 비밀번호가 들어 있었다
	if (!bOk)
	{
		if (WSAGetLastError() == WSAETIMEDOUT)
		{
			mRetryAfterTick = GetTickCount64() + REDIS_RETRY_DELAY_MS; // 멈춘 Redis - 다시 붙어 또 기다리지 않게
		}
		Close(); // 끊김·시간 초과 - 다음 명령 전에 다시 붙는다(받다 만 응답이 남지 않게 연결째 버린다)
		return false;
	}
	// 받기: 첫 글자로 종류를 구분한다
	const char kind = line[0];
	const std::string rest = line.substr(1);
	outValue->clear();
	*outInteger = 0;
	switch (kind)
	{
	case '+':
		*outType = eReply::Status;
		*outValue = rest;
		return true;
	case '-':
		*outType = eReply::Error;
		*outValue = rest;
		return true;
	case ':':
		*outType = eReply::Integer;
		*outInteger = _strtoi64(rest.c_str(), nullptr, 10);
		return true;
	case '$':
	{
		const int64_t size = _strtoi64(rest.c_str(), nullptr, 10);
		if (size == -1)
		{
			*outType = eReply::Nil;
			return true;
		}
		std::string tail;
		if (size < 0 || size > REDIS_BULK_MAX || !readExact(static_cast<size_t>(size), outValue) || !readExact(2, &tail) || tail != "\r\n")
		{
			Close();
			return false;
		}
		*outType = eReply::Bulk;
		return true;
	}
	default:
		Close(); // 우리 명령에는 배열 응답이 없다 - 형식 오류
		return false;
	}
}

bool RedisClient::sendAll(const std::string& data)
{
	size_t sent = 0;
	while (sent < data.size())
	{
		const int ret = send(mSock, data.data() + sent, static_cast<int>(data.size() - sent), 0);
		if (ret <= 0)
		{
			return false;
		}
		sent += static_cast<size_t>(ret);
	}
	return true;
}

bool RedisClient::readLine(std::string* outLine)
{
	while (true)
	{
		const size_t end = mRecvBuf.find("\r\n", mRecvPos);
		if (end != std::string::npos)
		{
			*outLine = mRecvBuf.substr(mRecvPos, end - mRecvPos);
			mRecvPos = end + 2;
			return true;
		}
		if (mRecvBuf.size() - mRecvPos > REDIS_LINE_MAX)
		{
			return false;
		}
		char buf[1024];
		const int ret = recv(mSock, buf, sizeof(buf), 0);
		if (ret <= 0)
		{
			return false;
		}
		mRecvBuf.erase(0, mRecvPos); // 읽은 앞부분을 버린다
		mRecvPos = 0;
		mRecvBuf.append(buf, static_cast<size_t>(ret));
	}
}

bool RedisClient::readExact(const size_t size, std::string* outData)
{
	while (mRecvBuf.size() - mRecvPos < size)
	{
		char buf[1024];
		const int ret = recv(mSock, buf, sizeof(buf), 0);
		if (ret <= 0)
		{
			return false;
		}
		mRecvBuf.erase(0, mRecvPos);
		mRecvPos = 0;
		mRecvBuf.append(buf, static_cast<size_t>(ret));
	}
	*outData = mRecvBuf.substr(mRecvPos, size);
	mRecvPos += size;
	return true;
}
