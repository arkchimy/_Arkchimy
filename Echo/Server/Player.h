#pragma once
#include <WinSock2.h>
#include <cstdint>

// 에코 서버의 접속자 한 명 - 세션과 같은 인덱스의 배열 칸을 다시 쓴다
// 같은 세션의 콜백은 동시에 불리지 않으므로 락 없이 고친다
class Player
{
public:
	void Init(const uint64_t sessionID, const SOCKADDR_IN& remoteAddr); // OnAccept에서
	void Clear();                                                        // OnRelease에서
	uint64_t GetSessionID() const;                                       // 0이면 빈 칸

private:
	uint64_t mSessionID = 0; // 0이면 빈 칸 - 세션 ID는 시퀀스 번호가 1부터라, 한 칸이 2^32번 재사용되기 전까지는 0이 나오지 않는다
	SOCKADDR_IN mRemoteAddr{};
};
