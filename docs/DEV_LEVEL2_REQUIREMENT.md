# DEV_LEVEL2_REQUIREMENT - Level 2 개발 요구사항

## 개요
Level 2는 Telnet 프로토콜 계층으로, 모뎀 브리지와 텔넷 서버 간의 통신을 담당하며 RFC 854/855 및 관련 확장 표준을 구현합니다.

## 핵심 요구사항

### 1. RFC 표준 준수
Level 2는 다음 RFC 표준들을 준수해야 합니다:
- **RFC 854**: Telnet Protocol Specification (기본 프로토콜)
- **RFC 855**: Telnet Option Specifications (옵션 협상)
- **RFC 858**: Suppress Go Ahead (SGA 옵션)
- **RFC 1091**: Terminal Type Option (터미널 타입)
- **RFC 1184**: Linemode Option (라인모드)

### 2. IAC(Interpret As Command) 처리

#### 2.1 기본 명령 처리
- **IAC (0xFF)**: 명령 시작 표시자
- **데이터 이스케이핑**: 데이터 내 0xFF는 0xFF 0xFF로 전송
- **명령 파싱**: SE(240), SB(250), WILL/WONT/DO/DONT(251-254) 등 처리

#### 2.2 필수 구현 명령
```
IAC SE   (240) - End of subnegotiation
IAC NOP  (241) - No operation
IAC DM   (242) - Data Mark
IAC BRK  (243) - Break
IAC IP   (244) - Interrupt Process
IAC AO   (245) - Abort output
IAC AYT  (246) - Are You There
IAC EC   (247) - Erase character
IAC EL   (248) - Erase Line
IAC GA   (249) - Go ahead
IAC SB   (250) - Subnegotiation begin
IAC WILL (251) - Will do option
IAC WONT (252) - Won't do option
IAC DO   (253) - Do option
IAC DONT (254) - Don't do option
IAC IAC  (255) - Data byte 0xFF
```

### 3. 옵션 협상 메커니즘

#### 3.1 협상 원칙 (RFC 855)
- 모든 옵션은 독립적으로 양방향 협상
- 기본값은 WONT/DONT (옵션 비활성)
- 미지원 옵션에는 즉시 부정 응답

#### 3.2 협상 규칙
```
수신: DO x   → 응답: WILL x (지원 시) 또는 WONT x (미지원)
수신: DONT x → 응답: WONT x
수신: WILL x → 응답: DO x (원하면) 또는 DONT x (원치 않으면)
수신: WONT x → 응답: DONT x
```

### 4. 주요 옵션 구현

#### 4.1 SGA (Suppress Go Ahead) - Option 3
- GA 신호 생략을 위한 옵션
- 문자 단위(character-at-a-time) 모드에서 필수
- 양방향 독립 협상

#### 4.2 ECHO - Option 1
- 에코 제어 옵션
- 한쪽만 에코 수행 (중복 방지)
- SGA와 함께 사용 시 문자 단위 환경 구성

#### 4.3 TERMINAL-TYPE - Option 24
- 터미널 타입 교환
- 서버 요청(SEND) 시에만 응답(IS)
- 서브협상 사용: IAC SB TERMINAL-TYPE ...

#### 4.4 LINEMODE - Option 34
- 라인 편집 모드 제어
- MODE, FORWARDMASK, SLC 서브옵션
- 로컬 편집 기능 활성화/비활성화

### 5. 동작 모드별 요구사항

#### 5.1 라인 모드 (Line-at-a-time)
- 로컬에서 라인 편집 후 전송
- Enter/CRLF 시 라인 전송
- LINEMODE 옵션으로 제어

#### 5.2 문자 모드 (Character-at-a-time)
- 키 입력 즉시 전송
- SGA + ECHO 옵션 조합
- GA 신호 억제

#### 5.3 바이너리 모드
- 8비트 투명 전송
- BINARY 옵션 (Option 0)
- 멀티바이트 문자 지원

### 6. 상태 기계 요구사항

#### 6.1 IAC 파서 상태
```
STATE_DATA    → 일반 데이터 처리
STATE_IAC     → IAC 수신, 다음 바이트 대기
STATE_WILL    → WILL 처리 중
STATE_WONT    → WONT 처리 중
STATE_DO      → DO 처리 중
STATE_DONT    → DONT 처리 중
STATE_SB      → 서브협상 수집 중
STATE_SB_IAC  → 서브협상 내 IAC 처리
```

#### 6.2 버퍼링 요구사항
- 서브협상 데이터 누적 버퍼
- IAC SE까지 수집 후 처리
- 부분 명령 처리 방지

### 7. 테스트 시나리오

#### 7.1 서버 연결 테스트
- 포트 9091: 라인 모드 서버
- 포트 9092: 문자 모드 서버
- 포트 9093: 라인 모드 바이너리 서버

#### 7.2 데이터 전송 테스트
- ASCII 문자열: "abcd"
- 한글: "한글"
- 일본어: "こんにちは。"
- 각 3초 간격 전송
- 에코 수신 확인

#### 7.3 타임스탬프 수신 테스트
- 30초간 대기
- 서버로부터 주기적 타임스탬프 수신
- 로그 기록 확인

### 8. 에러 처리 요구사항

#### 8.1 프로토콜 에러
- 잘못된 IAC 시퀀스 복구
- 불완전한 서브협상 타임아웃
- 순환 협상 방지

#### 8.2 연결 에러
- 연결 끊김 감지
- 자동 재연결 (옵션)
- 상태 초기화

### 9. 성능 요구사항

- 최대 지연시간: 100ms 이내
- 버퍼 오버플로우 방지
- 멀티바이트 문자 경계 보존
- CPU 사용률: 5% 이하

### 10. 로깅 요구사항

- 모든 IAC 명령 로깅 (디버그 모드)
- 옵션 협상 과정 기록
- 데이터 송수신 추적
- 에러 상황 상세 로깅

## 구현 우선순위

1. **필수**: IAC 기본 명령 처리
2. **필수**: DO/DONT/WILL/WONT 협상
3. **필수**: SGA, ECHO 옵션
4. **권장**: TERMINAL-TYPE 옵션
5. **선택**: LINEMODE 옵션

## 검증 기준

- [ ] 3가지 서버 모드 모두 연결 성공
- [ ] IAC 명령 올바른 파싱 및 응답
- [ ] 멀티바이트 문자 전송 무결성
- [ ] 30분 이상 연속 운영 안정성
- [ ] 모드 전환 시 상태 일관성