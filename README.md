# 미니 Redis

> 토이 5번. 예상 2~3주. 언어: C (Linux epoll, macOS kqueue).

## CS 지식

이벤트 루프, 소켓 멀티플렉싱, 캐시 정책, 영속화.

## 왜

- Redis를 소비만 해봄 (분산락, 캐시, 대기열). 본 프로젝트(cdc-view-engine, ticket-fraud-engine)에서 계속 쓸 거라 내부 이해가 바로 환급됨
- 단일 스레드 이벤트 루프가 왜 빠른지 체감
- 42 webserv 했으면 이벤트 루프는 아는 상태 - 그래도 TTL 휠/LRU/스냅샷은 새 영역

## 구성

- RESP 프로토콜 파싱 (실제 redis-cli가 붙는 게 완료 조건)
- epoll(Linux)과 kqueue(macOS) 기반 단일 스레드 이벤트 루프
- 자료구조: string, list, hash, set + GET/SET/DEL/EXPIRE/LPUSH/...
- TTL: 지연 삭제 + 주기 샘플링 (진짜 Redis 방식)
- LRU 근사 eviction (maxmemory)
- RDB 스냅샷: fork + COW 활용 - xv6 cow 랩과 개념 연결
- 벤치: redis-benchmark로 진짜 Redis 대비 수치 - 정직한 비교 공식 적용

## 실행

```bash
make
./mini-redis                 # 기본 포트 6380
./mini-redis --port 7777     # 포트 지정 (-p 7777 도 가능, 1-65535)
./mini-redis --appendonly    # AOF 영속화 (appendonly.aof)
redis-cli -p 6380 PING
```

## 테스트

```bash
make test                    # 실 서버를 격리 포트에 띄우고 redis-cli로 R1~R4 AC 검증 (33 checks)
```

실제 서버 프로세스를 스폰해 공식 `redis-cli`로 구동하는 end-to-end 테스트다 (`tests/run.sh`). `redis-cli` 필요.

## AOF 영속화

`--appendonly`로 켠다. 상태를 바꾸는 명령을 RESP 그대로 기록하고 시작할 때 재실행한다.

- 프로세스가 `kill -9`로 죽어도 유실이 없고, fsync는 1초 주기라 전원 장애 시 최대 약 1초분이 유실될 수 있다.
- `EXPIRE`는 절대 시각(`PEXPIREAT`)으로 기록해 재시작해도 TTL이 다시 늘어나지 않는다.
- eviction으로 지운 키는 `DEL`로 기록해 재실행 후 되살아나지 않는다.
- 쓰다 끊긴 마지막 명령은 잘라내고 경고를 남기며, 그 외 손상은 시작을 거부한다.
- AOF가 있으면 RDB보다 AOF를 우선한다. AOF rewrite(압축)는 구현하지 않았다.

## 확장 (선택)

- pub/sub
- DNS 리졸버(토이 4번) 캐시 백엔드로 셀프 소비
