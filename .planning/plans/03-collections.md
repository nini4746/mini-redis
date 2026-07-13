# Plan 03: list, hash, set 컬렉션

## 목표

list/hash/set 타입과 명령을 추가하고 타입 시스템(WRONGTYPE)을 완성한다. SPEC의 R3 구현.

## 컨텍스트

- Plan 02가 만든 것: dict(src/dict.c), 타입 태그 있는 객체 모델(src/object.c), 키스페이스 래퍼(src/db.c), string 명령들.
- 학습 포인트: Redis 값이 "타입 태그 + 내부 자료구조 포인터"라는 다형 객체이고, 명령 진입점마다 타입 검사가 선행된다는 것. (진짜 Redis의 ziplist/listpack 같은 인코딩 최적화는 범위 외 - 기본 자료구조로 간다.)
- 명령 추가 방식은 동일: src/command.c 테이블에 등록.

## 작업

1. `src/object.h`에 타입 추가: OBJ_LIST, OBJ_HASH, OBJ_SET.
2. `src/list.c` - 양방향 연결 리스트 (head/tail/len). LPUSH/RPUSH는 키 없으면 생성, LPOP/RPOP 후 길이 0이면 키스페이스에서 키 삭제 (진짜 Redis 동작 - 빈 컬렉션 키는 존재하지 않음). LRANGE는 음수 인덱스(-1 = 마지막) 지원, LLEN.
3. hash - dict를 내부 재사용 (필드 → 값). HSET(새 필드 개수 반환), HGET(없으면 nil), HDEL(삭제 개수, 비면 키 삭제), HGETALL(필드/값 교대 배열 응답).
4. set - dict를 값 NULL로 재사용. SADD(실제 추가된 개수 - 중복 무시), SREM(비면 키 삭제), SISMEMBER(0/1), SMEMBERS(배열), SCARD(정수).
5. 타입 검사 공통 헬퍼 - 키 조회 후 기대 타입과 다르면 `-WRONGTYPE Operation against a key holding the wrong kind of value` 반환. 모든 컬렉션 명령과 GET/INCR에 적용. SET만 예외 (타입 무관 덮어쓰기 - 이때 옛 객체를 타입별 소멸자로 해제).
6. 객체 해제를 타입별 소멸자로 정리 - DEL/FLUSHALL/덮어쓰기가 컬렉션 내부까지 재귀적으로 해제.

## must-haves

- 빈 컬렉션은 키스페이스에 남지 않는다 (마지막 요소 pop/del 시 키 삭제)
- 모든 명령의 인자 개수 검사 - 부족하면 -ERR wrong number of arguments
- 컬렉션 해제 경로에 누수 없음 (요소 문자열까지 해제)
- 응답 형식이 진짜 Redis와 동일

## 검증

- [ ] `RPUSH l a b c` 후 `LRANGE l 0 -1` → a b c 순서, `LPUSH l z` 후 첫 요소 z
- [ ] LPOP/RPOP이 양끝에서 꺼내고, 마지막 요소 pop 후 `EXISTS l` → 0
- [ ] `HSET h f1 v1 f2 v2` 후 HGET h f1 → v1, HDEL h f1 → 1, HGETALL이 남은 필드/값 반환
- [ ] `SADD s a a b` → 2 (중복 무시), SISMEMBER s a → 1, SCARD s → 2, SMEMBERS가 a b 반환
- [ ] `SET k v` 후 `LPUSH k x` → -WRONGTYPE. `HSET h f v` 후 `GET h` → -WRONGTYPE
- [ ] `RPUSH l a` 후 `SET l str` → +OK, `GET l` → "str" (SET은 타입 무관 덮어쓰기)
