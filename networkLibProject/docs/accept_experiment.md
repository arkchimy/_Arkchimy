# accept() vs AcceptEx 처리량 실험

## 목적

연결이 쏟아질 때 블로킹 `accept()`와 `AcceptEx`(IOCP 비동기) 중 어느 쪽이 accept 처리량(TPS)이
더 높은지 비교한다.

## 구조

- `AcceptServer` (`AcceptServer.h/cpp`): accept 전담 스레드 1개가 블로킹 `accept()`를 반복.
  받으면 즉시 `closesocket`.
- `AcceptExServer` (`AcceptExServer.h/cpp`): 리슨 소켓을 IOCP에 등록하고, 워커 스레드들이
  `GetQueuedCompletionStatus`로 `AcceptEx` 완료를 받음. 완료되면 즉시 `closesocket` 후 같은
  `AcceptOv` 슬롯으로 바로 재등록.
- 둘 다 Session/Recv/Send 없음 — 순수 accept 처리량만 측정하는 게 목적.
- 1초마다 별도 스레드(`statLoop`)가 그 1초간 처리한 accept 수(TPS)를 콘솔에 출력.

## TODO (실험 끝난 뒤 처리할 것)

- **`AcceptExServer::stop()`에서 pending 상태인 accept 소켓을 안 닫는다.** 종료 시점에 아직
  완료되지 않은 `AcceptEx`가 들고 있는 `mAcceptOvs[i].mAcceptSocket`들은 정리하지 않고 그냥
  프로세스를 끝냄 (지금은 실험용이라 허용). AcceptEx를 실제로 계속 쓸 것 같으므로, 이 실험이
  끝나고 실제 세션 구조로 넘어갈 때 **반드시 정리 로직을 작성할 것**
  (예: `CancelIoEx`로 pending I/O 취소 후 완료 대기, 또는 각 슬롯의 소켓을 순회하며 닫기).
