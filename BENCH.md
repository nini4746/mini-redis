# 벤치마크: mini-redis vs 진짜 Redis

> SPEC R7 / plan 06. 완료 조건은 "배율 달성"이 아니라 **같은 조건에서 정직하게 측정하고 기록**하는 것이다.

## 이 문서의 상태

`redis-benchmark`, `redis-server`(v8.8.0)를 설치해 **진짜 Redis와 나란히 측정한 실측 수치**를 아래에 채웠다.
각 조건 3회 측정 후 **중앙값**을 대표값으로 쓴다. 재현은 `bash bench/run_bench.sh`.

## 측정 조건

- 날짜: 2026-07-13
- 머신: Apple M2 (Mac14,7), 8 코어, 16 GiB RAM
- OS: macOS 26.5.1 (build 25F80)
- 컴파일러/빌드: Apple clang, `cc -std=c11 -Wall -Wextra -O2`
- mini-redis: 이 저장소 HEAD, 포트 6380, 기본 설정(maxmemory 0, 영속화는 벤치 중 미발동)
- 진짜 Redis: `redis-server --port 6379 --appendonly no --save ''` (영속화 OFF - 공정 비교), v8.8.0
- benchmark 옵션: `-t ping,set,get,incr,lpush,lpop,sadd,hset -n 100000 -c 50`, 파이프라인 `-P 1`과 `-P 16`
- 반복: 각 조건 3회 측정 후 **중앙값** 사용 (수치를 유리하게 고르지 않음)
- 측정 전 두 서버 모두 `FLUSHALL`, 백그라운드 부하 최소화

## 실행 방법 (진짜 비교)

```bash
make
bash bench/run_bench.sh      # redis-benchmark + redis-server 필요
```

## 결과 표 (redis-benchmark 실측, 3회 중앙값)

처리량 req/s (괄호는 p50 레이턴시 ms). `ping`은 PING_MBULK 기준. PING_INLINE은 표 아래 별도.

| 종목  | mini `-P1` | real `-P1` | mini `-P16` | real `-P16` |
|-------|-----------:|-----------:|------------:|------------:|
| ping  | 187,266 (0.135) | 182,815 (0.143) | 2,500,000 (0.151) | 2,777,778 (0.159) |
| set   | 176,367 (0.151) | 180,832 (0.143) | 2,272,727 (0.319) | 1,515,152 (0.455) |
| get   | 176,056 (0.151) | 182,149 (0.143) | 2,127,660 (0.311) | 2,127,660 (0.311) |
| incr  | 177,620 (0.151) | 183,150 (0.143) | 1,818,182 (0.391) | 1,960,784 (0.343) |
| lpush | 177,936 (0.151) | 185,185 (0.143) | 2,127,660 (0.343) | 1,562,500 (0.455) |
| lpop  | 176,991 (0.151) | 181,488 (0.143) | 2,083,333 (0.343) | 1,492,537 (0.479) |
| sadd  | 179,533 (0.151) | 204,082 (0.127) | 1,960,784 (0.383) | 1,754,386 (0.399) |
| hset  | 179,211 (0.151) | 164,745 (0.135) | 1,694,915 (0.423) | 1,408,451 (0.503) |

**PING_INLINE (inline 프로토콜):** P1 mini 196,850 / real 182,482 · P16 mini 2,380,952 / real 2,083,333

## 해석

**확인된 사실 (실측):**

- **`-P 1`에서 mini-redis ≈ 진짜 Redis** (대개 ±5% 이내). PING/GET처럼 할당 없는 종목은 사실상 동률이고,
  sadd에서만 real이 약 12% 앞선다. 이 조건은 요청 1건마다 왕복(read → 파싱 → write)이라 서버 처리 시간보다
  **syscall/RTT 오버헤드가 지배적**이며, 그 부분의 큰 틀(논블로킹 + kqueue 멀티플렉싱)이 동일하기 때문이다.
- **`-P 16`(파이프라이닝)에서 mini-redis가 다수 종목에서 오히려 앞선다** (set 2.27M vs 1.52M, lpush 2.13M vs 1.56M,
  lpop 2.08M vs 1.49M, hset 1.69M vs 1.41M). 이유는 mini-redis 경로가 단순하기 때문 - jemalloc 부기·
  listpack/intset 인코딩 변환·다중 DB·keyspace notification 등 진짜 Redis가 항상 지불하는 비용이 없다.
  파이프라인으로 syscall/RTT가 상각되면 이 **명령당 상수 비용 차이**가 드러난다.
- 파이프라이닝 자체 효과: `-P 16`은 `-P 1` 대비 약 10~13배 처리량. 한 번의 `read()`로 다수 요청을 받아
  `command_dispatch`를 연속 실행하고 응답을 쓰기 버퍼에 모아 한 번에 `write()` 하므로 **syscall과 RTT가 상각**된다.
  "단일 스레드 이벤트 루프가 왜 빠른가"의 핵심 체감 지점.

**주의 - 이 우세를 과대해석하지 말 것:**

- mini-redis가 앞서는 것은 성능이 더 좋아서가 아니라 **기능이 적어서**다. 진짜 Redis의 listpack/intset은
  큰 컬렉션·메모리 효율에서 이득을 주는데, 이 벤치는 작은 값 위주라 그 비용만 보이고 이득은 안 보인다.
- 진짜 Redis는 jemalloc으로 장시간 운영 시 단편화에 강하다. 이 짧은 벤치는 그 축을 측정하지 않는다.
- 격차는 알고리즘(둘 다 O(1)/O(N) 동일)이 아니라 **상수 계수**에서 나온다. 워크로드가 바뀌면(큰 컬렉션,
  maxmemory 압박, 장기 운영) 순위는 뒤집힐 수 있다.

## 프로토콜 참고: inline 명령

redis-benchmark의 `PING_INLINE` 테스트와 telnet식 클라이언트는 multibulk가 아닌 **inline 프로토콜**
(`PING\r\n`을 그대로 전송, 공백 분리)을 쓴다. 초기엔 파서가 `*`로 시작하지 않는 입력을 프로토콜 오류로
막아 redis-benchmark 기본 suite가 PING_INLINE에서 중단됐다. `resp_parse_inline`을 추가해 해결
(공백 분리 토큰화, 64KiB 라인 상한). 이로써 redis-benchmark 기본 suite 전체가 오류 없이 완주한다.

## (선택) 프로파일

`sample <pid>` 또는 Instruments로 mini-redis 프로파일을 떠서 할당·복사 비중을 검증할 수 있다.
