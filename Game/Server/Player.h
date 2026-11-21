#pragma once
#include <WinSock2.h>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include "GameProtocol.h"
#include "SecureChannel.h"

class Account;

// 연결 상태(흐름 게임서버_클라_흐름.docx 0-3) - 상태마다 받을 수 있는 것이 정해져 있고, 다른 것이 오면 끊는다
enum class eEntryState : uint8_t
{
	WaitHello = 0, // 접속함 - Hello만 받는다
	WaitEnter = 1, // Challenge를 보냄 - 입장 요청만 받는다
	Pending = 2,   // 입장 요청을 DB 스레드가 확인 중 - 아무것도 받지 않는다
	Entered = 3,   // 입장 - 통일 모양 메시지(채팅 등)
};

// 게임 서버의 접속자 한 명(세션 단위) - 세션과 같은 인덱스의 배열 칸을 다시 쓴다
// 같은 세션의 콜백(워커)은 동시에 불리지 않는다. 다른 스레드가 만지는 것: 입장 처리(DB 스레드 - 계정·상태·통로 키), 보내기(채팅을 방송하는 다른 세션의 워커·DB 스레드)
// 보내기는 송신 락 안에서 "세션 ID 확인 → SeqNumber → 암호화 → 송신 링"을 한 번에. 받기(OpenReceived)는 이 세션의 워커만
class Player final
{
public:
	void Init(const uint64_t sessionID, const SOCKADDR_IN& remoteAddr); // OnAccept에서 - 상태 = Hello 대기
	void Clear();                                                        // OnRelease에서
	uint64_t GetSessionID() const;                                       // 0이면 빈 칸
	void SetAccount(Account* account);                                   // 입장 성공 때(DB 스레드)
	Account* GetAccount() const;                                         // nullptr이면 입장 전
	eEntryState GetState() const;
	void SetState(const eEntryState state);                              // 이 세션의 워커가 WaitEnter → Pending으로 바꿀 때(Entered는 OpenChannelLocked)
	uint64_t GetHelloAccountNo() const;                                  // Hello로 받은 AccountNo - 워커가 적고 DB 스레드가 읽는다(대기열에 넣는 순서가 앞뒤를 정한다)
	uint64_t GetStartSeq() const;                                        // 이 연결의 SeqNumber 시작값

	std::mutex& GetSendLock();
	// 송신 락을 잡은 채로(워커, Hello 처리): AccountNo와 SeqNumber 시작값을 적고 상태 = 입장 요청 대기
	void AcceptHelloLocked(const uint64_t accountNo, const uint64_t startSeq);
	// 송신 락을 잡은 채로 부른다: 칸이 아직 sessionID의 것이고 입장했으면 *outPacket을 통일 모양 패킷으로 만들고 true. 입장 전이면 false(보내지 않는다)
	bool PrepareSendLocked(const uint64_t sessionID, const eGameType type, const void* body, const size_t bodyLen, std::string* outPacket);
	// 송신 락을 잡은 채로(DB 스레드, 입장 처리 중): 세션 키로 입장 요청을 푼다(SessionAuth::OpenEnterRequest). 틀리면 false(통로 키를 지움)
	bool BeginEntryLocked(const uint8_t (&sessionKey)[SESSION_KEY_LEN], const char* packet, const size_t packetLen);
	void CancelEntryLocked(); // 송신 락을 잡은 채로: 입장이 끝내 실패하면 통로 키를 지운다(그 연결은 바로 끊는다)
	void OpenChannelLocked(); // 송신 락을 잡은 채로: 상태 = 입장(BeginEntryLocked가 성공한 뒤 - 그 뒤의 보내기·받기는 모두 통일 모양)
	bool OpenReceived(const char* packet, const int32_t len, eGameType* outType, std::string* outBody); // 이 세션의 워커만
	// 감시 스레드: 접속한 뒤 limitMs가 지나도록 입장하지 않았으면(입장 처리 중은 빼고) 그 세션 ID, 아니면 0
	uint64_t GetEntryExpiredSessionID(const ULONGLONG limitMs);

private:
	std::atomic<Account*> mAccount = nullptr; // 계정 표가 갖고 있는 계정 - 계정은 지워지지 않으므로 주소가 그대로다. 다 만든 계정만 넣는다
	std::atomic<eEntryState> mState = eEntryState::WaitHello;
	uint64_t mHelloAccountNo = 0;
	uint64_t mStartSeq = 0;
	uint64_t mSessionID = 0; // 0이면 빈 칸 - 세션 ID는 시퀀스 번호가 1부터라, 한 칸이 2^32번 재사용되기 전까지는 0이 나오지 않는다. 바꿀 때는 송신 락 안에서
	SOCKADDR_IN mRemoteAddr{};
	ULONGLONG mAcceptTick = 0;         // 접속 시각 - 입장 기한을 센다. 송신 락 안에서 쓰고 읽는다
	std::mutex mSendLock;              // 이 칸의 송신(SeqNumber·암호화·SendPacket)과 세션 ID·상태·통로 키를 바꾸는 일을 묶는다
	ChannelCipher mCipher;             // 보내기 쪽은 송신 락 안에서, 받기 쪽은 이 세션의 워커만
};
