# 0003. inline 명령 지원

- 상태: 채택
- 날짜: 2026-07-13

## 맥락
SPEC 원안은 inline 명령(telnet식 평문)을 범위 밖으로 두고, `*`로 시작하지 않는 입력에 오류를 낸 뒤 연결을 닫았다.
그런데 `redis-benchmark` 기본 suite의 PING_INLINE이 inline 명령을 보내 벤치마크가 중단되었다.

## 결정
`*`로 시작하지 않는 입력은 `resp_parse_inline`으로 파싱한다. 공백으로 토큰을 나누고 한 줄 길이는 64KiB(`RESP_MAX_INLINE`)로 제한한다.

## 대안
- 비배열 입력을 Protocol error로 거부 (SPEC 원안): `redis-benchmark` 기본 suite를 완주하지 못한다.

## 결과
- 장점: `redis-benchmark` 기본 suite와 telnet식 클라이언트가 동작한다.
- 단점/감수한 점: SPEC의 범위 밖 항목과 R1 수용 기준 하나를 코드가 뒤집었다.
