# 워커 밖 스레드가 맡긴 송신의 큐 대기를 송신 정체로 세던 문제 (2026-11-10, PC A)

cpp-reviewer 지적(2026-11-07). `requestSendToWorker`가 아직 WSASend를 걸기 전에 `mWsaSendQPC`에 지금 시각을 적어, 맡긴 항목이 IOCP 큐에서 기준 시간보다 오래 기다리면 받고 있는 정상 클라도 송신 정체로 끊겼다. 수정: 맡길 때는 `INT64_MAX`를 적고, 실제 시각은 워커의 `postSend`가 WSASend 직전에 적는다.

빌드: 복사한 라이브러리 폴더에서 `cl /std:c++20 /EHsc /O2 /MT /W3 /I. /I<ProfilerLib> /I<ProtocolLib> test3.cpp NetworkLib.cpp ListenSocket.cpp SocketUtil.cpp Profiler.cpp`. 수정 전은 커밋 `77c087d`의 `NetworkLib.cpp`, `Session.h`.

- `test3.cpp`: 워커 1개, 송신 정체 기준 1초. 워커를 2.5초 붙잡아 두는 동안 워커 밖 스레드가 메시지를 맡긴다(큐에서 약 2.3초 대기). 클라는 계속 수신한다
  - 수정 전 3회: `stall=1 recv_bytes=0 client_closed=1` - 정상 클라가 송신 정체로 끊김
  - 수정 후 3회: `stall=0 recv_bytes=8 client_closed=0` - 메시지 도착, 끊김 없음
- `test4.cpp`: 진짜 송신 정체가 여전히 잡히는지. `SO_SNDBUF = 0`, 클라 수신 버퍼 1KB에 수신 안 함, 워커 밖 스레드가 4000B × 6개
  - 수정 후 3회: `stall=1 overflow=0 released=1 stop_ok` - 정체로 끊고 해제·종료 정상
- 에코 회귀 3종(수정 후): 에코 2,695MB / 비정상 헤더 5.6만 끊김 + 정상 1,876MB / 받지 않는 클라 넘침 526 + 정상 2,227MB. 정상 클라 끊김 0, 세션 0, 정상 종료
