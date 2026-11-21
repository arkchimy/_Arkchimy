#include <sodium.h>
#include "SessionAuth.h"
#include "Memory.h"
#include "SecureChannel.h"

static_assert(sizeof(uint8_t[SESSION_KEY_LEN]) == CHANNEL_KEY_LEN, "세션 키를 그대로 통로 키(AES-256)로 쓴다");

bool InitCrypto()
{
	return sodium_init() >= 0; // 1이면 이미 됨
}

void MakeEnterRequest(const uint8_t (&sessionKey)[SESSION_KEY_LEN], const uint64_t startSeq, const uint64_t accountNo, ChannelCipher* outCipher, std::string* outPacket)
{
	Assert(outCipher != nullptr && outPacket != nullptr, "MakeEnterRequest: outCipher 또는 outPacket이 null");
	outCipher->SetKey(sessionKey, false, startSeq);
	const LoginReqBody body{ accountNo };
	outCipher->SealPacket(eGameType::LoginReq, &body, sizeof(body), outPacket); // 클라 방향 SeqNumber = 시작값
}

bool OpenEnterRequest(const uint8_t (&sessionKey)[SESSION_KEY_LEN], const uint64_t startSeq, const uint64_t helloAccountNo,
	const char* packet, const size_t packetLen, ChannelCipher* outCipher)
{
	Assert(outCipher != nullptr, "OpenEnterRequest: outCipher가 null");
	outCipher->SetKey(sessionKey, true, startSeq);
	eGameType type = eGameType::Hello;
	std::string body;
	bool bOk = outCipher->OpenPacket(packet, packetLen, &type, &body) // SeqNumber·태그가 틀리면(키가 다름·바뀜·다른 연결) 실패
		&& type == eGameType::LoginReq && body.size() == sizeof(LoginReqBody);
	if (bOk)
	{
		LoginReqBody req;
		MemCpy(&req, body.data(), sizeof(req));
		bOk = (req.AccountNo == helloAccountNo); // 키로 확인된 AccountNo = 평문 Hello의 AccountNo
	}
	if (!bOk)
	{
		outCipher->Clear();
	}
	return bOk;
}
