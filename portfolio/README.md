# 포트폴리오 문서 안내

처음이라면 **구조 → 대표 사례 → 검증** 순서로 읽기를 권한다.

## 1. 구조

- [라이브러리 구조](architecture.md): 스레드, 세션, 수신·송신 경로, 락, 비용 분해

## 2. 사례

| 문서                                   | 내용                                |
| -------------------------------------- | ----------------------------------- |
| [송신 경로](cases/send_path.md)        | skip −61%의 원인과 모아 보내기 설계 |
| [브로드캐스트](cases/broadcast.md)     | 방 전달 12.7–12.9배, 기본값 재검증  |
| [데드락 경로](cases/deadlock.md)       | 강제 실패로 재현, 복구 대신 멈춤    |
| [세션 수명](cases/session_lifetime.md) | 해제 표시 비트, 리뷰에서 깨진 네 판 |
| [연결 수락](cases/accept.md)           | `accept`와 `AcceptEx` 공정 비교     |

## 3. 검증

| 문서                                            | 내용                     |
| ----------------------------------------------- | ------------------------ |
| [검증과 측정 방법](verification/README.md)      | 어떻게 시험하고 쟀는가   |
| [섞인 클라 시험](verification/mixed_clients.md) | 고부하에서 오판이 없는가 |
| [방 옮기기 검사](verification/room_move.md)     | 방 이동 중 메시지 순서   |

## 4. 설계 기록

결정 과정을 남긴 문서다. 선택지와 `의견:`/`결정:` 줄이 그대로 있다.

- [락 구조](design/lock_logic.md)
- [연결 감시](design/timeout.md)
- [송신 큐](design/send_queue.md)
- [DLL 분리](design/library_packaging.md)
- [`WSASend` 비용 실험](design/wsasend_cost.md)
- [포트폴리오 구조](design/portfolio_structure.md)

## 5. 원본

수치의 원본은 저장소의 [실험폴더](../실험폴더/)에 있다. 실행 파일은 추적하지 않으므로, 옛 실험을 다시 돌리려면 해당 커밋에서 빌드한다. 지난 에러와 해석 실수는 [README_ERRORS.md](../README_ERRORS.md)에 있다.
