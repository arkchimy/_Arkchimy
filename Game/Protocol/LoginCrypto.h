#pragma once
#include <cstdint>
#include <mutex>
#include <string>
#include "LoginProtocol.h"
#include "Memory.h"

// 로그인 구간 암호화 - RSA 2048만 쓴다(설계 login_rsa_change.docx). Windows CNG(bcrypt)로 계산한다
// 키 쌍 셋: 서버 고정 키(서명만, 공개 키는 클라에 박혀 있음), 서버 임시 키·클라 임시 키(접속마다 새로 만들고 끝나면 지운다)
// 잠그기는 OAEP(SHA-256) - 매번 무작위를 섞어 같은 값도 다른 암호문, 서명은 SHA-256 + PSS

// RSA 키 하나(공개 키만, 또는 키 쌍). 같은 키를 여러 스레드가 동시에 써도 되도록 키마다 락을 건다
// (CNG가 같은 키 핸들의 동시 사용을 보장한다고 문서에 적혀 있지 않다 - 서버 고정 키는 모든 세션이 함께 쓴다)
class RsaKey final
{
public:
	RsaKey() = default;
	RsaKey(const RsaKey&) = delete; // 핸들이 둘이 되면 한쪽이 지운 뒤 다른 쪽이 쓴다
	RsaKey& operator=(const RsaKey&) = delete;
	~RsaKey();

	bool Generate();                                                      // 새 2048비트 키 쌍(e = 65537) - 수십 ms
	bool ImportPublic(const uint8_t (&modulus)[RSA_MODULUS_LEN]);         // 상대 공개 키(n만, e는 65537)
	bool ImportPrivateBlob(const std::string& blob);                      // login.cfg의 서버 고정 키(CNG 비밀 키 형식)
	bool ExportPublic(uint8_t (&outModulus)[RSA_MODULUS_LEN]) const;      // 2048비트·e = 65537이 아니면 false
	bool ExportPrivateBlob(std::string* outBlob) const;                   // --genkey
	void Clear();
	bool IsReady() const;

	bool Lock(const void* plain, const size_t plainLen, uint8_t (&outLocked)[RSA_MODULUS_LEN]) const; // 공개 키로 잠금. 평문은 RSA_MAX_PLAIN_LEN 이하
	bool Unlock(const uint8_t (&locked)[RSA_MODULUS_LEN], std::string* outPlain) const;               // 비밀 키로 풂. 패딩이 틀리면 false
	bool Sign(const void* message, const size_t messageLen, uint8_t (&outSign)[RSA_SIGN_LEN]) const;  // 비밀 키로 서명
	bool Verify(const void* message, const size_t messageLen, const uint8_t (&sign)[RSA_SIGN_LEN]) const;

private:
	void* mKey = nullptr; // BCRYPT_KEY_HANDLE - 헤더에서 Windows.h를 끌어오지 않으려고(README_ERRORS 1-26)
	mutable std::mutex mLock;
};

// 로그인 연결 하나의 암호 상태
//   ① 클라 → ClientHello(클라 임시 공개 키)
//   ② 서버 → ServerHello(서버 임시 공개 키 + 서버 고정 키로 [서버 임시 공개 키 | 클라 임시 공개 키]에 서명)
//   ③ 클라가 박아 둔 서버 고정 공개 키로 서명 확인
//   ④ 요청 = 서버 임시 공개 키로 잠금 + 클라 임시 비밀 키로 [잠근 요청 | 요청 번호]에 서명
//   ⑤ 응답 = 클라 임시 공개 키로 잠금 + 서버 고정 키로 [잠근 응답 | 클라 임시 공개 키 | 응답 번호]에 서명
// 번호는 방향마다 0부터 - 같은 연결에서 예전 요청·응답을 다시 보내면 서명이 맞지 않는다
// 보내기(Seal)와 받기(Open)는 서로 다른 스레드가 동시에 해도 된다(번호가 따로). 같은 방향을 두 스레드가 동시에 하지 않는다
class LoginChannel final
{
public:
	LoginChannel() = default;
	LoginChannel(const LoginChannel&) = delete;
	LoginChannel& operator=(const LoginChannel&) = delete;

	// 클라: 임시 키 쌍을 만들어 ClientHello를 채운다
	bool StartClient(ClientHello* outHello);
	// 클라: 서명을 박아 둔 서버 고정 공개 키로 확인한다. 틀리면(가짜 서버) false
	bool FinishClient(const ServerHello& hello, const uint8_t (&serverSignPublicKey)[RSA_MODULUS_LEN]);
	// 서버: 클라 임시 공개 키를 받고, 미리 만들어 둔 서버 임시 키 쌍(serverKey, 이 연결이 가져간다)으로 서명한 ServerHello를 채운다
	// signKey는 서버 고정 키 - 이 객체가 Clear될 때까지 살아 있어야 한다
	bool AcceptClient(const ClientHello& hello, const RsaKey* signKey, UniquePtr<RsaKey> serverKey, ServerHello* outHello);

	bool Seal(const void* plain, const size_t plainLen, std::string* outPacket); // 평문 → LOGIN_SEALED_LEN 바이트
	bool Open(const void* packet, const size_t packetLen, std::string* outPlain); // 서명·번호·패딩이 틀리면 false - 그 연결은 끊는다
	bool IsReady() const;                                                        // 임시 공개 키를 주고받음
	void Clear();
	// 서버: 내 임시 키 쌍을 꺼내 돌려준다(키를 돌려 쓰는 풀에 반납할 때 - RsaEcho). 꺼낸 뒤 이 통로는 쓰지 않고 Clear한다
	UniquePtr<RsaKey> ReleaseMine();

private:
	void makeSignedMessage(const uint8_t (&locked)[RSA_MODULUS_LEN], const uint64_t seq, const bool bFromServer, std::string* outMessage) const;

	bool mbServer = false;
	UniquePtr<RsaKey> mMine;                        // 내 임시 키 쌍 - 받은 것을 풀고, 클라는 요청에 서명도. Clear에서 지운다
	RsaKey mPeer;                                   // 상대 임시 공개 키 - 보낼 것을 잠그고, 서버는 요청 서명 확인도
	RsaKey mPinned;                                 // 클라만: 박아 둔 서버 고정 공개 키 - 응답 서명 확인
	const RsaKey* mServerSign = nullptr;            // 응답에 서명·확인하는 키(서버: 고정 키, 클라: mPinned)
	uint8_t mClientPublicKey[RSA_MODULUS_LEN] = {}; // 응답 서명에 넣는다 - 이 연결의 응답이라는 표시
	uint64_t mTxSeq = 0;
	uint64_t mRxSeq = 0;
};
