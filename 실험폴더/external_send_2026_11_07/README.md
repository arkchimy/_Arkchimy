# 워커가 아닌 스레드의 SendPacket 시험 (2026-11-07, PC A)

결과와 해석은 `portfolio/design/external_send.md`. 저장소 코드에는 넣지 않았다.

- `test.cpp`: 시험 프로그램. 서버와 클라가 한 프로세스에 있다
- `V0`: SendPacket의 워커 검사만 제거
- `V1`: V0 + 외부 스레드에서 워커별 카운터를 건너뜀
- `A`: V1 + 외부 스레드는 그 자리에서 postSend
- `B`: V1 + 외부 스레드는 PQCS로 워커에 맡김 (IoOverlapped.h, Session.h 패치 포함)

패치 기준은 커밋 b77d52c 이후 작업 트리의 NetworkLib.cpp다. 빌드는 복사한 라이브러리 폴더에서:

cl /std:c++20 /EHsc /O2 /MT /W3 /I. /I<ProfilerLib> /I<ProtocolLib> test.cpp NetworkLib.cpp ListenSocket.cpp SocketUtil.cpp Profiler.cpp

회차별 결과(WSASend는 대량 구간):
- V0: 3회 모두 0xC0000005, 출력 없음
- V1: 2회 모두 single_arrived=0 echo_arrived=0 bulk_ok=3242 bulk_fail=16758 released=0 STOP_HANG
- A: 5회 모두 recv_ext=20001 recv_echo=201 order_err=0 released=1 stop_ok, wsasend=10177 10275 10300 10249 10029
- B: 5회 모두 recv_ext=20001 recv_echo=201 order_err=0 released=1 stop_ok, wsasend=421 404 406 410 407

## 보낼 데이터가 없는 PQCS 측정

- `test2.cpp`: test.cpp + 송신 버퍼 0(환경 변수 ZEROBUF) + SendFlush 카운터 출력
- `B1`: B + 항목을 꺼낼 때 링 빔·송신 중을 센다 (IoOverlapped.h, Session.h 패치는 B와 같음)
- `B2`: B1 + 외부 스레드가 넣을 때 mbSendFlag가 켜져 있으면 PQCS를 부르지 않는다

회차별 결과 (flush / empty / busy, 12회 모두 recv_ext=20001 recv_echo=201 order_err=0 released=1 stop_ok):
- 기본 B1: 249/0/44, 258/1/54, 258/1/55
- 기본 B2: 229/0/15, 216/1/11, 211/0/10
- 송신 버퍼 0 B1: 322/0/122, 333/0/133, 284/0/84
- 송신 버퍼 0 B2: 236/0/22, 224/0/24, 214/0/13

## 송신 권리 선점

- `B3`: 외부 스레드가 mSendLock 안에서 mbSendFlag를 올리고, 성공하면 락 밖에서 PQCS. 워커는 항목을 꺼내면 플래그를 내리고 postSend (IoOverlapped.h, Session.h 패치는 B와 같음, test2.cpp 사용. busy 열은 이 변형에서 세지 않는다)

회차별 결과 (flush / empty / wsasend, 10회 모두 recv_ext=20001 recv_echo=201 order_err=0 released=1 stop_ok):
- 기본: 216/0/417, 211/0/401, 205/0/409, 205/0/397, 209/0/407
- 송신 버퍼 0: 204/0/396, 209/0/402, 207/0/396, 212/0/402, 208/0/387

## 플래그를 잡고 Send 완료로 넣기

- `B4`: 외부 스레드가 mSendLock 안에서 mbSendFlag를 잡고(링이 비어 있으면 안 잡음), mSendOv를 0바이트 Send 완료로 PQCS. test2.cpp 사용, flush는 0바이트 완료 수, busy는 세지 않는다

회차별 결과 (flush / empty / wsasend, 10회 모두 recv_ext=20001 recv_echo=201 order_err=0 released=1 stop_ok):
- 기본: 210/0/417, 214/0/422, 224/0/409, 217/0/405, 216/0/407
- 송신 버퍼 0: 214/0/399, 213/0/400, 207/0/396, 205/0/398, 210/0/397
