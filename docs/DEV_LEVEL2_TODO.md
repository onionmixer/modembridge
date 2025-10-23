# DEV_LEVEL2_TODO - Level 2 남은 작업

## 개요
Level 2 Telnet 프로토콜 계층에서 미완성되거나 개선이 필요한 작업 목록입니다.

## 우선순위: 높음 🔴

### 1. LINEMODE 완전 구현
**현재 상태**: 60% 구현 (기본 MODE만 지원)

**필요 작업**:
```c
// RFC 1184 완전 구현
// telnet.c에 추가 필요

// FORWARDMASK 구현
typedef struct {
    uint8_t forward_mask[32];  // 256 bits
    bool mask_active;
} linemode_forwardmask_t;

// SLC (Set Local Characters) 구현
typedef struct {
    uint8_t function;  // SLC_xxx
    uint8_t flags;     // SLC_DEFAULT, etc.
    uint8_t value;     // Character value
} slc_triplet_t;

int handle_linemode_forwardmask(telnet_t *telnet,
                                const uint8_t *mask,
                                size_t len);

int handle_linemode_slc(telnet_t *telnet,
                        const slc_triplet_t *slc,
                        size_t count);
```

**구현 항목**:
- [ ] FORWARDMASK 서브옵션 처리
- [ ] SLC 트리플렛 파싱
- [ ] MODE ACK 비트 처리
- [ ] SOFT_TAB, LIT_ECHO 지원

**예상 효과**: 완전한 라인 편집 기능

### 2. 추가 IAC 명령 구현
**현재 상태**: 기본 명령만 구현

**필요 작업**:
```c
// telnet.c에 추가
case TELNET_DM:   // Data Mark
    handle_data_mark(telnet);
    break;
case TELNET_BRK:  // Break
    handle_break(telnet);
    break;
case TELNET_IP:   // Interrupt Process
    handle_interrupt_process(telnet);
    break;
case TELNET_AO:   // Abort Output
    handle_abort_output(telnet);
    break;
case TELNET_AYT:  // Are You There
    telnet_send_string(telnet, "[Yes]\r\n");
    break;
case TELNET_EC:   // Erase Character
    handle_erase_character(telnet);
    break;
case TELNET_EL:   // Erase Line
    handle_erase_line(telnet);
    break;
```

**구현 항목**:
- [ ] Urgent 데이터 처리 (DM)
- [ ] Break 신호 전달
- [ ] 프로세스 인터럽트
- [ ] 출력 중단/재개

### 3. 에러 복구 메커니즘
**현재 상태**: 기본 에러 처리만 존재

**필요 작업**:
```c
// 자동 재연결 기능
typedef struct {
    int retry_count;
    int max_retries;
    int retry_delay_ms;
    time_t last_attempt;
} reconnect_state_t;

int telnet_auto_reconnect(telnet_t *telnet) {
    if (telnet->reconnect.retry_count >= telnet->reconnect.max_retries) {
        return ERROR_MAX_RETRIES;
    }

    // Exponential backoff
    int delay = telnet->reconnect.retry_delay_ms *
                (1 << telnet->reconnect.retry_count);
    usleep(delay * 1000);

    telnet->reconnect.retry_count++;
    return telnet_connect(telnet, telnet->host, telnet->port);
}
```

**구현 항목**:
- [ ] 연결 끊김 감지 개선
- [ ] 자동 재연결 로직
- [ ] Exponential backoff
- [ ] 상태 복구

## 우선순위: 중간 🟡

### 4. 추가 옵션 지원
**현재 상태**: 6개 옵션만 지원

**추가할 옵션**:
```c
#define TELOPT_STATUS     5   // Status
#define TELOPT_TIMING     6   // Timing Mark
#define TELOPT_RCTE      7   // Remote Controlled Trans and Echo
#define TELOPT_NAOL      8   // Output Line Width
#define TELOPT_NAOP      9   // Output Page Size
#define TELOPT_NAOCRD   10   // Output CR Disposition
#define TELOPT_NAOHTS   11   // Output Horizontal Tab Stops
#define TELOPT_NAOHTD   12   // Output Horizontal Tab Disposition
#define TELOPT_NAOFFD   13   // Output Formfeed Disposition
#define TELOPT_NAOVTS   14   // Output Vertical Tabstops
#define TELOPT_NAOVTD   15   // Output Vertical Tab Disposition
#define TELOPT_NAOLFD   16   // Output Linefeed Disposition
#define TELOPT_XASCII   17   // Extended ASCII
#define TELOPT_LOGOUT   18   // Logout
#define TELOPT_BM       19   // Byte Macro
#define TELOPT_DET      20   // Data Entry Terminal
#define TELOPT_SUPDUP   21   // SUPDUP
#define TELOPT_SUPDUPOUTPUT 22  // SUPDUP Output
#define TELOPT_SNDLOC   23   // Send Location
```

**구현 항목**:
- [ ] STATUS 옵션
- [ ] TIMING MARK
- [ ] LOGOUT 처리
- [ ] 확장 ASCII

### 5. 성능 최적화
**현재 상태**: 기본 성능

**개선 영역**:
```c
// 버퍼 풀링
typedef struct {
    uint8_t *buffers[MAX_POOL_SIZE];
    size_t buffer_size;
    int free_count;
    pthread_mutex_t mutex;
} buffer_pool_t;

// Zero-copy 전송
int telnet_send_zerocopy(telnet_t *telnet,
                         const uint8_t *data,
                         size_t len) {
    // MSG_ZEROCOPY 사용
    return send(telnet->socket_fd, data, len,
                MSG_ZEROCOPY | MSG_NOSIGNAL);
}
```

**구현 항목**:
- [ ] 버퍼 풀 구현
- [ ] Zero-copy I/O
- [ ] Nagle 알고리즘 튜닝
- [ ] 배치 처리

### 6. 보안 기능
**현재 상태**: 보안 미고려

**필요 기능**:
```c
// 옵션 거부 목록
typedef struct {
    uint8_t blocked_options[256/8];  // Bitmap
    bool strict_mode;
} security_config_t;

// 입력 검증
int validate_iac_sequence(const uint8_t *data, size_t len) {
    // IAC 시퀀스 검증
    // 버퍼 오버플로우 방지
    // 악의적 패턴 감지
}
```

**구현 항목**:
- [ ] 옵션 블랙리스트
- [ ] 입력 크기 제한
- [ ] IAC 폭탄 방지
- [ ] 로깅 강화

## 우선순위: 낮음 🟢

### 7. 진단 기능
**현재 상태**: 기본 로깅만 존재

**추가 기능**:
```c
// 통계 수집
typedef struct {
    uint64_t bytes_sent;
    uint64_t bytes_received;
    uint64_t iac_commands;
    uint64_t options_negotiated;
    time_t connection_time;
    double average_latency;
} telnet_stats_t;

// 프로토콜 분석
void telnet_dump_state(telnet_t *telnet) {
    printf("=== Telnet State ===\n");
    printf("Parser State: %s\n", state_to_string(telnet->state));
    printf("Options:\n");
    for (int i = 0; i < 256; i++) {
        if (telnet->options[i].local_enabled ||
            telnet->options[i].remote_enabled) {
            printf("  Option %d: Local=%s, Remote=%s\n", i,
                   telnet->options[i].local_enabled ? "ON" : "OFF",
                   telnet->options[i].remote_enabled ? "ON" : "OFF");
        }
    }
}
```

**구현 항목**:
- [ ] 통계 수집 모듈
- [ ] 상태 덤프 기능
- [ ] 패킷 캡처 통합
- [ ] 성능 프로파일링

### 8. 테스트 자동화
**현재 상태**: 수동 테스트

**필요 작업**:
```c
// 자동화 테스트 프레임워크
typedef struct {
    const char *name;
    int (*test_func)(telnet_t *);
    bool enabled;
} test_case_t;

static test_case_t test_cases[] = {
    {"IAC Escaping", test_iac_escaping, true},
    {"Option Negotiation", test_option_negotiation, true},
    {"Multibyte Characters", test_multibyte, true},
    {"Mode Switching", test_mode_switch, true},
    {"Error Recovery", test_error_recovery, true},
    {NULL, NULL, false}
};

int run_all_tests(telnet_t *telnet) {
    int passed = 0, failed = 0;

    for (int i = 0; test_cases[i].name != NULL; i++) {
        if (!test_cases[i].enabled) continue;

        printf("Running: %s... ", test_cases[i].name);
        if (test_cases[i].test_func(telnet) == SUCCESS) {
            printf("PASS\n");
            passed++;
        } else {
            printf("FAIL\n");
            failed++;
        }
    }

    printf("Results: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? SUCCESS : ERROR_TEST_FAILED;
}
```

**구현 항목**:
- [ ] 단위 테스트 프레임워크
- [ ] 통합 테스트 스크립트
- [ ] 모의 서버 구현
- [ ] CI/CD 통합

### 9. 문서 개선
**현재 상태**: 기본 문서만 존재

**필요 문서**:
- [ ] Telnet 프로토콜 가이드
- [ ] 옵션 협상 흐름도
- [ ] 트러블슈팅 가이드
- [ ] API 레퍼런스

## 구현 로드맵

### 단기 (1-2주)
1. LINEMODE 완전 구현 🔴
2. 추가 IAC 명령 🔴
3. 에러 복구 메커니즘 🔴

### 중기 (3-4주)
4. 추가 옵션 지원 🟡
5. 성능 최적화 🟡
6. 보안 기능 🟡

### 장기 (선택적)
7. 진단 기능 🟢
8. 테스트 자동화 🟢
9. 문서 개선 🟢

## 테스트 요구사항

### 각 기능별 테스트
- [ ] LINEMODE: 모든 서브옵션 동작
- [ ] IAC 명령: 각 명령별 처리
- [ ] 재연결: 10회 연속 성공
- [ ] 성능: 115,200 bps 처리
- [ ] 보안: 악의적 입력 방어

### 회귀 테스트
- [ ] 기존 기능 영향 없음
- [ ] 메모리 누수 없음
- [ ] 성능 저하 없음

## 알려진 이슈

### 이슈 #1: LINEMODE ACK 처리
- **증상**: MODE ACK 비트 무시
- **영향**: 모드 동기화 실패 가능
- **우선순위**: 높음

### 이슈 #2: 대용량 서브협상
- **증상**: 4KB 이상 SB 데이터 처리 실패
- **영향**: 일부 옵션 제한
- **우선순위**: 중간

### 이슈 #3: IPv6 지원
- **증상**: IPv4만 지원
- **영향**: IPv6 환경 사용 불가
- **우선순위**: 낮음

## 참고사항

### RFC 문서
- RFC 854: Telnet Protocol Specification
- RFC 855: Telnet Option Specifications
- RFC 856: Binary Transmission
- RFC 857: Echo
- RFC 858: Suppress Go Ahead
- RFC 1091: Terminal Type
- RFC 1184: Linemode
- RFC 2217: Serial over Telnet

### 테스트 서버
- 포트 9091: 라인 모드
- 포트 9092: 문자 모드
- 포트 9093: 바이너리 라인 모드

---
*최종 업데이트: 2025-10-23*
*다음 검토: 2025-11-01*