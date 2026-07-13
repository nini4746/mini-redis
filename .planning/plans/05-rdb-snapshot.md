# Plan 05: RDB 스냅샷 - fork와 COW

## 목표

SAVE/BGSAVE로 전체 키스페이스를 자체 바이너리 포맷으로 저장하고 기동 시 로드한다. BGSAVE는 fork+COW로 서비스 중단 없이 수행한다. SPEC의 R6 구현.

## 컨텍스트

- Plan 02~04가 만든 것: 키스페이스와 만료 테이블(src/db.c), 4개 타입 객체(src/object.c), dict 순회 iterator(src/dict.c).
- 학습 포인트: fork()의 COW 덕에 자식은 fork 시점 메모리 스냅샷을 공짜로 얻고, 부모가 이후 쓰는 페이지만 복사된다는 것 - xv6 COW 랩에서 커널 쪽으로 본 메커니즘을 이번엔 사용자 쪽에서 소비한다. 직렬화는 fork 시점 일관성이 보장되므로 잠금이 필요 없다.
- 자식 프로세스 회수(SIGCHLD/waitpid)를 안 하면 좀비가 남는다 - 이벤트 루프 틱에서 논블로킹 waitpid로 회수.

## 작업

1. `src/rdb.c`, `src/rdb.h` - 자체 바이너리 포맷 (진짜 RDB 호환 아님):
   - 헤더: 매직 "MRDB" + 버전 1바이트
   - 레코드 반복: [만료 시각 유무 플래그 + ms 절대 시각(있으면)] [타입 태그] [키 길이+바이트] [타입별 페이로드 - string: 길이+바이트, list/set: 개수 + 요소 반복, hash: 개수 + 필드/값 반복]
   - 끝 마커 + (선택) 체크섬
2. 저장 - dict iterator로 전체 순회하며 직렬화. 만료 시각이 이미 지난 키는 건너뛴다. 임시 파일에 쓴 뒤 rename()으로 원자 교체 (쓰다 죽어도 기존 파일 보존).
3. 로드 - 기동 시 dump.mrdb 존재하면 파싱해 키스페이스 재구성. 만료 시각이 현재보다 과거인 키는 버림. 매직 불일치, 잘린 파일, 길이 필드 이상이면 stderr에 에러 로그 출력 후 비0 종료로 기동 거부 (진짜 Redis와 동일 - 데이터 유실을 빈 DB로 조용히 가리지 않음). segfault가 아닌 제어된 종료여야 하므로 모든 read에 EOF/범위 검사. 파일이 아예 없으면 빈 DB로 정상 기동.
4. SAVE 명령 - 부모가 직접 저장 (블로킹, 완료 후 +OK).
5. BGSAVE 명령 - fork(). 자식: 저장 후 _exit(0/1). 부모: 즉시 `+Background saving started` 응답하고 서비스 계속. fork 실패 시 -ERR 응답, 서버 생존. 이미 BGSAVE 진행 중이면 -ERR 응답. 이벤트 루프 틱에서 waitpid(WNOHANG)로 자식 회수하고 결과 로그.
6. 자식 프로세스에서는 상속받은 리스닝/클라이언트 fd를 닫고 시작 (자식이 소켓에 응답하는 사고 방지).

## must-haves

- rename() 원자 교체 - 저장 중 실패해도 기존 스냅샷 파일이 깨지지 않음
- 손상 파일이면 에러 로그 후 기동 거부 (제어된 비0 종료, segfault 금지). must-NOT: 빈 DB로 조용히 폴백하지 않음. 파일 부재는 정상 기동
- fork 실패, 디스크 오류가 서버 크래시로 이어지지 않음 (must-NOT)
- 자식 좀비 회수 (waitpid)
- 진행 중 BGSAVE 중복 실행 방지

## 검증

```bash
redis-cli -p 6380 SET k v && redis-cli -p 6380 RPUSH l a b && redis-cli -p 6380 SAVE
kill <pid> && ./mini-redis &
redis-cli -p 6380 GET k          # → "v"
redis-cli -p 6380 LRANGE l 0 -1  # → a b
```

- [ ] 4개 타입(string/list/hash/set) 각각 저장 후 재시작 시 값과 순서 복원
- [ ] BGSAVE가 즉시 +Background saving started 반환, 직후 SET 성공 (블로킹 없음 - 대형 데이터셋으로 확인)
- [ ] 대형 데이터셋 BGSAVE 중 SET한 새 키가 그 스냅샷 파일에 없음 (fork 시점 일관성 - 재시작으로 확인)
- [ ] EXPIRE 걸린 키 저장 → 만료 전 재시작이면 TTL 유지 복원, 만료 후 재시작이면 복원 안 됨
- [ ] dump.mrdb를 truncate 하거나 임의 바이트로 오염시킨 뒤 기동 → 에러 로그 출력 후 비0 종료 (`echo $?`로 확인, segfault 아님). 파일 삭제 후 재기동하면 빈 DB로 정상 시작
- [ ] must-NOT: 손상 파일 기동이 빈 DB로 조용히 폴백하지 않음 (위 시나리오에서 서버가 떠서 DBSIZE 0을 반환하면 실패)
- [ ] must-NOT: BGSAVE 실패 상황(읽기 전용 디렉토리에서 저장 시도 등)에서 -ERR 또는 로그만 남고 서버 생존, 이후 PING 정상
