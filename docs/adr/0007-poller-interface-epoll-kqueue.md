# 0007. epoll/kqueue를 poller 인터페이스로 감쌈

- 상태: 채택
- 날짜: 2026-11-02

## 맥락
0001의 이벤트 루프는 kqueue 전용이라 Linux에서 돌지 않았다. 외부 라이브러리 금지 제약은 그대로다.

## 결정
`poller.h`/`poller.c`에 `poller_create`, `poller_add`, `poller_del`, `poller_set_write`, `poller_wait`를 두고 Linux는 epoll, macOS는 kqueue로 구현한다.
두 쪽 모두 level-triggered로 맞춘다. 단일 스레드 구조는 0001과 같다.

## 대안
- `server.c`에 `#ifdef` 분기: 이벤트 루프 코드가 플랫폼별로 갈라진다.
- libevent 같은 외부 라이브러리: 외부 라이브러리 금지 제약에 걸린다.

## 결과
- 장점: 이벤트 루프 코드는 플랫폼과 무관하게 하나다. Linux와 macOS에서 `make test`를 CI로 돌린다.
- 단점/감수한 점: 두 백엔드의 동작 차이(예: HUP/ERR 전달)를 인터페이스에서 맞춰야 한다. epoll은 HUP/ERR를 readable로 올려 `read()`에서 처리한다.
