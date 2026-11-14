#pragma once
#include <WinSock2.h>
#include <cstdint>

class Account;

// 게임 서버의 접속자 한 명(세션 단위) - 로그인 뒤 계정과 연결된다 - 세션과 같은 인덱스의 배열 칸을 다시 쓴다
// 같은 세션의 콜백은 동시에 불리지 않으므로 락 없이 고친다
class Player final
{
public:
	void Init(const uint64_t sessionID, const SOCKADDR_IN& remoteAddr); // OnAccept에서
	void Clear();                                                        // OnRelease에서
	uint64_t GetSessionID() const;                                       // 0이면 빈 칸
	void SetAccount(Account* account);                                   // 로그인 성공 때
	Account* GetAccount() const;                                         // nullptr이면 로그인 전
	void SetPartyID(const uint64_t partyID);                             // 이 세션의 워커에서만 쓴다
	uint64_t GetPartyID() const;                                         // 0이면 파티 없음. 힌트일 뿐 - 진짜 판단은 파티 락 안에서(설계 3-8-3)

private:
	Account* mAccount = nullptr; // 계정 표가 갖고 있는 계정 - 계정은 지워지지 않으므로 주소가 그대로다
	uint64_t mPartyID = 0;       // 지금은 틀릴 일이 없다(남이 나를 빼거나 파티를 깨는 경로가 없음). 강퇴·해산이 생길 때를 대비해 쓸 때마다 파티 락 안에서 확인한다
	uint64_t mSessionID = 0; // 0이면 빈 칸 - 세션 ID는 시퀀스 번호가 1부터라, 한 칸이 2^32번 재사용되기 전까지는 0이 나오지 않는다
	SOCKADDR_IN mRemoteAddr{};
};
