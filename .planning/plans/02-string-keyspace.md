# Plan 02: 해시테이블 키스페이스와 string 명령

## 목표

체이닝 해시테이블 키스페이스를 만들고 GET/SET/DEL/EXISTS/INCR/DBSIZE/FLUSHALL을 구현한다. SPEC의 R2 구현.

## 컨텍스트

- Plan 01이 만든 것: kqueue 이벤트 루프(src/server.c, src/conn.c), incremental RESP 파서(src/resp.c), 명령 디스패치 테이블(src/command.c). 새 명령은 테이블에 항목 추가 + 핸들러 함수 작성으로 붙인다.
- 학습 포인트: Redis의 핵심이 결국 "명령 → 해시테이블 조작"이라는 것, 그리고 값에 타입 태그를 붙여 두는 객체 모델이 이후 WRONGTYPE/TTL/LRU/RDB의 공통 기반이 된다는 것.
- 이 plan의 객체 구조체 설계가 이후 plan 전부의 토대다. 타입 태그 필드를 지금 넣어 둔다 (지금은 OBJ_STRING만 사용).

## 작업

1. `src/dict.c`, `src/dict.h` - 체이닝 해시테이블. 해시 함수는 FNV-1a나 djb2 정도로 충분. 연산: set(소유권 이전), get, delete, size, 순회(iterator - FLUSHALL/RDB/샘플링에서 씀). load factor 초과 시 2배로 일괄 rehash (incremental rehash는 범위 외).
2. `src/object.c`, `src/object.h` - 값 객체: `{ 타입 태그, 값 포인터 }`. string 값은 길이+바이트 (바이너리 세이프 - 널 문자 포함 가능, strlen 의존 금지). 키도 길이 있는 바이트 문자열.
3. `src/db.c`, `src/db.h` - 전역 키스페이스(dict 인스턴스) + lookup/insert/remove 래퍼. 이후 plan에서 만료 체크(Plan 04)가 이 래퍼에 끼어든다.
4. `src/command.c`에 핸들러 추가:
   - GET - 없으면 nil(`$-1\r\n`), 있으면 벌크. string 아닌 타입이면 WRONGTYPE (Plan 03에서 실제 발동)
   - SET - 기존 키 타입 무관 덮어쓰기, `+OK`
   - DEL - 가변 인자, 삭제 개수 정수 반환
   - EXISTS - 0/1 반환
   - INCR - 값을 long long으로 파싱해 +1 후 문자열로 재저장, 정수 반환. 파싱 실패 시 `-ERR value is not an integer or out of range`. 없는 키는 0에서 시작해 1
   - DBSIZE - 키 개수, FLUSHALL - 전체 해제 후 `+OK`

## must-haves

- 값과 키가 바이너리 세이프 (1MB 임의 바이트 값 저장 가능)
- 모든 free 경로 정리: DEL, FLUSHALL, SET 덮어쓰기 시 옛 값 해제
- 응답 형식이 진짜 Redis와 동일 (redis-cli 출력으로 비교)
- rehash 이후에도 기존 키 전부 조회 가능

## 검증

```bash
redis-cli -p 6380 SET k v && redis-cli -p 6380 GET k
```

- [ ] SET k v 후 GET k가 "v", 없는 키 GET은 (nil)
- [ ] DEL k a b가 존재한 키 개수 반환, EXISTS가 0/1 반환
- [ ] INCR n이 1, 2, 3... 반환. SET s abc 후 INCR s가 -ERR value is not an integer
- [ ] 1MB 값 왕복: `redis-cli -p 6380 SET big "$(python3 -c 'print("x"*1048576)')"` 후 `redis-cli -p 6380 STRLEN big` 대신 `GET big | wc -c`로 크기 확인 (부분 수신 경로 검증)
- [ ] 키 10000개 삽입 후 전부 GET 성공 (rehash 검증), DBSIZE가 10000, FLUSHALL 후 0
