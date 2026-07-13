# 미니 Redis 명세

학습용 토이 프로젝트다. 사용자가 직접 코딩하며, 이 문서는 학습 가이드 수준의 명세다. 1인 개발, 예상 2~3주.

## 목표

C로 macOS에서 도는 미니 Redis 서버를 만든다. 진짜 redis-cli가 접속해 동작하고, redis-benchmark로 진짜 Redis 대비 수치를 정직하게 비교하는 것이 완료 조건이다. 학습 목표는 이벤트 루프, 소켓 멀티플렉싱, 캐시 정책(TTL/LRU), 영속화(fork+COW)의 체감이다.

## 제약

- 언어: C (C11), 외부 라이브러리 금지 - libc와 시스템 콜만 사용
- 플랫폼: macOS 전용, 멀티플렉싱은 kqueue
- 스레딩: 단일 스레드 이벤트 루프 (RDB 저장용 fork 자식 프로세스만 예외)
- 기본 포트: 6380 (진짜 Redis 6379와 충돌 회피)
- 성능: 절대 목표치 없음. 진짜 Redis와 같은 조건에서 측정한 비교 수치 기록이 기준
- 메모리 판정: RSS가 아닌 malloc 래퍼 기반 근사 할당량 추적. 바이트 정확도는 요구하지 않음

## 범위 내

- RESP2 프로토콜 파싱 (multibulk 배열 형식, incremental 파싱)
- kqueue 기반 단일 스레드 이벤트 루프, 논블로킹 소켓
- 자료구조: string, list, hash, set
- 지원 명령: PING, ECHO, GET, SET, DEL, EXISTS, INCR, TTL, EXPIRE, PERSIST, LPUSH, RPUSH, LPOP, RPOP, LRANGE, LLEN, HSET, HGET, HDEL, HGETALL, SADD, SREM, SISMEMBER, SMEMBERS, SCARD, DBSIZE, FLUSHALL, SAVE, BGSAVE, CONFIG GET/SET (maxmemory만)
- TTL: 지연 삭제 + 주기 샘플링 (진짜 Redis 방식 모사)
- maxmemory + LRU 근사 eviction (무작위 샘플링 방식, allkeys-lru만)
- RDB 스냅샷: fork + COW, 자체 바이너리 포맷, 기동 시 로드
- redis-benchmark 비교 측정 및 BENCH.md 기록

## 범위 외

- AOF 영속화 - 확장 후보. RDB만으로 영속화 개념(fork+COW) 학습 목표 달성
- pub/sub - 확장 후보. 키스페이스 학습과 직교
- 복제, 클러스터, Sentinel - 단일 노드 학습 토이 범위 초과
- RESP3, HELLO - redis-cli는 RESP2로 동작 가능
- MULTI/EXEC 트랜잭션, Lua 스크립팅 - 학습 목표와 무관
- 진짜 RDB 파일 포맷 호환 - 자체 포맷으로 충분, 포맷 리버싱은 학습 가치 낮음
- inline command (telnet식 평문) - redis-cli와 redis-benchmark 모두 multibulk만 전송
- incremental rehash - 해시테이블 리사이즈는 일괄 방식으로 단순화
- volatile-lru 등 기타 eviction 정책 - allkeys-lru 하나로 개념 학습 충분
- KEYS 글롭 패턴 매칭 - DBSIZE로 검증 대체
- TLS, ACL, 인증 - 로컬 학습용

## 요구사항

### R1. RESP 서버와 이벤트 루프

- 검증 문장: kqueue 단일 스레드 이벤트 루프 위에서 RESP2 요청을 incremental 파싱해 진짜 redis-cli 접속과 파이프라이닝이 동작한다.
- 현재 상태: 코드 없음 (README만 존재).
- 목표 상태: 논블로킹 TCP 서버가 6380 포트에서 다중 클라이언트를 kqueue로 처리하고, 부분 수신과 파이프라이닝을 견디는 RESP 파서 위에서 PING/ECHO가 동작한다.
- 수용 기준:
  - [ ] `redis-cli -p 6380 PING`이 PONG을 반환한다
  - [ ] `redis-cli -p 6380 ECHO hello`가 hello를 반환한다
  - [ ] 클라이언트 2개 이상 동시 접속 상태에서 각각 명령이 정상 동작한다
  - [ ] 한 요청이 여러 read()로 쪼개져 도착해도 (부분 수신) 정상 파싱한다
  - [ ] 한 read()에 여러 요청이 몰려 와도 (파이프라이닝) 요청 순서대로 응답한다
  - [ ] 배열(`*`)이 아닌 타입 바이트로 시작하는 입력에 -ERR 응답 후 연결을 닫는다
  - [ ] 미지원 명령에 -ERR unknown command 오류를 반환하고 연결은 유지한다
  - [ ] 클라이언트 급단절(EOF/RST)을 반복해도 fd와 버퍼가 누수 없이 정리된다

### R2. string 키스페이스

- 검증 문장: 해시테이블 키스페이스 위에서 string 타입 명령이 진짜 Redis와 같은 응답 형식으로 동작한다.
- 현재 상태: 없음.
- 목표 상태: GET, SET, DEL, EXISTS, INCR, DBSIZE, FLUSHALL이 동작하고 타입 시스템(WRONGTYPE)의 기반이 선다.
- 수용 기준:
  - [ ] SET k v 후 GET k가 v를 반환하고, 없는 키 GET은 nil을 반환한다
  - [ ] DEL이 삭제된 키 개수를 반환하고, EXISTS가 0/1을 반환한다
  - [ ] INCR이 정수 값을 1 증가시키고, 정수가 아닌 값에는 -ERR value is not an integer 오류를 반환한다
  - [ ] 1MB 값의 SET/GET 왕복이 성공한다 (대형 bulk string 부분 수신 검증)
  - [ ] DBSIZE가 키 개수를, FLUSHALL이 전체 삭제를 수행한다

### R3. 컬렉션 자료구조

- 검증 문장: list, hash, set 타입과 해당 명령이 동작하고 타입 불일치 시 WRONGTYPE 오류를 낸다.
- 현재 상태: 없음.
- 목표 상태: LPUSH/RPUSH/LPOP/RPOP/LRANGE/LLEN, HSET/HGET/HDEL/HGETALL, SADD/SREM/SISMEMBER/SMEMBERS/SCARD 동작.
- 수용 기준:
  - [ ] LPUSH/RPUSH 후 LRANGE 0 -1이 올바른 순서로 전체를 반환한다
  - [ ] LPOP/RPOP이 양끝에서 요소를 꺼내고 빈 리스트가 되면 키가 삭제된다
  - [ ] HSET/HGET/HDEL/HGETALL이 필드 단위로 동작한다
  - [ ] SADD가 중복을 무시하고, SISMEMBER/SCARD가 정확하다
  - [ ] string 키에 LPUSH 등 타입 불일치 명령 시 -WRONGTYPE 오류를 반환한다
  - [ ] SET은 기존 키의 타입과 무관하게 string으로 덮어쓴다

### R4. TTL - 지연 삭제와 주기 샘플링

- 검증 문장: EXPIRE로 설정한 만료가 지연 삭제(접근 시 체크)와 주기 샘플링(이벤트 루프 타이머) 두 경로로 처리된다.
- 현재 상태: 없음.
- 목표 상태: 진짜 Redis 방식 모사 - 접근 시 만료 체크 + 타이머 틱마다 만료 테이블에서 무작위 샘플을 뽑아 만료분 삭제, 만료 비율이 높으면 반복.
- 수용 기준:
  - [ ] EXPIRE k 1 설정 후 1초 경과 뒤 GET k가 nil을 반환한다 (지연 삭제)
  - [ ] TTL이 남은 초를, 만료 없는 키에 -1을, 없는 키에 -2를 반환한다
  - [ ] PERSIST가 만료를 제거하고 TTL이 -1이 된다
  - [ ] EXPIRE에 0 또는 음수를 주면 키가 즉시 삭제되고 1을 반환한다
  - [ ] 만료된 키에 아무 접근이 없어도 주기 샘플링이 삭제해 DBSIZE가 감소한다
  - [ ] must-NOT: 만료 시각이 지난 키가 GET에서 값을 반환하는 일이 없다

### R5. maxmemory와 LRU 근사 eviction

- 검증 문장: 근사 할당량이 maxmemory를 넘으면 무작위 샘플 중 가장 오래 접근 안 된 키를 evict하고, evict할 수 없으면 쓰기에 -OOM을 반환한다.
- 현재 상태: 없음.
- 목표 상태: 객체마다 접근 시각을 기록하고, 쓰기 명령 전에 메모리 초과 시 무작위 N개 샘플 중 LRU 키를 삭제하는 진짜 Redis 방식 모사. CONFIG SET maxmemory로 런타임 조절.
- 수용 기준:
  - [ ] CONFIG SET maxmemory <bytes> 후 초과 쓰기 시 기존 키가 evict되며 새 쓰기가 성공한다
  - [ ] 최근 GET한 키보다 오래 접근 안 된 키가 우선 evict되는 경향을 보인다 (근사이므로 통계적 경향으로 판정)
  - [ ] evict로 공간 확보가 불가능하면 쓰기 명령에 -OOM 오류를 반환하고 서버는 생존한다
  - [ ] must-NOT: maxmemory가 0 (무제한, 기본값) 일 때 eviction이 발동하지 않는다

### R6. RDB 스냅샷 - fork와 COW

- 검증 문장: SAVE/BGSAVE가 자체 바이너리 포맷으로 전체 키스페이스를 저장하고, BGSAVE는 fork+COW로 서비스 중단 없이 수행되며, 기동 시 파일을 로드한다.
- 현재 상태: 없음.
- 목표 상태: fork한 자식이 fork 시점 스냅샷을 파일로 쓰는 동안 부모는 쓰기를 계속 받는다. 만료 시각도 저장하고 로드 시 이미 만료된 키는 버린다.
- 수용 기준:
  - [ ] SAVE 후 서버 재시작 시 string/list/hash/set 데이터가 모두 복원된다
  - [ ] BGSAVE가 즉시 +Background saving started를 반환하고 저장 중에도 SET이 성공한다
  - [ ] BGSAVE 중 새로 SET한 키는 해당 스냅샷에 없어도 된다 (fork 시점 일관성)
  - [ ] 만료 시각이 저장되고, 로드 시점에 이미 만료된 키는 복원되지 않는다
  - [ ] 손상된 RDB 파일로 기동하면 에러 로그 후 기동을 거부한다 (제어된 비0 종료, segfault 아님. 파일이 아예 없으면 빈 DB로 정상 기동)
  - [ ] must-NOT: fork 실패나 디스크 오류가 서버 크래시로 이어지지 않는다 (오류 응답/로그 후 계속 서비스)

### R7. 벤치마크 - 진짜 Redis와 정직한 비교

- 검증 문장: redis-benchmark로 미니 Redis와 진짜 Redis를 같은 머신, 같은 옵션에서 측정한 수치가 BENCH.md에 기록된다.
- 현재 상태: 없음.
- 목표 상태: 정직한 비교 공식 - 같은 머신, 같은 -n/-c/-P 옵션, 진짜 Redis도 영속화 끈 상태(appendonly no, save 없음)로 측정. 배율 달성은 목표가 아니고 측정과 기록 자체가 완료 조건.
- 수용 기준:
  - [ ] `redis-benchmark -p 6380 -t ping,set,get,incr,lpush,lpop,sadd,hset -n 100000`이 오류 없이 완주한다
  - [ ] 파이프라이닝 조건(-P 16)에서도 완주한다
  - [ ] 진짜 Redis를 같은 옵션으로 측정한 수치와 나란히 BENCH.md에 기록된다 (측정 조건 명시 포함)
  - [ ] 비교 해석 한 단락이 포함된다 - 차이가 어디서 오는지 (파서, 메모리 할당, 자료구조 등) 추정

## 부정 수용 기준 (must-NOT 종합)

- [ ] 파이프라인 응답 순서가 요청 순서와 달라지는 일이 없다 (R1)
- [ ] 만료된 키가 GET에서 값을 반환하는 일이 없다 (R4)
- [ ] maxmemory 0일 때 eviction이 발동하는 일이 없다 (R5)
- [ ] BGSAVE 실패가 서버 크래시로 이어지는 일이 없다 (R6)
- [ ] 손상된 RDB를 빈 DB로 조용히 폴백해 데이터 유실을 가리는 일이 없다 (R6)

## 엣지 해소 기록

- RESP inline command: 기각 - redis-cli/redis-benchmark 모두 multibulk만 전송, 완료 조건에 불필요. 비배열 입력은 -ERR 후 연결 종료 (R1 수용 기준에 반영)
- 대형 bulk string 부분 수신: 명세 추가 (R2 - 1MB 왕복)
- EXPIRE 0/음수: 명세 추가 (R4 - 즉시 삭제 + 1 반환)
- fork 중 쓰기: 명세 추가 (R6 - fork 시점 일관성)
- 타입 불일치: 명세 추가 (R3 - WRONGTYPE)
- eviction 불가 상황: 명세 추가 (R5 - -OOM 응답)
- 만료 키의 RDB 저장/로드: 명세 추가 (R6)
- 클라이언트 급단절: 테스트로 방어 (R1 - fd 누수 검증)

## 점수표 (게이트 통과 기록)

| 차원 | 점수 | 최소 기준 |
|---|---|---|
| 목표 명확성 | 0.90 | 0.75 |
| 경계 명확성 | 0.90 | 0.70 |
| 제약 명확성 | 0.80 | 0.65 |
| 수용 기준 | 0.85 | 0.70 |

모호성 = 1 - (0.35×0.90 + 0.25×0.90 + 0.20×0.80 + 0.20×0.85) = **0.13** (게이트 0.20 이하 통과)

인터뷰는 서브에이전트 자문자답 3라운드로 진행했다 (연구자 → 경계지기/실패분석가 → 마무리자). 셀프 그릴링 1패스에서 발견한 구멍 2건 (RDB 손상 파일 처리, INCR non-integer 오류) 을 R6, R2에 반영했다.

검수 반영: RDB 손상 파일 처리를 "경고 후 빈 DB 기동"에서 "에러 로그 후 기동 거부"로 변경했다. 빈 DB 폴백은 데이터 유실을 조용히 가리는 것이라 must-NOT 철학과 모순되고, 진짜 Redis도 손상 RDB면 기동을 거부한다.
