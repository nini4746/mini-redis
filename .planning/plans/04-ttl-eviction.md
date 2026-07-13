# Plan 04: TTL과 LRU 근사 eviction

## 목표

만료(지연 삭제 + 주기 샘플링)와 maxmemory 기반 LRU 근사 eviction을 구현한다. SPEC의 R4, R5 구현.

## 컨텍스트

- Plan 02~03이 만든 것: 키스페이스(src/db.c), 객체 모델(src/object.c), 명령 테이블. 이벤트 루프는 src/server.c의 kevent() 루프.
- 학습 포인트 (이 토이의 핵심 새 영역): 진짜 Redis의 두 가지 확률적 알고리즘 - (1) 만료를 전수 스캔하지 않고 샘플링으로 상각하는 것, (2) 완벽한 LRU 리스트 대신 무작위 샘플 중 최소 접근 시각을 고르는 근사 LRU. 둘 다 "정확도를 조금 버리고 O(1) 상수 비용을 사는" 트레이드오프다.
- 타이머는 kevent()의 timeout 인자(예: 100ms)로 만든다 - 이벤트 없이 깨어나면 주기 작업 실행. EVFILT_TIMER를 써도 된다.

## 작업

1. `src/db.c`에 만료 테이블 추가 - 키 → 만료 시각(ms 단위 절대 시각)을 담는 별도 dict. EXPIRE 시 등록, DEL/덮어쓰기 시 제거.
2. 명령: EXPIRE(초, 키 있으면 1 없으면 0. 0 이하면 키 즉시 삭제 후 1 반환), TTL(남은 초, 만료 없으면 -1, 키 없으면 -2), PERSIST(만료 제거 시 1).
3. 지연 삭제 - src/db.c의 lookup 래퍼에 만료 체크 삽입: 만료 시각 지난 키는 그 자리에서 삭제하고 "없음"으로 처리. 모든 읽기/쓰기 명령이 이 래퍼를 지나므로 한 곳만 고치면 된다.
4. 주기 샘플링 - 100ms 틱마다: 만료 테이블에서 최대 20개 무작위 샘플 → 만료분 삭제 → 만료 비율이 25% 초과면 반복(시간 상한 두기). dict에 무작위 항목 뽑기 연산 필요 (무작위 버킷에서 시작해 선형 탐색).
5. 메모리 추적 - `src/zmalloc.c`: malloc/realloc/free 래퍼가 할당 총량 카운터 유지 (할당 크기를 헤더에 기록하거나 malloc_size() 사용). 전 코드의 할당을 이 래퍼로 교체.
6. `CONFIG GET maxmemory` / `CONFIG SET maxmemory <bytes>` - 기본 0(무제한). CONFIG GET은 키/값 배열 응답.
7. LRU 근사 - 객체에 마지막 접근 시각 필드 추가, lookup 시 갱신. 쓰기 명령 처리 전: `used > maxmemory && maxmemory > 0`이면 키스페이스에서 무작위 5개 샘플 중 접근 시각이 가장 오래된 키를 삭제, 내려갈 때까지 반복. 샘플이 안 뽑히거나 반복해도 공간 확보 불가면 `-OOM command not allowed when used memory > 'maxmemory'` 반환.

## must-haves

- 만료 체크가 lookup 래퍼 한 곳에 있어 우회 경로가 없다 (must-NOT: 만료 키가 GET에 값 반환 금지)
- maxmemory 0이면 eviction 코드가 아예 실행되지 않는다 (must-NOT)
- eviction 불가 시 -OOM 응답, 서버는 계속 동작
- 주기 샘플링이 이벤트 루프를 오래 막지 않는다 (반복 시간 상한)

## 검증

```bash
redis-cli -p 6380 SET k v
redis-cli -p 6380 EXPIRE k 1
sleep 2
redis-cli -p 6380 GET k   # → (nil)
```

- [ ] EXPIRE k 1 후 1초 경과 뒤 GET k → (nil)
- [ ] TTL: 만료 설정 키는 남은 초, 만료 없는 키 -1, 없는 키 -2
- [ ] PERSIST 후 TTL → -1, 키는 만료되지 않음
- [ ] EXPIRE k 0 / EXPIRE k -5 → 1 반환, 키 즉시 삭제
- [ ] 만료 키 100개 만들고 아무 접근 없이 수 초 대기 → DBSIZE 감소 (주기 샘플링)
- [ ] must-NOT: 만료 시각 지난 키가 GET에서 값 반환하는 일 없음 (경계 시각 반복 테스트)
- [ ] CONFIG SET maxmemory 1mb 후 큰 값 계속 SET → 기존 키 evict되며 SET 성공, DBSIZE 정체
- [ ] 키 다수 삽입 후 절반만 반복 GET → evict된 키가 주로 GET 안 한 쪽 (통계적 경향)
- [ ] 단일 키 값이 maxmemory보다 크면 -OOM 응답, 서버 생존 (이후 PING 정상)
- [ ] must-NOT: maxmemory 0 (기본) 상태에서 대량 SET에도 eviction 미발동
