#pragma once
#include <WinSock2.h>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include "GameProtocol.h"
#include "SecureChannel.h"

class Account;

// 게임 서버의 접속자 한 명(세션 단위) - 로그인 뒤 계정과 연결된다 - 세션과 같은 인덱스의 배열 칸을 다시 쓴다
// 같은 세션의 콜백은 동시에 불리지 않으므로 대부분 락 없이 고친다
// 예외: 처음 보는 아이디의 로그인은 DB 스레드가 계정을 읽어 연결한다 - 계정, 로그인 중 표시, 통로 열림(DB 스레드가 열고 워커가 읽음)이 atomic
// 암호 통로(설계 login_server.docx 5-2): 보내기는 아무 스레드(워커·타이머·DB)에서 오므로 송신 락 안에서 "세션 ID 확인 → 순번 → 암호화 → 송신 링"을 한 번에,
// 받기(Open)는 이 세션의 워커만 한다
class Player final
{
public:
	void Init(const uint64_t sessionID, const SOCKADDR_IN& remoteAddr); // OnAccept에서 - 이 연결의 EnterChallenge를 만든다
	void Clear();                                                        // OnRelease에서
	uint64_t GetSessionID() const;                                       // 0이면 빈 칸
	void SetAccount(Account* account);                                   // 로그인 성공 때(워커 또는 DB 스레드)
	Account* GetAccount() const;                                         // nullptr이면 로그인 전
	void SetLoginPending(const bool bPending);                           // DB에서 계정을 읽는 중 - 그동안 온 로그인은 잘못된 패킷
	bool IsLoginPending() const;
	void SetPartyID(const uint64_t partyID);                             // 이 세션의 워커에서만 쓴다
	uint64_t GetPartyID() const;                                         // 0이면 파티 없음. 힌트일 뿐 - 진짜 판단은 파티 락 안에서(설계 3-8-3)

	void GetChallenge(uint8_t (&outChallenge)[ENTER_CHALLENGE_LEN]) const; // 이 연결의 무작위 값(Init 뒤 바뀌지 않는다)
	bool IsSecure() const;                                                 // 입장에 성공해 암호 통로가 열림
	std::mutex& GetSendLock();
	// 송신 락을 잡은 채로 부른다: 칸이 아직 sessionID의 것이면 true. 통로가 열렸으면 *outPayload를 암호문으로, 열리기 전이면 EnterChallenge·LoginRes만 평문으로(다른 것은 false)
	bool PrepareSendLocked(const uint64_t sessionID, const char* data, const int32_t len, std::string* outPayload);
	// 송신 락을 잡은 채로 부른다: 통로를 연다(그 뒤의 보내기는 모두 암호문)
	void OpenChannelLocked(const uint8_t (&sessionKey)[SESSION_KEY_LEN]);
	bool OpenReceived(const char* cipher, const int32_t len, std::string* outPlain); // 이 세션의 워커만
	// 감시 스레드: 접속한 뒤 limitMs가 지나도록 입장하지 않았으면(입장 처리 중은 빼고) 그 세션 ID, 아니면 0
	uint64_t GetEntryExpiredSessionID(const ULONGLONG limitMs);

private:
	std::atomic<Account*> mAccount = nullptr; // 계정 표가 갖고 있는 계정 - 계정은 지워지지 않으므로 주소가 그대로다. 다 만든 계정만 넣는다
	std::atomic<bool> mbLoginPending = false;
	uint64_t mPartyID = 0;   // 지금은 틀릴 일이 없다(남이 나를 빼거나 파티를 깨는 경로가 없음). 강퇴·해산이 생길 때를 대비해 쓸 때마다 파티 락 안에서 확인한다
	uint64_t mSessionID = 0; // 0이면 빈 칸 - 세션 ID는 시퀀스 번호가 1부터라, 한 칸이 2^32번 재사용되기 전까지는 0이 나오지 않는다. 바꿀 때는 송신 락 안에서
	SOCKADDR_IN mRemoteAddr{};
	uint8_t mChallenge[ENTER_CHALLENGE_LEN] = {};
	ULONGLONG mAcceptTick = 0;         // 접속 시각 - 입장 기한을 센다. 송신 락 안에서 쓰고 읽는다
	std::mutex mSendLock;              // 이 칸의 송신(순번·암호화·SendPacket)과 세션 ID·통로를 바꾸는 일을 묶는다
	ChannelCipher mCipher;             // 보내기 쪽은 송신 락 안에서, 받기 쪽은 이 세션의 워커만
	std::atomic<bool> mbSecure = false; // 통로가 열림 - 워커가 받은 패킷을 풀지 결정할 때 읽는다
};
