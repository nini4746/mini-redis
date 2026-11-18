# 설계 결정 기록 (ADR)

| 번호 | 결정 | 상태 | 날짜 |
|------|------|------|------|
| [0001](0001-single-thread-kqueue-event-loop.md) | 단일 스레드 kqueue 이벤트 루프 | 대체됨 (0007) | 2026-07-13 |
| [0002](0002-incremental-resp-parser-and-batched-write.md) | 재파싱 방식 incremental RESP2 파서와 응답 일괄 write | 채택 | 2026-07-13 |
| [0003](0003-support-inline-commands.md) | inline 명령 지원 | 채택 | 2026-07-13 |
| [0004](0004-ttl-lazy-delete-and-active-sampling.md) | TTL: 별도 만료 테이블, 지연 삭제, 주기 샘플링 | 채택 | 2026-07-13 |
| [0005](0005-maxmemory-approximate-lru.md) | maxmemory: malloc 블록 크기 추적과 샘플링 LRU 근사 | 채택 | 2026-07-13 |
| [0006](0006-rdb-fork-cow-custom-format.md) | RDB 스냅샷: fork + COW, 자체 포맷, 손상 시 기동 거부 | 채택 | 2026-07-13 |
| [0007](0007-poller-interface-epoll-kqueue.md) | epoll/kqueue를 poller 인터페이스로 감쌈 | 채택 | 2026-11-02 |
| [0008](0008-aof-persistence.md) | AOF 영속화: RESP 기록, 1초 fsync, 손상 처리 | 채택 | 2026-11-02 |
