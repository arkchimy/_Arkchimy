#pragma once
#include <atomic>
#include <ostream>
#include <vector>
#include "NetworkLib.h"
#include "EchoProtocol.h"
#include "Player.h"

// 에코 서버 - 요청(EchoReq)의 값을 응답(EchoRes)으로 돌려준다. 라이브러리 시험용 컨텐츠
class EchoServer final : public NetworkLib
{
public:
	explicit EchoServer(const int32_t maxSession); // Player 배열을 maxSession개 만든다 - Start에 넘길 MaxSession과 같은 값
	~EchoServer() override;

	int64_t GetBadPacketCnt() const // 컨텐츠가 잘못된 패킷(짧음, 모르는 종류, 종류와 맞지 않는 길이)으로 끊은 수 (누적)
	{
		return mBadPacketCnt;
	}

protected:
	void OnAccept(const uint64_t id, const SOCKADDR_IN& remoteAddr) override;
	void OnRecv(const uint64_t id, const char* payload, const int32_t len) override;
	void OnRelease(const uint64_t id) override;

private:
	Player& getPlayer(const uint64_t id);                                      // 세션 ID의 하위 32비트(인덱스)로 같은 칸을 찾는다
	void onEchoReq(const uint64_t id, const char* payload, const int32_t len); // EchoReq → EchoRes
	void disconnectBadPacket(const uint64_t id);                              // 잘못된 패킷 - 세고 끊는다

	std::vector<Player> mPlayers;          // 세션과 같은 인덱스 - 같은 인덱스는 이전 세션의 OnRelease가 끝난 뒤에만 다시 쓰인다
	std::atomic<int64_t> mBadPacketCnt = 0;  // 서로 다른 세션의 OnRecv가 여러 워커에서 동시에 올린다
};

// 모니터링 출력 - std::cout << server; 한 줄(줄바꿈은 부르는 쪽이 붙인다)
std::ostream& operator<<(std::ostream& out, const EchoServer& server);
