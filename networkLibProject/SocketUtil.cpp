#include "SocketUtil.h"

void closeReset(SOCKET s)
{
	LINGER linger{};
	linger.l_onoff = 1;
	linger.l_linger = 0;
	setsockopt(s, SOL_SOCKET, SO_LINGER, reinterpret_cast<const char*>(&linger), sizeof(linger));
	closesocket(s);
}
