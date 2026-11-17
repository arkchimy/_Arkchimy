#include <memory>
#include "RsaEchoServer.h"
#include "ParserLib.h"
#include "Profiler.h" // NetworkLib.h(WinSock2.h) 뒤에 - Windows.h가 먼저 들어오면 재정의 오류(README_ERRORS 1-26)

RsaEchoServer::RsaEchoServer(const int32_t maxSession)
	: mSessions(maxSession)
	, mKeyPool(RSA_ECHO_KEY_MAX_USE) // 빈 채로 시작 - 워커가 필요할 때 만든다
{
}

RsaEchoServer::~RsaEchoServer()
{
	Stop(); // 파생 소멸자에서 멈춘다 - 기반 소멸자 시점엔 파생 부분이 없어 OnRelease가 순수 가상 호출이 된다
	mKeyPool.Clear();
}

bool RsaEchoServer::LoadSignKey(const char* secretCfgPath)
{
	// 서버 고정 키 - 로그인 서버의 loadSignKey와 같다. cfg의 16진수를 CNG 비밀 키 형식으로 바꿔 읽고, 2048비트·e = 65537인지까지 본다
	utility::stDebug debug;
	const std::unique_ptr<utility::Parser> parser = utility::Parser::CreateParser(secretCfgPath, debug);
	std::string hex;
	std::string blob;
	uint8_t modulus[RSA_MODULUS_LEN]; // ExportPublic의 결과는 버리고 형식 검사로만 쓴다 - ImportPrivateBlob은 4096비트·다른 e도 받아들이는데,
	                                  // 프로토콜은 n 256바이트만 보내고 e = 65537로 정해 두었다. 잘못된 키면 접속마다가 아니라 시작할 때 실패하게
	const bool bOk = parser != nullptr && parser->Get("signSecretKey", hex) && HexToBytes(hex, &blob)
		&& mSignKey.ImportPrivateBlob(blob) && mSignKey.ExportPublic(modulus);
	SecureZeroMemory(hex.data(), hex.size()); // 비밀 키 사본 - 실패 경로에서도 지운다
	SecureZeroMemory(blob.data(), blob.size());
	return bOk;
}

void RsaEchoServer::OnAccept(const uint64_t id, const SOCKADDR_IN& remoteAddr)
{
	EchoSession& session = getSession(id);
	Assert(session.SessionID == 0, "OnAccept: 이전 세션이 정리되지 않음");
	session.SessionID = id;
	session.Channel.Clear(); // 임시 키는 이 연결의 ClientHello에서 새로 받는다
}

void RsaEchoServer::OnRecv(const uint64_t id, const char* payload, const int32_t len)
{
	EchoSession& session = getSession(id);
	if (!session.Channel.IsReady())
	{
		onClientHello(id, session, payload, len); // 첫 패킷은 임시 공개 키 주고받기
	}
	else
	{
		onEchoReq(id, session, payload, len);
	}
}

void RsaEchoServer::OnRelease(const uint64_t id)
{
	EchoSession& session = getSession(id);
	Assert(session.SessionID == id, "OnRelease: 다른 세션");
	UniquePtr<RsaKey> serverKey = session.Channel.ReleaseMine(); // Hello 전에 끊겼으면 빌린 키가 없다
	if (serverKey != nullptr)
	{
		mKeyPool.Return(std::move(serverKey), session.KeyUseCnt); // 5번 썼으면 풀이 지우고 새로 만든다
	}
	session.KeyUseCnt = 0;
	session.Channel.Clear();
	session.SessionID = 0;
}

EchoSession& RsaEchoServer::getSession(const uint64_t id)
{
	const uint32_t index = static_cast<uint32_t>(id); // 라이브러리의 세션 ID = (시퀀스 번호 << 32) | 인덱스
	Assert(index < mSessions.size(), "getSession: 인덱스 범위 밖 - 생성자의 maxSession과 Start의 MaxSession이 다름");
	return mSessions[index];
}

void RsaEchoServer::onClientHello(const uint64_t id, EchoSession& session, const char* payload, const int32_t len)
{
	if (len != static_cast<int32_t>(sizeof(ClientHello)))
	{
		disconnectBadPacket(id);
		return;
	}
	ClientHello hello;
	MemCpy(&hello, payload, sizeof(hello));
	UniquePtr<RsaKey> serverKey;
	if (!mKeyPool.Take(&serverKey, &session.KeyUseCnt))
	{
		++mKeyEmptyCnt; // 풀이 비어 새로 만들다 실패(CNG) - 그 접속은 거절
		Disconnect(id);
		return;
	}
	ServerHello reply{};
	bool bOk = false;
	{
		ProfileScope scope("S Hello(sign)"); // 대부분 서버 고정 키 서명 1번
		bOk = session.Channel.AcceptClient(hello, &mSignKey, std::move(serverKey), &reply);
	}
	if (!bOk)
	{
		disconnectBadPacket(id); // 잘못된 클라 공개 키
		return;
	}
	++mHelloCnt;
	SendPacket(id, reinterpret_cast<const char*>(&reply), static_cast<int32_t>(sizeof(reply)));
}

void RsaEchoServer::onEchoReq(const uint64_t id, EchoSession& session, const char* payload, const int32_t len)
{
	std::string plain;
	bool bOpened = false;
	{
		ProfileScope scope("S Open(verify+unlock)"); // 클라 서명 확인(공개 키) + 서버 임시 키로 풀기(비밀 키)
		bOpened = session.Channel.Open(payload, static_cast<size_t>(len), &plain);
	}
	if (!bOpened)
	{
		disconnectBadPacket(id); // 길이·서명·번호·패딩이 틀림
		return;
	}
	std::string sealed;
	bool bSealed = false;
	{
		ProfileScope scope("S Seal(lock+sign)"); // 클라 임시 키로 잠그기(공개 키) + 서버 고정 키로 서명(비밀 키)
		bSealed = session.Channel.Seal(plain.data(), plain.size(), &sealed);
	}
	if (!bSealed)
	{
		disconnectBadPacket(id);
		return;
	}
	++mEchoCnt;
	SendPacket(id, sealed.data(), static_cast<int32_t>(sealed.size()));
}

void RsaEchoServer::disconnectBadPacket(const uint64_t id)
{
	++mBadPacketCnt;
	Disconnect(id);
}

std::ostream& operator<<(std::ostream& out, const RsaEchoServer& server)
{
	const NetStat stat = server.GetStat();
	out << "[stat] 세션=" << server.GetSessionCnt()
		<< " 수락=" << server.GetAcceptCnt()
		<< " 해제=" << server.GetReleaseCnt()
		<< " Hello=" << server.GetHelloCnt()          // 누적
		<< " 에코=" << server.GetEchoCnt()            // 누적
		<< " 키쌍=" << server.GetReadyKeyCnt()        // 풀에 있는 서버 임시 키 쌍(빌려줄 수 있음)
		<< " 빌려줌=" << server.GetLentKeyCnt()       // 세션이 쓰는 중
		<< " 키생성=" << server.GetCreatedKeyCnt()    // 풀이 비어 워커가 새로 만든 수(누적)
		<< " 키교체=" << server.GetReplacedKeyCnt()   // 5번 써서 버린 수(누적)
		<< " 키생성실패끊김=" << server.GetKeyEmptyCnt()
		<< " 비정상헤더=" << stat.BadPacketCnt
		<< " 잘못된패킷=" << server.GetBadPacketCnt();
	return out;
}
