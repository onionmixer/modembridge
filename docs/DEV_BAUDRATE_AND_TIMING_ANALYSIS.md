# DEV_BAUDRATE_AND_TIMING_ANALYSIS - Baudrate 및 Timing 분석

## 개요
ModemBridge의 다양한 baudrate 지원과 timing 관련 분석을 통합한 문서입니다.
저속(300-2400bps)부터 고속(115200bps)까지의 전체 baudrate 스펙트럼을 다룹니다.

## 1. Baudrate별 특성 분석

### 1.1 저속 구간 (300-2400 bps)

#### 300 bps (Bell 103)
```
- 1 문자 전송 시간: 36.67ms (11 bits @ 300bps)
- 초당 최대 문자수: 27.27자
- 응답 지연 허용치: 1000ms
- 주요 용도: 초기 모뎀, 텔레타이프
```

#### 1200 bps (Bell 212A)
```
- 1 문자 전송 시간: 9.17ms
- 초당 최대 문자수: 109자
- 응답 지연 허용치: 500ms
- 주요 용도: 초기 PC 통신, BBS
```

#### 2400 bps (V.22bis)
```
- 1 문자 전송 시간: 4.58ms
- 초당 최대 문자수: 218자
- 응답 지연 허용치: 250ms
- 주요 용도: 표준 BBS, 텍스트 전송
```

### 1.2 중속 구간 (4800-19200 bps)

#### 9600 bps
```
- 1 문자 전송 시간: 1.15ms
- 초당 최대 문자수: 872자
- 버퍼 권장 크기: 1024 bytes
- 주요 용도: 표준 터미널, 원격 접속
```

#### 19200 bps
```
- 1 문자 전송 시간: 0.57ms
- 초당 최대 문자수: 1745자
- 버퍼 권장 크기: 2048 bytes
- 주요 용도: 향상된 터미널, 파일 전송
```

### 1.3 고속 구간 (38400-115200 bps)

#### 57600 bps
```
- 1 문자 전송 시간: 0.19ms
- 초당 최대 문자수: 5236자
- 버퍼 권장 크기: 4096 bytes
- 주요 용도: 고속 모뎀, PPP 연결
```

#### 115200 bps
```
- 1 문자 전송 시간: 0.095ms
- 초당 최대 문자수: 10472자
- 버퍼 권장 크기: 8192 bytes
- 주요 용도: 최대 속도 연결
```

## 2. Timing 계산 및 분석

### 2.1 문자 전송 시간 계산
```c
// 11 bits per character (8 data + 1 start + 2 stop)
double char_time_ms = (11.0 * 1000.0) / baudrate;

// 예시 계산
300 bps: 11 * 1000 / 300 = 36.67ms
1200 bps: 11 * 1000 / 1200 = 9.17ms
2400 bps: 11 * 1000 / 2400 = 4.58ms
9600 bps: 11 * 1000 / 9600 = 1.15ms
```

### 2.2 버퍼 크기 권장사항
| Baudrate | 최소 버퍼 | 권장 버퍼 | 최대 버퍼 |
|----------|-----------|-----------|-----------|
| 300      | 64        | 128       | 256       |
| 1200     | 128       | 256       | 512       |
| 2400     | 256       | 512       | 1024      |
| 9600     | 512       | 1024      | 2048      |
| 19200    | 1024      | 2048      | 4096      |
| 57600    | 2048      | 4096      | 8192      |
| 115200   | 4096      | 8192      | 16384     |

### 2.3 Timeout 설정
```c
// Baudrate별 timeout 권장값
int get_timeout_ms(int baudrate) {
    if (baudrate <= 300) return 2000;
    if (baudrate <= 1200) return 1000;
    if (baudrate <= 2400) return 500;
    if (baudrate <= 9600) return 250;
    if (baudrate <= 19200) return 100;
    if (baudrate <= 57600) return 50;
    return 20;  // 115200 이상
}
```

## 3. ModemBridge 구현 상태

### 3.1 코드 확인 결과

#### serial.c 분석
```c
// 현재 지원 baudrate (serial_init_port)
static speed_t baud_to_speed(int baudrate) {
    switch(baudrate) {
        case 300: return B300;
        case 1200: return B1200;
        case 2400: return B2400;
        case 4800: return B4800;
        case 9600: return B9600;
        case 19200: return B19200;
        case 38400: return B38400;
        case 57600: return B57600;
        case 115200: return B115200;
        case 230400: return B230400;
        default: return B9600;
    }
}

// 동적 baudrate 변경 (serial_set_baudrate)
int serial_set_baudrate(serial_t *serial, int baudrate) {
    speed_t speed = baud_to_speed(baudrate);
    // termios 설정 변경
    cfsetispeed(&serial->tio, speed);
    cfsetospeed(&serial->tio, speed);
    return tcsetattr(serial->fd, TCSANOW, &serial->tio);
}
```

#### modem.c 분석
```c
// AT&B 명령 처리 (자동 baudrate 조정)
case 'B':  // AT&B - Baudrate settings
    if (strchr(cmd_ptr, '0')) {
        // AT&B0 - Fixed DTE rate
        modem->autobaud = false;
    } else if (strchr(cmd_ptr, '1')) {
        // AT&B1 - Variable, follows connection
        modem->autobaud = true;
    }
    break;

// CONNECT 응답시 baudrate 표시
if (modem->connect_speed > 0) {
    snprintf(response, sizeof(response),
             "CONNECT %d\r\n", modem->connect_speed);
} else {
    strcpy(response, "CONNECT\r\n");
}
```

### 3.2 검증 완료 항목
✅ 모든 표준 baudrate 지원 (300-115200)
✅ 동적 baudrate 변경
✅ AT&B 명령 구현
✅ CONNECT 응답에 속도 표시
✅ Autobaud 협상 지원

### 3.3 알려진 제한사항
- 230400bps 이상은 하드웨어 의존적
- 일부 USB-시리얼 변환기에서 저속 불안정
- Windows에서 300bps 지원 제한적

## 4. 성능 최적화

### 4.1 Baudrate별 최적화 전략

#### 저속 (300-2400bps)
```c
// 작은 버퍼, 긴 timeout
#define LOW_SPEED_BUFFER 256
#define LOW_SPEED_TIMEOUT 1000

// select timeout 조정
if (baudrate <= 2400) {
    timeout.tv_sec = 1;
    timeout.tv_usec = 0;
}
```

#### 중속 (4800-19200bps)
```c
// 중간 버퍼, 표준 timeout
#define MID_SPEED_BUFFER 1024
#define MID_SPEED_TIMEOUT 100

// Nagle 알고리즘 활성화
if (baudrate >= 4800 && baudrate <= 19200) {
    int flag = 0;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY,
               &flag, sizeof(flag));
}
```

#### 고속 (38400bps 이상)
```c
// 큰 버퍼, 짧은 timeout
#define HIGH_SPEED_BUFFER 4096
#define HIGH_SPEED_TIMEOUT 20

// Nagle 비활성화, 즉시 전송
if (baudrate >= 38400) {
    int flag = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY,
               &flag, sizeof(flag));
}
```

### 4.2 Flow Control 설정
```c
// Baudrate별 흐름 제어
if (baudrate <= 9600) {
    // 소프트웨어 흐름 제어 (XON/XOFF)
    tio.c_iflag |= IXON | IXOFF;
} else {
    // 하드웨어 흐름 제어 (RTS/CTS)
    tio.c_cflag |= CRTSCTS;
}
```

## 5. 테스트 시나리오

### 5.1 Baudrate 전환 테스트
```bash
# 저속 시작, 고속 전환
AT&B1         # Autobaud 활성화
ATDT host     # 연결
# 서버가 57600 협상 → 자동 전환
```

### 5.2 Stress 테스트
```bash
# 각 baudrate별 1MB 파일 전송
for rate in 300 1200 2400 9600 19200 57600 115200; do
    echo "Testing $rate bps"
    time sz -b $rate testfile.dat
done
```

### 5.3 Timing 정확도 테스트
```c
// 문자 간격 측정
struct timespec start, end;
clock_gettime(CLOCK_MONOTONIC, &start);
write(fd, "A", 1);
tcdrain(fd);  // 전송 완료 대기
clock_gettime(CLOCK_MONOTONIC, &end);

double elapsed = (end.tv_sec - start.tv_sec) * 1000.0 +
                 (end.tv_nsec - start.tv_nsec) / 1000000.0;
double expected = (11.0 * 1000.0) / baudrate;
double error = fabs(elapsed - expected) / expected * 100;

printf("Expected: %.2fms, Actual: %.2fms, Error: %.1f%%\n",
       expected, elapsed, error);
```

## 6. 트러블슈팅

### 6.1 저속 연결 문제
**증상**: 300-1200bps에서 문자 깨짐
**원인**: 타이밍 부정확
**해결**:
- Stop bit 2개 사용
- 더 큰 timeout 설정
- 버퍼 크기 축소

### 6.2 고속 연결 문제
**증상**: 57600bps 이상에서 데이터 손실
**원인**: 버퍼 오버플로우
**해결**:
- 버퍼 크기 증가
- 하드웨어 흐름 제어 필수
- TCP_NODELAY 설정

### 6.3 Autobaud 실패
**증상**: AT&B1 후 속도 감지 실패
**원인**: 패턴 인식 오류
**해결**:
- 초기 AT 명령으로 훈련
- 명확한 CR/LF 전송
- Fallback 속도 설정

## 7. 구현 권장사항

### 7.1 향후 개선 사항
1. **적응형 버퍼 크기**: Baudrate에 따른 동적 조정
2. **지능형 Timeout**: 트래픽 패턴 학습
3. **Error Correction**: 저속 연결용 재전송 로직
4. **Compression**: 저속 연결용 압축 옵션

### 7.2 코드 개선 제안
```c
// 구조체로 baudrate 프로파일 관리
typedef struct {
    int baudrate;
    size_t buffer_size;
    int timeout_ms;
    bool use_hw_flow;
    bool use_nagle;
} baudrate_profile_t;

static const baudrate_profile_t profiles[] = {
    {300,    128,  2000, false, true},
    {1200,   256,  1000, false, true},
    {2400,   512,  500,  false, true},
    {9600,   1024, 250,  false, true},
    {19200,  2048, 100,  true,  true},
    {57600,  4096, 50,   true,  false},
    {115200, 8192, 20,   true,  false},
};
```

## 8. 참고 자료

### 표준 문서
- ITU-T V.24: DTE/DCE Interface
- ITU-T V.250: AT Command Set
- EIA-232: Serial Communication Standard

### 역사적 모뎀 표준
- Bell 103: 300 bps (1962)
- Bell 212A: 1200 bps (1976)
- V.22bis: 2400 bps (1984)
- V.32: 9600 bps (1991)
- V.34: 33600 bps (1994)
- V.90: 56K (1998)

---
*통합 문서 생성: 2025-10-23*
*원본: DEV_2400BPS_ANALYSIS.md, DEV_BAUDRATE_ANALYSIS.md*