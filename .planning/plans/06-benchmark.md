# Plan 06: redis-benchmark 비교와 정직한 기록

## 목표

redis-benchmark로 미니 Redis와 진짜 Redis를 같은 조건에서 측정해 BENCH.md에 기록한다. SPEC의 R7 구현. 배율 달성이 아니라 측정과 해석이 완료 조건이다.

## 컨텍스트

- Plan 01~05 완료 상태: 전체 명령, TTL, eviction, RDB가 동작하는 서버.
- 학습 포인트: "단일 스레드 이벤트 루프가 왜 빠른지 체감"의 수확 지점. 파이프라이닝(-P) 유무에 따른 처리량 차이가 시스템 콜/RTT 상각 효과를 보여준다.
- 정직한 비교 공식 (SPEC 제약): 같은 머신, 같은 옵션, 진짜 Redis도 영속화 끈 상태. 진짜 Redis는 `redis-server --port 6379 --appendonly no --save ''`로 기동. 측정 전 두 서버 모두 FLUSHALL. 백그라운드 부하(브라우저 등) 최소화하고 각 측정 2~3회 반복해 대표값 사용.
- redis-benchmark의 SPOP 등 미구현 명령이 섞이지 않게 -t로 종목을 명시한다.

## 작업

1. 사전 수정 - redis-benchmark가 완주하지 못하면 원인 수정이 이 plan 범위: 흔한 원인은 (1) 쓰기 버퍼 백프레셔(EVFILT_WRITE 경로) 결함, (2) benchmark가 보내는 CONFIG GET save 류 명령 - 미지원이면 -ERR로 응답해도 benchmark는 진행되는지 확인, 안 되면 해당 CONFIG 키에 빈 값 응답 추가.
2. 측정 매트릭스 실행:
   - `redis-benchmark -p 6380 -t ping,set,get,incr,lpush,lpop,sadd,hset -n 100000 -c 50`
   - 같은 커맨드에 `-P 16` 추가한 파이프라이닝 버전
   - 진짜 Redis(-p 6379)로 동일 2종 반복
3. `BENCH.md` 작성:
   - 측정 조건: 날짜, 머신(칩/메모리/OS), 두 서버 버전과 기동 옵션, benchmark 옵션, 반복 횟수
   - 표: 종목 × (mini/real) × (-P 1/-P 16) 처리량(req/s)과 p50 레이턴시
   - 해석 한 단락: 차이가 어디서 오는지 추정 - 예: 할당 전략(zmalloc 대 jemalloc), 파서 복사 횟수, 자료구조 인코딩(listpack 부재), 응답 버퍼링. 추정과 확인된 사실을 구분해 서술
4. (선택) 병목 확인 - Instruments나 `sample <pid>`로 mini-redis 프로파일 한 번 떠서 해석 단락의 근거 보강.

## must-haves

- 두 서버가 완전히 같은 benchmark 옵션으로 측정됨
- 진짜 Redis의 영속화 꺼짐 (appendonly no, save 없음)
- BENCH.md에 재현 가능한 수준의 조건 명시
- 수치를 유리하게 고르지 않음 - 반복 측정의 대표값 기준을 명시 (예: 중앙값)

## 검증

- [ ] `redis-benchmark -p 6380 -t ping,set,get,incr,lpush,lpop,sadd,hset -n 100000 -c 50`이 오류 없이 완주
- [ ] `-P 16` 파이프라이닝 조건에서도 완주
- [ ] BENCH.md에 진짜 Redis 동일 옵션 수치가 나란히 기록되고 측정 조건이 명시됨
- [ ] BENCH.md에 차이 원인 추정 해석 단락 포함
