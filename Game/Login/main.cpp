#include <algorithm>
#include <atomic>
#include <chrono>
#include <conio.h>
#include <cstdio>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <sodium.h>
#include "LoginServer.h"
#include "CrushDump_lib.h" // LoginServer.h(WinSock2.h) 뒤에 - Windows.h를 먼저 끌어오면 WinSock2.h와 재정의 오류가 난다
#include "ParserLib.h"

// db.cfg(.gitignore)에서 MySQL·Redis 접속 설정. 비밀번호는 출력하지 않는다
static bool loadDbConfig(const char* path, DbConfig* outDb, RedisConfig* outRedis)
{
	Assert(outDb != nullptr && outRedis != nullptr, "loadDbConfig: out 매개변수가 null");
	utility::stDebug debug;
	const std::unique_ptr<utility::Parser> parser = utility::Parser::CreateParser(path, debug);
	if (parser == nullptr)
	{
		fprintf(stderr, "[main] %s를 읽지 못함 err=%d - db.cfg.example을 복사해 값을 채운다\n", path, debug.err);
		return false;
	}
	if (!parser->Get("host", outDb->Host) || !parser->Get("port", outDb->Port) || !parser->Get("user", outDb->User)
		|| !parser->Get("password", outDb->Password) || !parser->Get("database", outDb->Database))
	{
		fprintf(stderr, "[main] %s에 host·port·user·password·database가 모두 있어야 한다\n", path);
		return false;
	}
	if (!LoadRedisConfig(*parser, outRedis))
	{
		fprintf(stderr, "[main] %s에 redisHost·redisPort·redisPassword가 있어야 한다(db.cfg.example 참고)\n", path);
		return false;
	}
	printf("[main] DB: %s@%s:%d/%s, Redis: %s:%d\n", outDb->User.c_str(), outDb->Host.c_str(), outDb->Port, outDb->Database.c_str(),
		outRedis->Host.c_str(), outRedis->Port);
	return true;
}

// login.cfg(.gitignore) - 페퍼(32바이트 16진수)와 Argon2id 설정
static bool loadHashConfig(const char* path, PasswordHashConfig* outConfig)
{
	Assert(outConfig != nullptr, "loadHashConfig: outConfig가 null");
	utility::stDebug debug;
	const std::unique_ptr<utility::Parser> parser = utility::Parser::CreateParser(path, debug);
	if (parser == nullptr)
	{
		fprintf(stderr, "[main] %s를 읽지 못함 err=%d - login.cfg.example을 복사해 값을 채운다\n", path, debug.err);
		return false;
	}
	std::string pepperHex;
	int32_t ops = 0;
	int32_t memMiB = 0;
	if (!parser->Get("pepper", pepperHex) || !parser->Get("argonOps", ops) || !parser->Get("argonMemMiB", memMiB))
	{
		fprintf(stderr, "[main] %s에 pepper·argonOps·argonMemMiB가 있어야 한다\n", path);
		return false;
	}
	unsigned char pepper[32];
	size_t pepperLen = 0;
	if (sodium_hex2bin(pepper, sizeof(pepper), pepperHex.c_str(), pepperHex.size(), nullptr, &pepperLen, nullptr) != 0 || pepperLen != sizeof(pepper))
	{
		fprintf(stderr, "[main] pepper는 16진수 64글자(32바이트)여야 한다\n");
		return false;
	}
	// OWASP 최소(19MiB, 2회)보다 약하게는 두지 않는다
	if (ops < 1 || ops > 10 || memMiB < 19 || memMiB > 1024 || (memMiB < 46 && ops < 2))
	{
		fprintf(stderr, "[main] argonOps 1-10, argonMemMiB 19-1024, 그리고 46MiB 미만이면 반복 2회 이상\n");
		return false;
	}
	outConfig->Pepper.assign(reinterpret_cast<const char*>(pepper), sizeof(pepper));
	sodium_memzero(pepper, sizeof(pepper));
	outConfig->OpsLimit = static_cast<uint64_t>(ops);
	outConfig->MemLimit = static_cast<size_t>(memMiB) << 20;
	return true;
}

// --bench: 메모리 × 반복마다 해시 한 번 시간(10번 중앙값) - 설정값을 정할 때(설계 4장)
static void runHashBench()
{
	const int32_t memList[] = { 19, 32, 46, 64, 128 };
	const int32_t opsList[] = { 1, 2, 3, 4 };
	unsigned char out[32];
	unsigned char salt[crypto_pwhash_SALTBYTES] = {};
	const char password[] = "benchmark-password";
	printf("메모리(MiB)\t반복\t중앙값(ms)\t최소\t최대\n");
	for (const int32_t memMiB : memList)
	{
		for (const int32_t ops : opsList)
		{
			std::vector<double> times;
			for (int32_t i = 0; i < 10; ++i)
			{
				const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
				crypto_pwhash(out, sizeof(out), password, sizeof(password) - 1, salt, static_cast<unsigned long long>(ops),
					static_cast<size_t>(memMiB) << 20, crypto_pwhash_ALG_ARGON2ID13);
				times.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
			}
			std::sort(times.begin(), times.end());
			printf("%d\t%d\t%.1f\t%.1f\t%.1f\n", memMiB, ops, (times[4] + times[5]) / 2, times.front(), times.back());
		}
	}
}

// 메모리·핸들 - 장시간 시험에서 로그인 서버도 조금씩 새는지 본다(게임 서버 main과 같은 형식)
static void printProcessInfo(std::ostream& out)
{
	PROCESS_MEMORY_COUNTERS_EX memory{};
	GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory));
	DWORD handleCnt = 0;
	GetProcessHandleCount(GetCurrentProcess(), &handleCnt);
	char buf[64];
	snprintf(buf, sizeof(buf), "메모리=%lluMB 핸들=%lu ", static_cast<unsigned long long>(memory.PrivateUsage / (1024 * 1024)), handleCnt);
	out << buf;
}

enum : int32_t
{
	SIGN_KEY_BLOB_MAX = 2048, // CNG RSA 2048 비밀 키 형식은 약 1,200바이트
};

// login.cfg의 signSecretKey(서버 고정 RSA 비밀 키, CNG 형식의 16진수) - HelloClient와 응답에 서명. 출력하지 않는다
static bool loadSignKey(const char* path, RsaKey* outKey)
{
	Assert(outKey != nullptr, "loadSignKey: outKey가 null");
	utility::stDebug debug;
	const std::unique_ptr<utility::Parser> parser = utility::Parser::CreateParser(path, debug);
	std::string hex;
	std::string blob(SIGN_KEY_BLOB_MAX, '\0');
	size_t blobLen = 0;
	bool bOk = parser != nullptr && parser->Get("signSecretKey", hex)
		&& sodium_hex2bin(reinterpret_cast<unsigned char*>(blob.data()), blob.size(), hex.c_str(), hex.size(), nullptr, &blobLen, nullptr) == 0;
	if (bOk)
	{
		blob.resize(blobLen);
		uint8_t modulus[RSA_MODULUS_LEN];
		bOk = outKey->ImportPrivateBlob(blob) && outKey->ExportPublic(modulus); // 2048비트·e = 65537인지까지
	}
	sodium_memzero(hex.data(), hex.size()); // 실패 경로에서도 지운다
	sodium_memzero(blob.data(), blob.size());
	if (!bOk)
	{
		fprintf(stderr, "[main] %s에 signSecretKey(RSA 2048 비밀 키 16진수)가 있어야 한다 - LoginServer.exe --genkey로 만든다\n", path);
	}
	return bOk;
}

static std::string toHex(const void* data, const size_t size)
{
	std::string hex(size * 2 + 1, '\0');
	sodium_bin2hex(hex.data(), hex.size(), static_cast<const unsigned char*>(data), size);
	hex.pop_back();
	return hex;
}

// --genkey: 서버 고정 RSA 키 쌍을 만든다 - 비밀 키는 login.cfg에, 공개 키(n)는 클라·봇이 읽는 server_public.cfg에 넣는다
static bool printNewSignKey()
{
	RsaKey key;
	std::string blob;
	uint8_t modulus[RSA_MODULUS_LEN];
	if (!key.Generate() || !key.ExportPrivateBlob(&blob) || !key.ExportPublic(modulus))
	{
		fprintf(stderr, "[main] RSA 키 쌍을 만들지 못함\n");
		return false;
	}
	std::string secretHex = toHex(blob.data(), blob.size());
	printf("signSecretKey = %s;\nserverPublicKey = %s;\n", secretHex.c_str(), toHex(modulus, sizeof(modulus)).c_str());
	sodium_memzero(blob.data(), blob.size());
	sodium_memzero(secretHex.data(), secretHex.size());
	return true;
}

// --bench: RSA 2048 계산 한 번의 시간(중앙값·최대) - 설계 login_rsa_change.docx 2-7
enum class eRsaBenchOp
{
	Generate,
	Sign,
	Verify,
	Lock,
	Unlock,
};

struct RsaBench
{
	RsaKey Key;
	uint8_t Locked[RSA_MODULUS_LEN] = {};
	uint8_t Sign[RSA_SIGN_LEN] = {};
	uint8_t Message[RSA_MODULUS_LEN * 2 + 8] = {};
	uint8_t Plain[sizeof(AuthReq)] = {};
	std::string Unlocked;
};

static bool runRsaOnce(RsaBench* outBench, const eRsaBenchOp op)
{
	Assert(outBench != nullptr, "runRsaOnce: outBench가 null");
	bool bOk = false;
	switch (op)
	{
	case eRsaBenchOp::Generate:
		bOk = outBench->Key.Generate();
		break;
	case eRsaBenchOp::Sign:
		bOk = outBench->Key.Sign(outBench->Message, sizeof(outBench->Message), outBench->Sign);
		break;
	case eRsaBenchOp::Verify:
		bOk = outBench->Key.Verify(outBench->Message, sizeof(outBench->Message), outBench->Sign);
		break;
	case eRsaBenchOp::Lock:
		bOk = outBench->Key.Lock(outBench->Plain, sizeof(outBench->Plain), outBench->Locked);
		break;
	case eRsaBenchOp::Unlock:
		bOk = outBench->Key.Unlock(outBench->Locked, &outBench->Unlocked);
		break;
	default:
		Assert(false, "runRsaOnce: 모르는 계산");
		break;
	}
	return bOk;
}

static void measureRsa(RsaBench* outBench, const char* name, const eRsaBenchOp op, const int32_t count)
{
	Assert(outBench != nullptr, "measureRsa: outBench가 null");
	std::vector<double> times;
	bool bOk = true;
	for (int32_t i = 0; i < count && bOk; ++i)
	{
		const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
		bOk = runRsaOnce(outBench, op);
		times.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
	}
	if (!bOk)
	{
		printf("%s 실패\n", name);
		return;
	}
	std::sort(times.begin(), times.end());
	printf("%s\t%d\t%.3f\t%.3f\n", name, count, times[times.size() / 2], times.back());
}

static void runRsaBench()
{
	RsaBench bench; // 키 쌍 만들기가 먼저 - 뒤의 계산은 그 키로(서명 확인은 바로 앞 서명, 풀기는 바로 앞 잠그기)
	printf("계산\t횟수\t중앙값(ms)\t최대(ms)\n");
	measureRsa(&bench, "키 쌍 만들기", eRsaBenchOp::Generate, 40);
	measureRsa(&bench, "서명", eRsaBenchOp::Sign, 300);
	measureRsa(&bench, "서명 확인", eRsaBenchOp::Verify, 300);
	measureRsa(&bench, "잠그기", eRsaBenchOp::Lock, 300);
	measureRsa(&bench, "풀기", eRsaBenchOp::Unlock, 300);
}

static void runMonitorLoop(const LoginServer& server, const std::atomic<bool>& bMonitorRunning)
{
	while (bMonitorRunning)
	{
		Sleep(1000);
		printProcessInfo(std::cout);
		std::cout << server << std::endl;
	}
}

int main(int argc, char* argv[])
{
	CDump dump;
	if (sodium_init() < 0)
	{
		fprintf(stderr, "[main] libsodium 초기화 실패\n");
		return 1;
	}
	if (argc > 1 && std::string(argv[1]) == "--bench")
	{
		if (argc > 2 && std::string(argv[2]) == "rsa")
		{
			runRsaBench();
		}
		else
		{
			runHashBench();
		}
		return 0;
	}
	if (argc > 1 && std::string(argv[1]) == "--genkey")
	{
		return printNewSignKey() ? 0 : 1;
	}
	PasswordHashConfig hashConfig;
	if (!loadHashConfig("login.cfg", &hashConfig))
	{
		return 1;
	}
	if (argc > 2 && std::string(argv[1]) == "--hash")
	{
		// 시험용: 비밀번호의 저장 형식 문자열(이 PC의 페퍼로) - 시험 스크립트가 credentials에 직접 넣을 때
		const PasswordHasher hasher(hashConfig);
		std::string hash;
		if (!hasher.Hash(argv[2], &hash))
		{
			return 1;
		}
		printf("%s\n", hash.c_str());
		return 0;
	}

	printf("[main] Argon2id: 반복 %llu회, 메모리 %zuMiB\n", static_cast<unsigned long long>(hashConfig.OpsLimit), hashConfig.MemLimit >> 20); // --hash는 해시 한 줄만 출력하도록 여기서
	const WsadataRAII wsa;
	NetConfig config;
	NetError configError;
	if (!LoadNetConfig("server.cfg", &config, &configError))
	{
		fprintf(stderr, "[main] server.cfg를 읽지 못함 - 단계=%s 코드=%d\n", configError.Step != nullptr ? configError.Step : "?", configError.Code);
		return 1;
	}
	int32_t loginPort = 0;
	int32_t loginIdleSec = 10; // 접속·응답 뒤 이 시간 안에 요청이 없으면 끊는다(0이면 끔). 클라는 요청할 때만 접속하므로 짧게
	int32_t keyPoolSize = 256; // 서버 임시 키 쌍 수 - 시작할 때 만들고, 다 쓴 키는 돌려준 워커가 새로 만들어 채운다. HelloServer를 보낸 뒤 끊기 전인 연결 수의 상한이기도 하다
	int32_t keyMaxUse = 2;     // 서버 임시 키 하나를 몇 연결까지 쓸지(설계 암호화_구조_결정.docx 2-3, 2026-11-21 5 → 2) - 1이면 접속마다 새 키
	int32_t ackSec = 5;        // 로그인 성공 응답(세션 키)을 보낸 뒤 이 시간 안에 클라의 AuthAck가 없으면 끊는다
	{
		utility::stDebug debug;
		const std::unique_ptr<utility::Parser> parser = utility::Parser::CreateParser("server.cfg", debug);
		if (parser == nullptr || !parser->Get("loginPort", loginPort) || loginPort <= 0 || loginPort > UINT16_MAX)
		{
			fprintf(stderr, "[main] server.cfg에 loginPort(1-65535)가 있어야 한다\n");
			return 1;
		}
		parser->Get("loginIdleSec", loginIdleSec);
		if (loginIdleSec < 0 || loginIdleSec > 3600)
		{
			fprintf(stderr, "[main] server.cfg의 loginIdleSec는 0-3600\n");
			return 1;
		}
		parser->Get("loginKeyPool", keyPoolSize);
		if (keyPoolSize < 1 || keyPoolSize > 1024)
		{
			fprintf(stderr, "[main] server.cfg의 loginKeyPool은 1-1024\n");
			return 1;
		}
		parser->Get("loginKeyMaxUse", keyMaxUse);
		if (keyMaxUse < 1 || keyMaxUse > 100)
		{
			fprintf(stderr, "[main] server.cfg의 loginKeyMaxUse는 1-100\n");
			return 1;
		}
		parser->Get("loginAckSec", ackSec);
		if (ackSec < 1 || ackSec > 60)
		{
			fprintf(stderr, "[main] server.cfg의 loginAckSec는 1-60\n");
			return 1;
		}
	}
	config.Port = static_cast<USHORT>(loginPort); // 나머지 네트워크 설정은 게임 서버와 같게

	DbConfig dbConfig;
	RedisConfig redisConfig;
	if (!loadDbConfig("db.cfg", &dbConfig, &redisConfig))
	{
		return 1;
	}
	// 가입·로그인은 워커가 처리한다 - 워커 수만큼 DB·Redis 연결 쌍을 만들어 두고 요청마다 빌린다
	LoginStorePool stores;
	std::string error;
	if (!stores.Connect(config.WorkerCnt, dbConfig, redisConfig, &error))
	{
		fprintf(stderr, "[main] DB·Redis 연결 실패 - %s\n", error.c_str());
		return 1;
	}
	// 계정 번호(AccountNo)는 가입할 때 MySQL이 준다. 게임 데이터(accounts 등)는 게임 서버가 첫 입장 때 만든다
	if (!stores.GetStartupDb().Exec("CREATE TABLE IF NOT EXISTS credentials ("
		" account_no BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,"
		" account_id VARBINARY(15) NOT NULL UNIQUE,"
		" password_hash VARCHAR(128) NOT NULL,"
		" created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP)"))
	{
		fprintf(stderr, "[main] credentials 테이블을 만들지 못함\n");
		return 1;
	}
	RsaKey signKey; // 서버 고정 키 - 서버보다 먼저 만들고 나중에 없어진다
	if (!loadSignKey("login.cfg", &signKey))
	{
		return 1;
	}
	// 서버 임시 키 쌍을 main 스레드가 미리 만든다(설계 login_rsa_change.docx 2-7) - loginKeyPool개(기본 256) × 약 55ms라 시작이 약 14초 늦어진다
	RsaKeyPool keyPool(keyPoolSize, keyMaxUse);
	if (!keyPool.Fill())
	{
		fprintf(stderr, "[main] 서버 임시 키 쌍을 만들지 못함(%d개 만든 뒤 CNG 실패)\n", keyPool.GetReadyCnt());
		return 1;
	}
	const PasswordHasher hasher(hashConfig);

	LoginServer server(config.MaxSession, &stores, &hasher, &signKey, &keyPool, loginIdleSec, ackSec);
	if (!server.Start(config))
	{
		const NetError startError = server.GetNetError();
		fprintf(stderr, "[main] 로그인 서버 시작 실패 - 단계=%s 코드=%d\n", startError.Step != nullptr ? startError.Step : "?", startError.Code);
		return 1;
	}
	printf("[main] 로그인 서버 포트 %d, 요청 기한 %d초, 확인 기한 %d초, 서버 임시 키 쌍 %d개(키 하나를 %d연결까지)\n", loginPort, loginIdleSec, ackSec, keyPoolSize, keyMaxUse);

	std::atomic<bool> bMonitorRunning = true;
	std::thread monitorThread(runMonitorLoop, std::cref(server), std::cref(bMonitorRunning));
	while (server.IsRunning())
	{
		if (_kbhit())
		{
			const int key = _getch();
			if (key == 'q' || key == 'Q')
			{
				break;
			}
		}
		Sleep(100);
	}
	bMonitorRunning = false;
	monitorThread.join();
	std::cout << "[main] 종료 전 " << server << std::endl;
	server.Shutdown();
	printf("[main] 종료 완료\n");
	return 0;
}
