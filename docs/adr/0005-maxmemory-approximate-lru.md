# 0005. maxmemory: malloc 블록 크기 추적과 샘플링 LRU 근사

- 상태: 채택
- 날짜: 2026-07-13

## 맥락
근사 할당량이 maxmemory를 넘으면 키를 evict해야 한다. 메모리 판정은 RSS가 아닌 malloc 래퍼 기반이고, 바이트 정확도는 요구하지 않는다.

## 결정
- `zmalloc`이 `malloc_size()`(macOS)/`malloc_usable_size()`(Linux)로 실제 블록 크기를 더하고 뺀다.
- 메모리를 늘리는 명령(`deny_oom`) 전에, maxmemory를 넘으면 무작위 5개 샘플 중 가장 오래 접근하지 않은 키를 반복해서 지운다 (allkeys-lru만).
- 공간을 확보하지 못하면 `-OOM`을 반환한다. maxmemory 0(기본값)이면 eviction은 꺼진다.

## 대안
- RSS 측정, 할당 헤더에 크기 기록: 헤더 없이 실제 블록 크기를 얻는 쪽을 택했다.
- 정확한 LRU 연결 리스트: 키마다 리스트 포인터와 갱신 비용이 든다.

## 결과
- 장점: 진짜 Redis 방식이고, 키마다 접근 시각(`lru`) 하나만 둔다.
- 단점/감수한 점: LRU 순서는 통계적 경향일 뿐 정확하지 않다. 다른 eviction 정책은 없다.
