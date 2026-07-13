# Plan 01: kqueue 이벤트 루프와 RESP 파서

## 목표

kqueue 단일 스레드 이벤트 루프 위에 incremental RESP2 파서를 얹어 진짜 redis-cli가 접속해 PING/ECHO가 동작하게 한다. SPEC의 R1 구현.

## 컨텍스트

- 그린필드. 저장소에는 README.md와 .planning/만 있다.
- 이 plan이 서버의 뼈대다. 이후 모든 plan은 여기서 만든 명령 디스패치 테이블에 명령을 추가하는 방식으로 쌓인다.
- 학습 포인트: 논블로킹 소켓 + kqueue 멀티플렉싱이 왜 단일 스레드로 다중 클라이언트를 감당하는지, 그리고 스트림 프로토콜 파싱에서 "부분 수신"과 "파이프라이닝"이 대칭 문제라는 것.
- RESP2 요청 형식: `*<개수>\r\n$<길이>\r\n<바이트>\r\n...` (redis-cli는 항상 이 multibulk 배열로 보냄). 응답 타입: `+단순문자열`, `-오류`, `:정수`, `$벌크`, `*배열`, `$-1\r\n` (nil).

## 작업

1. `Makefile` - cc -std=c11 -Wall -Wextra, 산출물 `mini-redis`. `make` 하나로 빌드.
2. `src/server.c` - main: socket/bind/listen (포트 6380, SO_REUSEADDR), listen fd와 클라이언트 fd 모두 논블로킹(O_NONBLOCK). kqueue() 생성 후 EVFILT_READ 등록, kevent() 무한 루프. listen fd 이벤트면 accept, 클라이언트 fd 이벤트면 read.
3. `src/conn.c`, `src/conn.h` - 클라이언트별 상태 구조체: fd, 읽기 버퍼(동적 성장), 쓰기 버퍼. read()가 0(EOF)이나 오류를 반환하면 fd 닫고 구조체와 버퍼 해제.
4. `src/resp.c`, `src/resp.h` - incremental 파서. 핵심 계약: 읽기 버퍼에 완전한 요청이 있으면 argv(문자열 배열)로 잘라 반환하고 소비한 바이트만큼 버퍼를 전진, 불완전하면 "더 필요함"을 반환하고 버퍼를 건드리지 않음. 첫 바이트가 `*`가 아니면 프로토콜 오류. 한 read() 안에 요청이 여러 개면 루프 돌며 전부 처리 (파이프라이닝).
5. `src/command.c` - 명령 이름(대소문자 무시) → 핸들러 함수 테이블. PING → `+PONG`, ECHO → 인자 벌크 반환, 미지명령 → `-ERR unknown command '<이름>'` (연결 유지). 프로토콜 오류만 `-ERR Protocol error` 응답 후 연결 종료.
6. 쓰기 경로 - 응답을 쓰기 버퍼에 쌓고 write(). EAGAIN으로 다 못 쓰면 EVFILT_WRITE를 등록해 나머지를 이어 쓴다 (redis-benchmark -P 16에서 반드시 겪는 경로).

## must-haves

- 파서는 절대 "버퍼에 요청 전체가 한 번에 들어온다"를 가정하지 않는다 - 바이트 1개씩 도착해도 동작
- 응답은 요청 도착 순서대로 쓰기 버퍼에 쌓인다 (must-NOT: 순서 역전 금지)
- 접속-단절 반복에 fd/메모리 누수 없음
- read/write/accept의 EAGAIN, EINTR 처리

## 검증

```bash
make && ./mini-redis &
redis-cli -p 6380 PING            # → PONG
redis-cli -p 6380 ECHO hello      # → "hello"
redis-cli -p 6380 NOSUCHCMD       # → (error) ERR unknown command
```

- [ ] `redis-cli -p 6380 PING`이 PONG 반환
- [ ] `redis-cli -p 6380 ECHO hello`가 hello 반환
- [ ] 터미널 2개에서 redis-cli 동시 접속, 각각 정상 동작
- [ ] 부분 수신: `printf '*1\r\n$4\r\nPI'; sleep 1; printf 'NG\r\n'`을 nc로 이어 보내 PONG 수신 - `(printf '*1\r\n$4\r\nPI'; sleep 1; printf 'NG\r\n') | nc localhost 6380`
- [ ] 파이프라이닝: `printf '*1\r\n$4\r\nPING\r\n*1\r\n$4\r\nPING\r\n' | nc localhost 6380` → +PONG 두 번, 순서대로
- [ ] 비배열 입력: `printf 'GET foo\r\n' | nc localhost 6380` → -ERR 후 연결 종료
- [ ] 미지원 명령에 -ERR unknown command, 연결은 유지 (같은 세션에서 이어서 PING 성공)
- [ ] fd 누수: 접속-단절 100회 반복 후 `lsof -p <pid> | wc -l`이 증가 추세 없음
