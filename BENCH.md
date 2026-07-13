# 벤치마크: mini-redis vs 진짜 Redis

> SPEC R7 / plan 06. 완료 조건은 "배율 달성"이 아니라 **같은 조건에서 정직하게 측정하고 기록**하는 것이다.

## 이 문서의 상태

이 머신에는 `redis-benchmark`와 `redis-server`가 **설치되어 있지 않다** (`which redis-benchmark redis-server` → not found).
따라서 SPEC R7이 요구하는 **진짜 Redis와의 나란한 비교 수치는 아직 비어 있다**. 아래 표의 real-redis 열과
공식 mini-redis 열(redis-benchmark 측정)은 `redis-benchmark`가 있는 머신에서 `bench/run_bench.sh`를 돌린 뒤 채워야 한다.

대신, 서버가 요구 워크로드를 **오류 없이 완주**한다는 것과 파이프라이닝 경로가 동작한다는 것을 증명하기 위해
외부 도구가 필요 없는 자체 부하 생성기(`bench/loadgen.py`)로 측정한 **참고용(indicative) mini-redis 단독 수치**를 함께 싣는다.
이 수치는 클라이언트가 파이썬(단일 프로세스, GIL)이라 **클라이언트 병목**이 지배적이므로 서버의 상한이 아니라 하한에 가깝다.

## 측정 조건

- 날짜: 2026-07-13
- 머신: Apple M2 (Mac14,7), 8 코어, 16 GiB RAM
- OS: macOS 26.5.1 (build 25F80)
- 컴파일러/빌드: Apple clang 21.0.0, `cc -std=c11 -Wall -Wextra -O2`
- mini-redis: 이 저장소 HEAD, 포트 6380, 기본 설정(maxmemory 0, 영속화는 벤치 중 미발동)
- 진짜 Redis(예정): `redis-server --port 6379 --appendonly no --save ''` (영속화 OFF - 공정 비교)
- benchmark 옵션: `-t ping,set,get,incr,lpush,lpop,sadd,hset -n 100000 -c 50`, 파이프라인 `-P 1`과 `-P 16` 두 조건
- 반복: 각 조건 2~3회 측정 후 **중앙값**을 대표값으로 사용 (수치를 유리하게 고르지 않음)
- 측정 전 두 서버 모두 `FLUSHALL`, 백그라운드 부하 최소화

## 실행 방법 (진짜 비교)

```bash
make
bash bench/run_bench.sh      # redis-benchmark + redis-server 필요
```

`run_bench.sh`는 두 서버를 같은 옵션으로 띄우고 `-P 1`, `-P 16`을 각각 mini/real에 대해 `--csv`로 출력한다.
2~3회 돌려 중앙값을 아래 표에 옮긴다.

## 결과 표 (redis-benchmark 측정 - 채워야 함)

처리량 req/s (p50 레이턴시 ms). `redis-benchmark` 미설치로 현재 공란.

| 종목  | mini `-P1` | real `-P1` | mini `-P16` | real `-P16` |
|-------|-----------:|-----------:|------------:|------------:|
| ping  | _requires redis-benchmark_ | _" _ | _" _ | _" _ |
| set   | _requires redis-benchmark_ | _" _ | _" _ | _" _ |
| get   | _requires redis-benchmark_ | _" _ | _" _ | _" _ |
| incr  | _requires redis-benchmark_ | _" _ | _" _ | _" _ |
| lpush | _requires redis-benchmark_ | _" _ | _" _ | _" _ |
| lpop  | _requires redis-benchmark_ | _" _ | _" _ | _" _ |
| sadd  | _requires redis-benchmark_ | _" _ | _" _ | _" _ |
| hset  | _requires redis-benchmark_ | _" _ | _" _ | _" _ |

## 참고용 mini-redis 단독 수치 (fallback, redis-benchmark 아님)

`bench/loadgen.py` (파이썬, `-n 100000 -c 50`), 대표 1회 실행. **클라이언트 병목이 지배적**이므로
서버 성능의 하한 참고값이다. redis-benchmark(C, 무병목)로 재면 이보다 훨씬 높게 나온다.

| 종목  | `-P1` req/s | `-P16` req/s | 파이프라인 배율 |
|-------|------------:|-------------:|----------------:|
| ping  | 62,602      | 685,039      | 10.9x |
| set   | 71,607      | 559,982      | 7.8x  |
| get   | 68,415      | 562,389      | 8.2x  |
| incr  | 67,734      | 577,120      | 8.5x  |
| lpush | 68,127      | 456,947      | 6.7x  |
| lpop  | 67,375      | 341,069      | 5.1x  |
| sadd  | 65,461      | 522,029      | 8.0x  |
| hset  | 67,426      | 473,547      | 7.0x  |

모든 종목이 `-P 1`과 `-P 16`에서 오류 없이 완주했다 (R7 수용 기준의 "완주" 항목 충족, 단 공식 도구는 loadgen으로 대체).

## 해석

**확인된 사실:**

- 파이프라이닝(`-P 16`)은 `-P 1` 대비 5~11배 처리량을 낸다. 원인은 명확하다 - 파이프라인 없이는 요청 1건마다
  왕복(read syscall → 파싱 → write syscall)이 붙지만, 파이프라인은 한 번의 `read()`로 다수 요청을 받아
  `command_dispatch`를 연속 실행하고 응답을 쓰기 버퍼에 모아 한 번에 `write()` 하므로 **시스템 콜과 RTT가 상각**된다.
  이것이 "단일 스레드 이벤트 루프가 왜 빠른가"의 핵심 체감 지점이다.
- `lpop`의 배율이 가장 낮은 이유도 구조적이다 - loadgen의 lpop 반복은 `RPUSH`+`LPOP` 두 명령을 보내므로
  요청·응답 수가 2배라 파이프라인 효율이 상대적으로 낮게 잡힌다.

**추정 (redis-benchmark로 확인 필요):** 진짜 Redis와의 절대 격차는 다음에서 올 것으로 본다 -

- **메모리 할당자**: 진짜 Redis는 jemalloc, mini-redis는 libc malloc을 감싼 `zmalloc`(추적 오버헤드로 `malloc_size` 호출 추가). 할당 집약적인 SET/LPUSH에서 불리할 것.
- **자료구조 인코딩**: 진짜 Redis는 작은 컬렉션을 listpack/intset로 압축해 캐시 지역성과 할당 횟수에서 이득을 본다. mini-redis는 항상 연결 리스트/해시테이블이라 작은 list/hash/set에서 할당·포인터 추적 비용이 크다.
- **파서 복사**: mini-redis 파서는 각 인자를 읽기 버퍼에서 **별도 버퍼로 복사**(`resp_parse`의 `zmalloc`+`memcpy`)한다. 진짜 Redis는 쿼리 버퍼를 가리키는 방식으로 복사를 줄인다. 인자가 큰 SET에서 차이가 날 것.
- **응답 버퍼링/이벤트 루프**: 큰 틀(논블로킹 + 멀티플렉싱 + 응답 배칭)은 같으므로 PING/GET 같은 할당 없는 종목에서는 격차가 가장 작을 것으로 추정.

요약하면 격차는 알고리즘(둘 다 O(1)/O(N) 동일)이 아니라 **상수 계수** - 할당자, 인코딩 최적화, 복사 횟수 - 에서 나올 것으로 본다.
정확한 배율은 redis-benchmark 설치 후 위 표를 채워 확정한다.

## (선택) 프로파일

`sample <pid>` 또는 Instruments로 mini-redis 프로파일을 떠서 위 추정(할당·복사 비중)을 검증할 수 있다. 도구 설치 후 진행.
