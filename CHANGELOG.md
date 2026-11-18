# 변경 이력

이 프로젝트의 주요 변경 사항을 기록합니다. 형식은 [Keep a Changelog](https://keepachangelog.com/ko/1.1.0/)를 따릅니다.

## 2026-11-18
### 수정
- maxmemory보다 큰 값을 쓰면 키스페이스를 비우지 않고 바로 `-OOM`을 반환합니다 (#1).
- 핸들러가 오류로 거부한 쓰기 명령은 AOF에 기록하지 않습니다. 재시작 시 기동 거부가 없어졌습니다 (#2).
- RDB만 있던 상태에서 `--appendonly`를 켜면 처음 만드는 AOF에 RDB에서 읽은 키스페이스를 써 둡니다. TTL은 `PEXPIREAT`로 유지합니다. 두 번째 기동부터 RDB 키가 사라지던 문제를 고쳤습니다 (#3).
- maxmemory 안쪽의 큰 값을 쓸 때 요청 자체의 버퍼 때문에 키스페이스를 전부 지우던 문제를 고쳤습니다. 요청 버퍼는 한도 계산에서 빼고, 키를 다 지워도 들어가지 않는 쓰기는 지우지 않고 거부합니다 (#4).
### 문서
- README를 구조도와 흐름도 중심으로 다시 쓰고 AOF, epoll 내용과 테스트 수(45)를 반영했습니다.
- 요구사항 명세(SRS)와 설계 명세(SDS)를 추가하고 AOF와 Linux 지원 내용을 맞췄습니다.

## 2026-11-02
### 추가
- Linux 지원: 이벤트 루프가 Linux에서는 epoll, macOS에서는 kqueue를 씁니다.
- `--appendonly` 옵션으로 AOF 영속화를 켭니다. 쓰기 명령을 기록하고 시작할 때 재실행합니다. kill -9 후 복구, 끊긴 꼬리 복구, 손상 시 기동 거부를 지원합니다.
### 변경
- CI 추가

## 2026-07-15
### 추가
- `make test`: 진짜 redis-cli로 서버를 검사하는 end-to-end 테스트(33개)를 추가했습니다.

## 2026-07-14
### 추가
- `--port`/`-p` 옵션으로 listen 포트를 바꿀 수 있습니다 (기본 6380, 1-65535).

## 2026-07-13
### 추가
- kqueue 단일 스레드 이벤트 루프와 incremental RESP2 파서. PING, ECHO를 지원합니다.
- string 명령: GET, SET, DEL, EXISTS, INCR, DBSIZE, FLUSHALL.
- list, hash, set 명령과 타입 불일치 시 WRONGTYPE 오류.
- TTL(EXPIRE, TTL, PERSIST): 지연 삭제와 주기 샘플링으로 만료를 처리합니다.
- maxmemory와 LRU 근사 eviction, `CONFIG GET/SET maxmemory`.
- RDB 스냅샷: SAVE, fork 기반 BGSAVE, 기동 시 로드. 손상된 파일이면 기동을 거부합니다.
- 벤치마크 스크립트(`bench/run_bench.sh`, `bench/loadgen.py`)와 진짜 Redis 비교 결과(`BENCH.md`).
### 수정
- inline 명령(telnet식 평문)을 파싱합니다. 이전에는 오류로 연결을 닫아 `redis-benchmark` 기본 suite가 PING_INLINE에서 중단되었습니다.
