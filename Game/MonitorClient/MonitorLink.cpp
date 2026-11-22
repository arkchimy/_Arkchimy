#include <WinSock2.h>
#include <WS2tcpip.h>
#include <cstring>
#include "MonitorLink.h"

// 이 프로젝트는 /utf-8로 컴파일한다 - 한글 문자열 상수가 UTF-8이라 ImGui가 그대로 그린다

template <size_t N>
static std::string toText(const char (&text)[N])
{
	return std::string(text, strnlen(text, N));
}

template <size_t N>
static void copyText(char (&outText)[N], const std::string& text)
{
	memset(outText, 0, N);
	memcpy(outText, text.data(), (text.size() < N) ? text.size() : N - 1);
}

MonitorLink::~MonitorLink()
{
	Stop();
}

void MonitorLink::Start(const std::string& host, const int32_t port, const std::string& key)
{
	mHost = host;
	mPort = port;
	mKey = key;
	mbStopping = false;
	mThread = std::thread(&MonitorLink::run, this);
}

void MonitorLink::Stop()
{
	{
		std::lock_guard<std::mutex> lock(mLock);
		mbStopping = true;
		if (mSock != INVALID_SOCKET)
		{
			shutdown(mSock, SD_BOTH); // 받는 스레드의 recv를 깨운다
		}
	}
	if (mThread.joinable())
	{
		mThread.join();
	}
}

void MonitorLink::TakeSnapshot(Snapshot* outSnapshot)
{
	std::lock_guard<std::mutex> lock(mLock);
	outSnapshot->Status = mStatus;
	outSnapshot->bConnected = mbConnected;
	outSnapshot->Servers = mServers;
	outSnapshot->Events.assign(mEvents.begin(), mEvents.end());
	outSnapshot->History = mHistory;
}

void MonitorLink::RequestHistory(const std::string& name, const int64_t fromSec, const int64_t toSec, const bool bMinute)
{
	std::lock_guard<std::mutex> lock(mLock);
	if (!mbConnected || mHistory.bLoading)
	{
		return; // 한 번에 하나 - 답을 받기 전에 또 보내면 모니터가 끊는다
	}
	mHistory = HistoryView{};
	mHistory.Name = name;
	mHistory.bMinute = bMinute;
	mHistory.FromSec = fromSec;
	mHistory.ToSec = toSec;
	mHistory.bLoading = true;
	++mHistoryReqID;
	mHistoryNextFrom = fromSec;
	if (!sendHistoryReqLocked())
	{
		mHistory.bLoading = false;
	}
}

bool MonitorLink::sendHistoryReqLocked()
{
	MonHistoryReq req{};
	req.Type = eMonType::HistoryReq;
	req.ReqID = mHistoryReqID;
	copyText(req.Name, mHistory.Name);
	req.FromSec = mHistoryNextFrom;
	req.ToSec = mHistory.ToSec;
	req.bMinute = mHistory.bMinute ? 1 : 0;
	char buf[PACKET_HEADER_SIZE + sizeof(req)];
	const PacketHeader header{ static_cast<uint16_t>(sizeof(req)) };
	memcpy(buf, &header, PACKET_HEADER_SIZE);
	memcpy(buf + PACKET_HEADER_SIZE, &req, sizeof(req));
	std::lock_guard<std::mutex> sendLock(mSendLock);
	return mSock != INVALID_SOCKET && send(mSock, buf, static_cast<int>(sizeof(buf)), 0) == static_cast<int>(sizeof(buf));
}

void MonitorLink::run()
{
	while (true)
	{
		{
			std::lock_guard<std::mutex> lock(mLock);
			if (mbStopping)
			{
				break;
			}
			mStatus = "연결 중 " + mHost + ":" + std::to_string(mPort);
		}
		if (connectAndHello())
		{
			char payload[MAX_PAYLOAD_SIZE];
			PacketHeader header{};
			while (recvAll(reinterpret_cast<char*>(&header), PACKET_HEADER_SIZE) && header.Len <= MAX_PAYLOAD_SIZE && recvAll(payload, header.Len))
			{
				onPacket(payload, header.Len);
			}
		}
		closeSocket();
		{
			std::lock_guard<std::mutex> lock(mLock);
			mbConnected = false;
			mHistory.bLoading = false;
			if (mStatus.rfind("키", 0) != 0)
			{
				mStatus = "끊김 - 2초 뒤 다시 연결";
			}
		}
		for (int32_t i = 0; i < 20; ++i)
		{
			{
				std::lock_guard<std::mutex> lock(mLock);
				if (mbStopping)
				{
					break;
				}
			}
			Sleep(100);
		}
	}
}

bool MonitorLink::connectAndHello()
{
	SOCKADDR_IN addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(static_cast<u_short>(mPort));
	if (inet_pton(AF_INET, mHost.c_str(), &addr.sin_addr) != 1)
	{
		std::lock_guard<std::mutex> lock(mLock);
		mStatus = "주소가 IPv4가 아님: " + mHost;
		return false;
	}
	const SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (sock == INVALID_SOCKET || connect(sock, reinterpret_cast<const SOCKADDR*>(&addr), sizeof(addr)) == SOCKET_ERROR)
	{
		if (sock != INVALID_SOCKET)
		{
			closesocket(sock);
		}
		return false;
	}
	{
		std::lock_guard<std::mutex> lock(mLock);
		if (mbStopping)
		{
			closesocket(sock);
			return false;
		}
		mSock = sock;
	}
	MonViewerHello hello{};
	hello.Type = eMonType::ViewerHello;
	copyText(hello.Key, mKey);
	char buf[PACKET_HEADER_SIZE + sizeof(hello)];
	const PacketHeader header{ static_cast<uint16_t>(sizeof(hello)) };
	memcpy(buf, &header, PACKET_HEADER_SIZE);
	memcpy(buf + PACKET_HEADER_SIZE, &hello, sizeof(hello));
	{
		std::lock_guard<std::mutex> sendLock(mSendLock);
		if (send(mSock, buf, static_cast<int>(sizeof(buf)), 0) != static_cast<int>(sizeof(buf)))
		{
			return false;
		}
	}
	// 첫 답은 키 결과
	PacketHeader resHeader{};
	MonHelloRes res{};
	if (!recvAll(reinterpret_cast<char*>(&resHeader), PACKET_HEADER_SIZE) || resHeader.Len != sizeof(res) || !recvAll(reinterpret_cast<char*>(&res), sizeof(res))
		|| res.Type != eMonType::HelloRes || res.bOk == 0)
	{
		std::lock_guard<std::mutex> lock(mLock);
		mStatus = "키가 맞지 않음(db.cfg의 monitorKey) - 다시 시도";
		return false;
	}
	std::lock_guard<std::mutex> lock(mLock);
	mbConnected = true;
	mStatus = "연결됨 " + mHost + ":" + std::to_string(mPort);
	return true;
}

bool MonitorLink::recvAll(char* buf, const int32_t len)
{
	for (int32_t received = 0; received < len;)
	{
		const int ret = recv(mSock, buf + received, len - received, 0);
		if (ret <= 0)
		{
			return false;
		}
		received += ret;
	}
	return true;
}

void MonitorLink::onPacket(const char* payload, const int32_t len)
{
	if (len < 1)
	{
		return;
	}
	eMonType type;
	memcpy(&type, payload, sizeof(type));
	std::lock_guard<std::mutex> lock(mLock);
	switch (type)
	{
	case eMonType::ServerState:
		if (len == static_cast<int32_t>(sizeof(MonServerState)))
		{
			MonServerState state;
			memcpy(&state, payload, sizeof(state));
			ServerView& server = getServerLocked(toText(state.Name));
			server.Kind = state.Kind;
			server.State = state.State;
		}
		break;
	case eMonType::Rate:
		if (len == static_cast<int32_t>(sizeof(MonRate)))
		{
			MonRate rate;
			memcpy(&rate, payload, sizeof(rate));
			ServerView& server = getServerLocked(toText(rate.Name));
			if (server.Live.empty() || server.Live.back().UnixSec < rate.Row.UnixSec)
			{
				server.Live.push_back(rate.Row);
			}
			while (!server.Live.empty() && server.Live.front().UnixSec < rate.Row.UnixSec - LIVE_KEEP_SEC)
			{
				server.Live.pop_front();
			}
		}
		break;
	case eMonType::Event:
		if (len == static_cast<int32_t>(sizeof(MonEvent)))
		{
			MonEvent event;
			memcpy(&event, payload, sizeof(event));
			mEvents.push_back(EventView{ toText(event.Name), event.UnixMs, toText(event.Text) });
			while (mEvents.size() > EVENT_KEEP)
			{
				mEvents.pop_front();
			}
		}
		break;
	case eMonType::HistoryRes:
		if (len >= static_cast<int32_t>(sizeof(MonHistoryRes) - sizeof(MonRateRow) * MON_HISTORY_ROWS))
		{
			MonHistoryRes res{};
			memcpy(&res, payload, (len < static_cast<int32_t>(sizeof(res))) ? len : sizeof(res));
			if (res.ReqID != mHistoryReqID || !mHistory.bLoading)
			{
				break;
			}
			for (int32_t i = 0; i < res.RowCnt && i < MON_HISTORY_ROWS; ++i)
			{
				mHistory.Rows.push_back(res.Rows[i]);
			}
			if (res.bLast != 0)
			{
				if (res.NextFromSec != 0 && res.NextFromSec <= mHistory.ToSec)
				{
					mHistoryNextFrom = res.NextFromSec;
					if (!sendHistoryReqLocked())
					{
						mHistory.bLoading = false;
					}
				}
				else
				{
					mHistory.bLoading = false;
				}
			}
		}
		break;
	default:
		break;
	}
}

ServerView& MonitorLink::getServerLocked(const std::string& name)
{
	for (ServerView& server : mServers)
	{
		if (server.Name == name)
		{
			return server;
		}
	}
	mServers.push_back(ServerView{});
	mServers.back().Name = name;
	return mServers.back();
}

void MonitorLink::closeSocket()
{
	std::lock_guard<std::mutex> lock(mLock); // 순서는 항상 mLock → mSendLock(RequestHistory·onPacket과 같게)
	std::lock_guard<std::mutex> sendLock(mSendLock);
	if (mSock != INVALID_SOCKET)
	{
		closesocket(mSock);
		mSock = INVALID_SOCKET;
	}
}
