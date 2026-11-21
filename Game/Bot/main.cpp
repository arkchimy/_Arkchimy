#include <WinSock2.h>
#include <WS2tcpip.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <deque>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>
#include "GameProtocol.h"
#include "LoginClient.h"
#include "PacketHeader.h"
#include "SecureChannel.h"
#include "SessionAuth.h"

#pragma comment(lib, "ws2_32.lib")

// 장시간 시험용 봇 - 계정마다 스레드 하나로 로그인 서버 → 게임 서버 입장 → 채팅 → 가끔 스스로 끊고 다시 들어가기를 반복한다
// 사용: GameBot.exe <봇 수> <시간(초)> [아이디 접두어] [게임 서버 IP] [포트] [로그인 서버 포트] [chat|churn] [로그인 서버 IP]
// chat(기본) = 입장 → 채팅 → 가끔 스스로 끊기. churn = 입장하자마자 끊고 바로 다시(입장 폭주 - 채팅 안 함)
// 5초마다 [lat] 줄: 그 구간에 돌아온 내 채팅의 왕복 시간(보낸 때 → 방송을 받은 때, ms) p50·p99·최대와 입장 수
// 처음에 한 번 가입하고(이미 있으면 그대로), 접속할 때마다 로그인 서버에서 세션 키를 받아 게임 서버에 들어간다
// 판정에 쓰는 값: 예상밖끊김(입장한 뒤 봇이 끊지 않았는데 서버가 끊음), 프로토콜오류(모르는 종류·길이, 풀리지 않는 암호문, 내 채팅의 순서·누락),
// 인증실패(가입·로그인 서버 실패) - 모두 0이어야 한다

using Clock = std::chrono::steady_clock;

enum : int32_t
{
	MAX_BOT = 600,
	SELECT_WAIT_MS = 50,
	RECONNECT_MIN_SEC = 30, // 스스로 끊는 간격 - 3분 시험에서도 다시 들어가기를 여러 번 지나게
	RECONNECT_MAX_SEC = 90,
};

struct Stats
{
	std::atomic<int64_t> Logins{ 0 };
	std::atomic<int64_t> ChatSent{ 0 };
	std::atomic<int64_t> ChatReceived{ 0 };
	std::atomic<int64_t> SelfDisconnects{ 0 };
	std::atomic<int64_t> UnexpectedClose{ 0 }; // 판정
	std::atomic<int64_t> ProtocolErrors{ 0 };  // 판정
	std::atomic<int64_t> AuthFails{ 0 };       // 판정 - 가입·로그인 서버 실패
	std::atomic<int64_t> EntryClosed{ 0 };     // 입장 요청 뒤 응답 전에 서버가 끊음 - 게임 서버는 입장 실패면 응답 없이 끊는다. 스스로 끊은 직후 다시 들어오면
	                                           // 이전 접속이 정리 중이라 "이미 접속 중"으로 끊길 수 있어 판정에 넣지 않는다(진짜 실패는 서버 통계 입장확인실패로 본다)
	std::atomic<int64_t> Connected{ 0 };
};

static Stats sStats;
static std::atomic<bool> sbStop = false;
// 내 채팅이 돌아오기까지(ms, 5초마다 [lat] 줄로 비움), churn 모드 = 입장하자마자 끊고 다시 들어가기(채팅 안 함)
static std::mutex sLatLock;
static std::vector<double> sLatMs;
static bool sbChurn = false;
static std::string sIP = "127.0.0.1";
static int sPort = 33000;
static LoginServerAddr sLoginAddr; // IP·포트·서버 고정 공개 키(server_public.cfg)
static const char BOT_PASSWORD[] = "bot-password"; // 봇 계정의 비밀번호(시험용)

static bool sendRaw(const SOCKET sock, const char* data, const int len)
{
	int sent = 0;
	while (sent < len)
	{
		const int ret = send(sock, data + sent, len - sent, 0);
		if (ret <= 0)
		{
			return false;
		}
		sent += ret;
	}
	return true;
}

// 헤더(길이 2바이트)를 붙여 보낸다
static bool sendPayload(const SOCKET sock, const void* payload, const size_t len)
{
	char buf[PACKET_HEADER_SIZE + MAX_PAYLOAD_SIZE];
	const PacketHeader header{ static_cast<uint16_t>(len) };
	memcpy(buf, &header, PACKET_HEADER_SIZE);
	memcpy(buf + PACKET_HEADER_SIZE, payload, len);
	return sendRaw(sock, buf, static_cast<int>(PACKET_HEADER_SIZE + len));
}

class Bot
{
public:
	Bot(const int idx, const std::string& prefix)
		: mID(prefix + std::to_string(idx))
		, mRng(static_cast<uint32_t>(idx * 7919 + 17))
	{
	}

	void Run()
	{
		eRegisterResult registerResult = eRegisterResult::ServerError;
		if (RequestRegister(sLoginAddr, mID, BOT_PASSWORD, &registerResult) != eLoginCall::Ok
			|| (registerResult != eRegisterResult::Ok && registerResult != eRegisterResult::Exists))
		{
			++sStats.AuthFails;
			fprintf(stderr, "[bot] %s 가입 실패(결과 %d)\n", mID.c_str(), static_cast<int>(registerResult));
		}
		while (!sbStop)
		{
			if (mSock == INVALID_SOCKET && Clock::now() >= mReconnectAt)
			{
				connectAndEnter();
			}
			if (mSock == INVALID_SOCKET)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
				continue;
			}
			pump();
			if (mSock != INVALID_SOCKET)
			{
				act();
			}
		}
		closeSocket(true);
	}

private:
	int randInt(const int lo, const int hi)
	{
		return std::uniform_int_distribution<int>(lo, hi)(mRng);
	}

	void connectAndEnter()
	{
		// 로그인 서버에서 세션 키를 받는다(별도 접속) - 30초 안에 게임 서버에 들어가야 한다
		AuthRes auth{};
		if (RequestAuth(sLoginAddr, mID, BOT_PASSWORD, &auth) != eLoginCall::Ok || auth.Result != eAuthResult::Ok)
		{
			++sStats.AuthFails;
			fprintf(stderr, "[bot] %s 로그인 서버 실패(결과 %d)\n", mID.c_str(), static_cast<int>(auth.Result));
			mReconnectAt = Clock::now() + std::chrono::seconds(1);
			return;
		}
		mSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		SOCKADDR_IN addr{};
		addr.sin_family = AF_INET;
		addr.sin_port = htons(static_cast<u_short>(sPort));
		inet_pton(AF_INET, sIP.c_str(), &addr.sin_addr);
		if (connect(mSock, reinterpret_cast<SOCKADDR*>(&addr), sizeof(addr)) == SOCKET_ERROR)
		{
			closesocket(mSock);
			mSock = INVALID_SOCKET;
			mReconnectAt = Clock::now() + std::chrono::seconds(1);
			return;
		}
		++sStats.Connected;
		mRecvBuf.clear();
		mbLoggedIn = false;
		mSentSeq = 0;
		mNextExpectedSeq = 0;
		mSentAt.clear();
		mSelfDisconnectAt = Clock::now() + std::chrono::seconds(randInt(RECONNECT_MIN_SEC, RECONNECT_MAX_SEC));
		// ① 접속하자마자 Hello(평문) - 입장 요청은 Challenge(SeqNumber 시작값)를 받은 뒤(onChallenge). 세션 키는 그때까지만 보관한다
		copyPendingKey(auth.SessionKey);
		mAccountNo = auth.AccountNo;
		SecureZeroMemory(auth.SessionKey, sizeof(auth.SessionKey));
		mbSecure = false;
		mbWaitChallenge = true;
		const Hello hello{ eGameType::Hello, auth.AccountNo };
		sendPayload(mSock, &hello, sizeof(hello));
	}

	void copyPendingKey(const uint8_t (&key)[SESSION_KEY_LEN])
	{
		memcpy(mPendingKey, key, sizeof(mPendingKey));
	}

	// ③ Challenge를 받으면 시작값으로 키를 정하고 입장 요청(통일 모양)을 보낸다. 보내기·받기를 이 봇의 스레드 하나가 하므로 키 객체 하나로 두 방향을 센다
	void onChallenge(const Challenge& challenge)
	{
		std::string packet;
		MakeEnterRequest(mPendingKey, challenge.StartSeq, mAccountNo, &mCipher, &packet);
		SecureZeroMemory(mPendingKey, sizeof(mPendingKey));
		mbWaitChallenge = false;
		sendPayload(mSock, packet.data(), packet.size());
	}
	// bSelf: 봇이 스스로 끊음(아니면 서버가 끊은 것)
	void closeSocket(const bool bSelf)
	{
		if (mSock == INVALID_SOCKET)
		{
			return;
		}
		if (bSelf && randInt(0, 1) == 0)
		{
			const LINGER linger{ 1, 0 }; // 절반은 RST로 끊는다(클라 강제 종료와 같다)
			setsockopt(mSock, SOL_SOCKET, SO_LINGER, reinterpret_cast<const char*>(&linger), sizeof(linger));
		}
		closesocket(mSock);
		mSock = INVALID_SOCKET;
		mCipher.Clear();
		SecureZeroMemory(mPendingKey, sizeof(mPendingKey));
		mbSecure = false;
		mbWaitChallenge = false;
		--sStats.Connected;
		mReconnectAt = Clock::now() + std::chrono::milliseconds(sbChurn ? randInt(0, 200) : randInt(500, 2000));
	}

	// 받은 것을 모두 꺼내 처리한다(최대 SELECT_WAIT_MS 기다림)
	void pump()
	{
		fd_set readSet;
		FD_ZERO(&readSet);
		FD_SET(mSock, &readSet);
		timeval wait{ 0, SELECT_WAIT_MS * 1000 };
		if (select(0, &readSet, nullptr, nullptr, &wait) <= 0)
		{
			return;
		}
		char buf[16384];
		const int ret = recv(mSock, buf, sizeof(buf), 0);
		if (ret <= 0)
		{
			if (!mbLoggedIn)
			{
				++sStats.EntryClosed; // 입장 실패 - 서버가 응답 없이 끊었다. 잠시 뒤 로그인 서버부터 다시
				fprintf(stderr, "[bot] %s 입장 요청 뒤 서버가 끊음(입장 실패)\n", mID.c_str());
			}
			else
			{
				++sStats.UnexpectedClose; // 봇이 끊지 않았는데 끊김
				fprintf(stderr, "[bot] %s 서버가 끊음(입장 뒤, 보낸 채팅 %llu)\n", mID.c_str(), static_cast<unsigned long long>(mSentSeq));
			}
			closeSocket(false);
			return;
		}
		mRecvBuf.append(buf, static_cast<size_t>(ret));
		size_t pos = 0;
		while (mRecvBuf.size() - pos >= PACKET_HEADER_SIZE)
		{
			PacketHeader header;
			memcpy(&header, mRecvBuf.data() + pos, PACKET_HEADER_SIZE);
			if (header.Len > MAX_PAYLOAD_SIZE)
			{
				++sStats.ProtocolErrors;
				closeSocket(true);
				return;
			}
			if (mRecvBuf.size() - pos < PACKET_HEADER_SIZE + header.Len)
			{
				break;
			}
			// Challenge만 평문, 그 뒤는 모두 통일 모양(SeqNumber·태그 확인). 입장에 실패하면 서버는 응답 없이 끊는다
			const char* const body = mRecvBuf.data() + pos + PACKET_HEADER_SIZE;
			if (mbWaitChallenge)
			{
				Challenge challenge{};
				if (header.Len == sizeof(Challenge))
				{
					memcpy(&challenge, body, sizeof(challenge));
				}
				if (challenge.Type != eGameType::Challenge)
				{
					++sStats.ProtocolErrors;
					fprintf(stderr, "[bot] %s Challenge가 아닌 패킷(길이 %u)\n", mID.c_str(), static_cast<unsigned>(header.Len));
					closeSocket(true);
					return;
				}
				onChallenge(challenge);
			}
			else
			{
				eGameType type = eGameType::Hello;
				std::string plain;
				if (!mCipher.OpenPacket(body, header.Len, &type, &plain))
				{
					++sStats.ProtocolErrors; // 서버 메시지의 SeqNumber·태그가 틀림 - 정상이면 0
					fprintf(stderr, "[bot] %s 서버 메시지를 풀지 못함(입장 %d, 길이 %u)\n", mID.c_str(), mbSecure ? 1 : 0, static_cast<unsigned>(header.Len));
					closeSocket(true);
					return;
				}
				onPacket(type, plain);
			}
			pos += PACKET_HEADER_SIZE + header.Len;
			if (mSock == INVALID_SOCKET)
			{
				return;
			}
		}
		mRecvBuf.erase(0, pos);
	}

	void onPacket(const eGameType type, const std::string& body)
	{
		const int chatTextLen = static_cast<int>(body.size()) - static_cast<int>(offsetof(ChatNotifyBody, Text));
		if (type == eGameType::LoginRes && !mbSecure && body.size() == sizeof(LoginResBody))
		{
			LoginResBody res;
			memcpy(&res, body.data(), sizeof(res));
			if (res.Result != eLoginResult::Ok)
			{
				++sStats.ProtocolErrors; // 서버는 실패를 응답하지 않고 끊는다 - 실패 응답은 오지 않는다
				fprintf(stderr, "[bot] %s 입장 응답이 성공이 아님(결과 %d)\n", mID.c_str(), static_cast<int>(res.Result));
				closeSocket(true);
				return;
			}
			mbSecure = true;
			mbLoggedIn = true;
			++sStats.Logins;
			if (sbChurn)
			{
				++sStats.SelfDisconnects;
				closeSocket(true); // churn - 입장만 반복
				return;
			}
		}
		else if (type == eGameType::ChatNotify && mbLoggedIn && chatTextLen >= 1 && chatTextLen <= CHAT_TEXT_MAX)
		{
			ChatNotifyBody notify;
			memcpy(&notify, body.data(), body.size());
			++sStats.ChatReceived;
			if (strncmp(notify.SenderID, mID.c_str(), ACCOUNT_ID_LEN) == 0)
			{
				// 내 채팅 - 보낸 순서대로 빠짐없이 돌아와야 한다(본문 = 보낸 번호). 방 락 안에서 방송하므로 내 것끼리 순서가 바뀌지 않는다
				const uint64_t seq = std::strtoull(std::string(notify.Text, static_cast<size_t>(chatTextLen)).c_str(), nullptr, 10);
				if (seq != mNextExpectedSeq)
				{
					++sStats.ProtocolErrors;
					fprintf(stderr, "[bot] %s 내 채팅 순서 어긋남(받은 %llu, 기대 %llu)\n", mID.c_str(), static_cast<unsigned long long>(seq),
						static_cast<unsigned long long>(mNextExpectedSeq));
				}
				mNextExpectedSeq = seq + 1;
				if (!mSentAt.empty())
				{
					const double rttMs = std::chrono::duration<double, std::milli>(Clock::now() - mSentAt.front()).count();
					mSentAt.pop_front();
					std::lock_guard<std::mutex> lock(sLatLock);
					sLatMs.push_back(rttMs);
				}
			}
		}
		else
		{
			++sStats.ProtocolErrors;
			fprintf(stderr, "[bot] %s 모르는 패킷 type=%u len=%zu\n", mID.c_str(), static_cast<unsigned>(type), body.size());
		}
	}
	// 입장한 뒤 0.5–3초마다 채팅 하나 - 본문은 이 접속에서 보낸 번호(돌아온 것으로 순서를 본다)
	void act()
	{
		const Clock::time_point now = Clock::now();
		if (now >= mSelfDisconnectAt)
		{
			++sStats.SelfDisconnects;
			closeSocket(true);
			return;
		}
		if (!mbLoggedIn || now < mNextActAt)
		{
			return;
		}
		mNextActAt = now + std::chrono::milliseconds(randInt(500, 3000));
		const std::string text = std::to_string(mSentSeq);
		std::string sealed;
		mCipher.SealPacket(eGameType::ChatReq, text.data(), text.size(), &sealed);
		if (sendPayload(mSock, sealed.data(), sealed.size()))
		{
			mSentAt.push_back(Clock::now());
			++mSentSeq;
			++sStats.ChatSent;
		}
	}

	const std::string mID;
	std::mt19937 mRng;
	SOCKET mSock = INVALID_SOCKET;
	std::string mRecvBuf;
	bool mbLoggedIn = false;
	ChannelCipher mCipher;          // 입장 요청부터 세션 키 하나로 - 이 봇의 스레드만 쓰고 보내는·받는 SeqNumber를 따로 센다
	bool mbSecure = false;          // 입장 성공 응답을 받음 - 그 뒤 보내기·받기는 통일 모양
	bool mbWaitChallenge = false;   // Hello를 보내고 Challenge를 기다림
	uint8_t mPendingKey[SESSION_KEY_LEN] = {}; // Challenge를 받을 때까지만 보관
	uint64_t mAccountNo = 0;
	uint64_t mSentSeq = 0;          // 이 접속에서 보낸 채팅 수
	uint64_t mNextExpectedSeq = 0;  // 돌아올 내 다음 채팅 번호
	std::deque<Clock::time_point> mSentAt; // 보낸 채팅의 시각(돌아오는 순서 = 보낸 순서)
	Clock::time_point mNextActAt = Clock::now();
	Clock::time_point mReconnectAt = Clock::now();
	Clock::time_point mSelfDisconnectAt = Clock::now();
};

static void printStats(const char* tag)
{
	// 행동 = 보낸 채팅(장시간 시험이 매 분 늘어나는지로 멈춤을 본다)
	printf("%s 접속=%lld 로그인=%lld 행동=%lld 받은채팅=%lld 스스로끊음=%lld 예상밖끊김=%lld 프로토콜오류=%lld 인증실패=%lld 입장거절끊김=%lld\n",
		tag, sStats.Connected.load(), sStats.Logins.load(), sStats.ChatSent.load(), sStats.ChatReceived.load(), sStats.SelfDisconnects.load(),
		sStats.UnexpectedClose.load(), sStats.ProtocolErrors.load(), sStats.AuthFails.load(), sStats.EntryClosed.load());
	fflush(stdout);
}

static void runBot(const int idx, const std::string prefix)
{
	Bot bot(idx, prefix);
	bot.Run();
}

int main(int argc, char* argv[])
{
	if (argc < 3)
	{
		printf("사용: GameBot.exe <봇 수> <시간(초)> [아이디 접두어] [게임 서버 IP] [포트] [로그인 서버 포트] [chat|churn] [로그인 서버 IP]\n");
		return 1;
	}
	const int botCnt = atoi(argv[1]);
	const int durationSec = atoi(argv[2]);
	const std::string prefix = (argc > 3) ? argv[3] : "bot";
	sIP = (argc > 4) ? argv[4] : sIP;
	sPort = (argc > 5) ? atoi(argv[5]) : sPort;
	sLoginAddr.IP = sIP;
	sLoginAddr.Port = (argc > 6) ? atoi(argv[6]) : 33002;
	sbChurn = (argc > 7) && strcmp(argv[7], "churn") == 0; // chat(기본) 또는 churn
	sLoginAddr.IP = (argc > 8) ? argv[8] : sIP;          // 로그인 서버가 다른 PC면 - 기본은 게임 서버와 같은 IP
	if (!InitCrypto() || !LoadServerPublicKey("server_public.cfg", sLoginAddr.SignPublicKey))
	{
		printf("암호 초기화 실패, 또는 server_public.cfg(serverPublicKey)가 없음\n");
		return 1;
	}
	if (botCnt < 1 || botCnt > MAX_BOT || durationSec < 1 || prefix.size() + 3 >= ACCOUNT_ID_LEN)
	{
		printf("봇 수 1~%d, 시간 1초 이상, 접두어는 12바이트 이하\n", MAX_BOT);
		return 1;
	}
	WSADATA wsa;
	WSAStartup(MAKEWORD(2, 2), &wsa);
	printf("[bot] %d개, %d초, 접두어 %s, %s:%d\n", botCnt, durationSec, prefix.c_str(), sIP.c_str(), sPort);

	std::vector<std::thread> threads;
	for (int i = 0; i < botCnt; ++i)
	{
		threads.emplace_back(runBot, i, prefix);
		std::this_thread::sleep_for(std::chrono::milliseconds(20)); // 한꺼번에 몰리지 않게
	}
	const Clock::time_point start = Clock::now();
	const Clock::time_point end = start + std::chrono::seconds(durationSec);
	int64_t lastLogins = 0;
	while (Clock::now() < end)
	{
		std::this_thread::sleep_for(std::chrono::seconds(5));
		// 5초 구간의 내 채팅 왕복 시간과 입장 수
		std::vector<double> lat;
		{
			std::lock_guard<std::mutex> lock(sLatLock);
			lat.swap(sLatMs);
		}
		std::sort(lat.begin(), lat.end());
		const size_t n = lat.size();
		const double p50 = (n > 0) ? lat[n / 2] : 0.0;
		const double p99 = (n > 0) ? lat[(std::min)(n - 1, n * 99 / 100)] : 0.0;
		const double maxMs = (n > 0) ? lat[n - 1] : 0.0;
		const int64_t logins = sStats.Logins.load();
		const long long elapsed = std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - start).count();
		printf("[lat] t=%lld n=%zu p50=%.2f p99=%.2f max=%.2f 입장=%lld\n", elapsed, n, p50, p99, maxMs, logins - lastLogins);
		lastLogins = logins;
		printStats("[bot]");
	}
	sbStop = true;
	for (std::thread& t : threads)
	{
		t.join();
	}
	printStats("[bot-result]");
	const bool bPass = sStats.UnexpectedClose == 0 && sStats.ProtocolErrors == 0 && sStats.AuthFails == 0;
	printf("[bot-result] %s\n", bPass ? "PASS" : "FAIL");
	WSACleanup();
	return bPass ? 0 : 2;
}
