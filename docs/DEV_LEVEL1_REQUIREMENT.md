# DEV_LEVEL1_REQUIREMENT - Level 1 개발 요구사항

## 개요
Level 1은 Serial/Modem 계층으로, 클라이언트와 시리얼 포트를 통해 통신하며 Hayes AT 명령어를 처리하는 모뎀 에뮬레이션 계층입니다.

## 핵심 요구사항

### 1. modem_sample 패턴 적용
Level 1 구현 시 검증된 modem_sample 프로그램의 다음 패턴들을 적용해야 합니다:

#### 1.1 Serial Port 처리
- **OPOST/ONLCR 비활성화**: Raw 모드로 모든 데이터를 그대로 처리
- **CLOCAL 설정**: 모뎀 제어 신호 무시
- **Blocking Mode**: VMIN=1, VTIME=0 설정으로 안정적인 데이터 수신
- **Select() with Timeout**: 블로킹 방지를 위한 100ms 타임아웃

#### 1.2 Port Locking
- **UUCP Lock 파일**: `/var/lock/LCK..` 형식 사용
- **Lock 순서**: 호출자가 lock → open → close → unlock 순서 보장
- **PID 기록**: Lock 파일에 현재 프로세스 PID 저장

#### 1.3 Line Buffering
- **serial_read_line()**: 내부 버퍼로 완전한 라인 단위 읽기
- **Hardware Message Buffer**: 하드웨어 메시지 전용 버퍼 (4096 바이트)
- **Escape Sequence Buffer**: AT 명령 이스케이프 시퀀스 처리용

### 2. CONNECT 메시지 단편화 문제
#### 문제 상황
```
Iteration 5: Read 7 bytes: CONNECT
Iteration 6: Read 7 bytes: 57600\r
```
CONNECT 응답이 여러 read()로 분리되어 전체 메시지를 놓치는 문제

#### 근본 원인
- Draining 단계에서 RING만 필터링하고 CONNECT/NO CARRIER는 무시
- 하드웨어 메시지 버퍼가 있지만 활용하지 않음

#### 해결 요구사항
- Draining 중에도 하드웨어 메시지 감지 및 버퍼링
- 완전한 라인 단위로 메시지 처리
- RING, CONNECT, NO CARRIER 모두 처리

### 3. RING 감지 모드
#### SOFTWARE 모드 (S0=0)
- RING 신호 2회 감지 → ATA 명령 자동 전송
- 사용자가 직접 ATA 입력 가능

#### HARDWARE 모드 (S0>0)
- S0 레지스터 값만큼 RING 후 자동 응답
- 별도 처리 없이 모뎀이 자동 처리

### 4. 모뎀 초기화 시퀀스
#### 초기화 흐름
1. DTR 설정으로 모뎀 리셋
2. 초기 버퍼 드레이닝 (잔여 데이터 제거)
3. ATZ 명령으로 소프트 리셋
4. MODEM_INIT_COMMAND 실행
5. Auto-answer 모드별 설정 명령 실행

#### 드레이닝 단계 요구사항
- RING 신호 감지 및 처리
- Select() 타임아웃으로 블로킹 방지
- 설정 상태 표시 (디버깅용)

### 5. 에러 처리 및 복구
- 모든 시리얼 작업에 타임아웃 적용
- Health Check를 통한 주기적 상태 확인
- 실패 시 자동 재초기화 시도

### 6. 멀티바이트 문자 처리
- UTF-8 문자 경계 보존
- 이스케이프 시퀀스(+++)와 멀티바이트 문자 구분
- 버퍼 경계에서 문자 분할 방지

## 개발 우선순위
1. **필수**: Serial port 안정성 (blocking fix, lock ordering)
2. **필수**: RING 감지 및 자동 응답
3. **필수**: 완전한 라인 버퍼링
4. **권장**: Health check 통합
5. **선택**: 성능 최적화

## 검증 기준
- [ ] 시리얼 포트 열기/닫기 100회 연속 성공
- [ ] RING 신호 감지 및 자동 응답 동작
- [ ] CONNECT 메시지 완전성 보장
- [ ] 30분 이상 연속 운영 안정성
- [ ] 멀티바이트 문자 전송 무결성