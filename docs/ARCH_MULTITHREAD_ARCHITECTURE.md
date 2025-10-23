# ModemBridge 멀티스레드 아키텍처 설계

## 관련 문서
- **개발 구간 정의**: ARCH_LEVEL_DEFINITION.txt - Level 1/2/3 개요
- **구현 가이드**: ARCH_IMPLEMENTATION_PLAN.md - 상세 구현 코드
- **데이터 경로 분석**: ARCH_DATA_PATH_REVIEW.md - 멀티바이트 문자 처리

## 개요

ModemBridge를 단일 스레드 select() 기반에서 멀티스레드 구조로 변경합니다.

## 목표

- **Level 1**: Serial/Modem I/O와 상태 관리를 전담하는 스레드
- **Level 2**: Telnet I/O와 프로토콜 처리를 전담하는 스레드
- **Level 3**: 스레드 간 안전한 데이터 동기화 메커니즘

## 현재 아키텍처 (단일 스레드)

```
┌─────────────────────────────────────────────────────┐
│           main.c (Main Loop)                        │
│                                                     │
│  ┌───────────────────────────────────────────────┐ │
│  │  bridge_run() - select() based I/O mux        │ │
│  │                                                │ │
│  │  ┌──────────────────┐  ┌──────────────────┐  │ │
│  │  │ Serial FD ready? │  │ Telnet FD ready? │  │ │
│  │  │ ↓ call           │  │ ↓ call           │  │ │
│  │  │ bridge_process_  │  │ bridge_process_  │  │ │
│  │  │ serial_data()    │  │ telnet_data()    │  │ │
│  │  └──────────────────┘  └──────────────────┘  │ │
│  └───────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────┘
```

**문제점:**
- 단일 스레드에서 모든 I/O 처리 → 동시성 부족
- select() 타임아웃(1초)으로 인한 latency
- 한쪽 I/O가 블록되면 전체 영향

## 새로운 아키텍처 (멀티스레드)

```
┌──────────────────────────────────────────────────────────────────┐
│                       Main Thread                                │
│  - 초기화 및 스레드 생성                                            │
│  - 시그널 처리                                                     │
│  - 종료 시 스레드 join                                             │
└──────────────────────────────────────────────────────────────────┘
          │                                 │
          │ pthread_create()                │ pthread_create()
          ↓                                 ↓
┌──────────────────────────┐      ┌──────────────────────────┐
│  Thread 1: Serial/Modem  │      │  Thread 2: Telnet        │
│                          │      │                          │
│  ┌────────────────────┐  │      │  ┌────────────────────┐  │
│  │ Serial I/O Loop    │  │      │  │ Telnet I/O Loop    │  │
│  │                    │  │      │  │                    │  │
│  │ - serial_read()    │  │      │  │ - telnet_recv()    │  │
│  │ - modem_process_   │  │      │  │ - telnet_process_  │  │
│  │   input()          │  │      │  │   input()          │  │
│  │ - AT 명령 처리      │  │      │  │ - IAC 처리         │  │
│  └────────────────────┘  │      │  └────────────────────┘  │
│           │              │      │           │              │
│           ↓              │      │           ↓              │
│  ┌────────────────────┐  │      │  ┌────────────────────┐  │
│  │ Data to Telnet     │  │      │  │ Data from Telnet   │  │
│  │ ANSI filter        │  │      │  │ IAC removed        │  │
│  │ IAC escape         │  │      │  │ ANSI passthrough   │  │
│  └────────────────────┘  │      │  └────────────────────┘  │
│           │              │      │           │              │
│           ↓              │      │           ↓              │
│    [serial_to_telnet]◄───┼──────┼──────[serial_to_telnet] │
│        _buf (mutex)      │      │          _buf (read)     │
│           │              │      │                          │
│           ↑              │      │           │              │
│    [telnet_to_serial]    │      │    [telnet_to_serial]───┼─►
│        _buf (read)       │      │        _buf (mutex)      │
└──────────────────────────┘      └──────────────────────────┘
```

## Level 1: Serial/Modem Thread

### 책임

1. Serial port에서 데이터 읽기 (`serial_read()`)
2. Modem 계층 처리 (`modem_process_input()`)
   - **COMMAND 모드**: AT 명령 파싱 및 응답
   - **ONLINE 모드**: 데이터 전달 및 escape sequence 감지
3. Serial → Telnet 방향 데이터 처리:
   - ANSI 필터링 (`ansi_filter_modem_to_telnet()`)
   - IAC escaping (`telnet_prepare_output()`)
   - Thread-safe buffer에 쓰기
4. Telnet → Serial 방향 데이터 전송:
   - Thread-safe buffer에서 읽기
   - Serial port로 전송 (`serial_write()`)

### 주요 루프

```c
void *serial_modem_thread_func(void *arg) {
    bridge_ctx_t *ctx = (bridge_ctx_t *)arg;
    unsigned char buf[BUFFER_SIZE];

    while (ctx->thread_running) {
        // 1. Serial에서 데이터 읽기
        ssize_t n = serial_read(&ctx->serial, buf, sizeof(buf));
        if (n > 0) {
            // 2. Modem 처리
            if (modem_is_online(&ctx->modem)) {
                // ONLINE: 데이터 전달
                // ANSI filter → IAC escape → write to buffer
                pthread_mutex_lock(&ctx->serial_to_telnet_buf.mutex);
                cbuf_write(&ctx->serial_to_telnet_buf.cbuf, processed, len);
                pthread_cond_signal(&ctx->serial_to_telnet_buf.cond_not_empty);
                pthread_mutex_unlock(&ctx->serial_to_telnet_buf.mutex);
            } else {
                // COMMAND: AT 명령 처리
                modem_process_input(&ctx->modem, buf, n);
            }
        }

        // 3. Telnet→Serial 버퍼에서 읽어서 전송
        pthread_mutex_lock(&ctx->telnet_to_serial_buf.mutex);
        if (cbuf_available(&ctx->telnet_to_serial_buf.cbuf) > 0) {
            size_t n = cbuf_read(&ctx->telnet_to_serial_buf.cbuf, buf, sizeof(buf));
            pthread_mutex_unlock(&ctx->telnet_to_serial_buf.mutex);
            serial_write(&ctx->serial, buf, n);
        } else {
            pthread_mutex_unlock(&ctx->telnet_to_serial_buf.mutex);
        }

        // 짧은 sleep (CPU 부하 방지)
        usleep(1000); // 1ms
    }

    return NULL;
}
```

## Level 2: Telnet Thread

### 책임

1. Telnet server 연결 관리 (`telnet_connect()`)
2. Telnet I/O (`telnet_recv()`)
3. IAC 프로토콜 처리 (`telnet_process_input()`)
4. Option negotiation 처리
5. Telnet → Serial 방향 데이터 처리:
   - IAC 제거
   - ANSI passthrough
   - Thread-safe buffer에 쓰기
6. Serial → Telnet 방향 데이터 전송:
   - Thread-safe buffer에서 읽기
   - Telnet server로 전송 (`telnet_send()`)

### 주요 루프

```c
void *telnet_thread_func(void *arg) {
    bridge_ctx_t *ctx = (bridge_ctx_t *)arg;
    unsigned char buf[BUFFER_SIZE];

    while (ctx->thread_running) {
        // Telnet 연결 확인 (없으면 대기)
        if (!telnet_is_connected(&ctx->telnet)) {
            usleep(100000); // 100ms
            continue;
        }

        // 1. Telnet에서 데이터 읽기
        ssize_t n = telnet_recv(&ctx->telnet, buf, sizeof(buf));
        if (n > 0) {
            // 2. IAC 프로토콜 처리
            unsigned char processed[BUFFER_SIZE];
            size_t processed_len;
            telnet_process_input(&ctx->telnet, buf, n,
                                processed, sizeof(processed), &processed_len);

            if (processed_len > 0) {
                // 3. Thread-safe buffer에 쓰기
                pthread_mutex_lock(&ctx->telnet_to_serial_buf.mutex);
                cbuf_write(&ctx->telnet_to_serial_buf.cbuf, processed, processed_len);
                pthread_cond_signal(&ctx->telnet_to_serial_buf.cond_not_empty);
                pthread_mutex_unlock(&ctx->telnet_to_serial_buf.mutex);
            }
        }

        // 4. Serial→Telnet 버퍼에서 읽어서 전송
        pthread_mutex_lock(&ctx->serial_to_telnet_buf.mutex);
        if (cbuf_available(&ctx->serial_to_telnet_buf.cbuf) > 0) {
            size_t n = cbuf_read(&ctx->serial_to_telnet_buf.cbuf, buf, sizeof(buf));
            pthread_mutex_unlock(&ctx->serial_to_telnet_buf.mutex);
            telnet_send(&ctx->telnet, buf, n);
        } else {
            pthread_mutex_unlock(&ctx->serial_to_telnet_buf.mutex);
        }

        usleep(1000); // 1ms
    }

    return NULL;
}
```

## Level 3: 동기화 메커니즘

### Thread-Safe Circular Buffer

```c
typedef struct {
    circular_buffer_t cbuf;             // 기존 circular buffer 재사용
    pthread_mutex_t mutex;              // 버퍼 접근 동기화
    pthread_cond_t cond_not_empty;      // 데이터 도착 알림
    pthread_cond_t cond_not_full;       // 공간 확보 알림
} ts_circular_buffer_t;

/* 초기화 */
void ts_cbuf_init(ts_circular_buffer_t *tsbuf) {
    cbuf_init(&tsbuf->cbuf);
    pthread_mutex_init(&tsbuf->mutex, NULL);
    pthread_cond_init(&tsbuf->cond_not_empty, NULL);
    pthread_cond_init(&tsbuf->cond_not_full, NULL);
}

/* Thread-safe 쓰기 */
size_t ts_cbuf_write(ts_circular_buffer_t *tsbuf, const unsigned char *data, size_t len) {
    pthread_mutex_lock(&tsbuf->mutex);

    // 버퍼가 가득 찬 경우 대기 (선택적)
    while (cbuf_free(&tsbuf->cbuf) == 0) {
        pthread_cond_wait(&tsbuf->cond_not_full, &tsbuf->mutex);
    }

    size_t written = cbuf_write(&tsbuf->cbuf, data, len);

    // 데이터 도착 알림
    if (written > 0) {
        pthread_cond_signal(&tsbuf->cond_not_empty);
    }

    pthread_mutex_unlock(&tsbuf->mutex);
    return written;
}

/* Thread-safe 읽기 */
size_t ts_cbuf_read(ts_circular_buffer_t *tsbuf, unsigned char *data, size_t len) {
    pthread_mutex_lock(&tsbuf->mutex);

    // 데이터가 없는 경우 대기 (선택적)
    while (cbuf_available(&tsbuf->cbuf) == 0) {
        pthread_cond_wait(&tsbuf->cond_not_empty, &tsbuf->mutex);
    }

    size_t read = cbuf_read(&tsbuf->cbuf, data, len);

    // 공간 확보 알림
    if (read > 0) {
        pthread_cond_signal(&tsbuf->cond_not_full);
    }

    pthread_mutex_unlock(&tsbuf->mutex);
    return read;
}
```

### 공유 자원 보호

| 자원 | 접근 스레드 | 보호 방법 |
|------|------------|----------|
| `serial_to_telnet_buf` | Thread 1 (write), Thread 2 (read) | `ts_circular_buffer_t` (자체 mutex) |
| `telnet_to_serial_buf` | Thread 2 (write), Thread 1 (read) | `ts_circular_buffer_t` (자체 mutex) |
| `modem.state` | Thread 1 (read/write), Thread 2 (read) | `ctx->modem_mutex` |
| `ctx->state` | Thread 1, Thread 2 | `ctx->state_mutex` |
| `serial` | Thread 1 only | No mutex (단일 스레드 전용) |
| `telnet` | Thread 2 only | No mutex (단일 스레드 전용) |

### 초기화 및 종료

```c
/* bridge_start()에서 스레드 생성 */
int bridge_start(bridge_ctx_t *ctx) {
    // ... 기존 초기화 ...

    // Thread-safe 버퍼 초기화
    ts_cbuf_init(&ctx->serial_to_telnet_buf);
    ts_cbuf_init(&ctx->telnet_to_serial_buf);

    // Mutex 초기화
    pthread_mutex_init(&ctx->state_mutex, NULL);
    pthread_mutex_init(&ctx->modem_mutex, NULL);

    // 스레드 시작
    ctx->thread_running = true;
    pthread_create(&ctx->serial_thread, NULL, serial_modem_thread_func, ctx);
    pthread_create(&ctx->telnet_thread, NULL, telnet_thread_func, ctx);

    return SUCCESS;
}

/* bridge_stop()에서 스레드 종료 */
int bridge_stop(bridge_ctx_t *ctx) {
    // 스레드 종료 요청
    ctx->thread_running = false;

    // Condition variable 시그널 (대기 중인 스레드 깨우기)
    pthread_cond_broadcast(&ctx->serial_to_telnet_buf.cond_not_empty);
    pthread_cond_broadcast(&ctx->telnet_to_serial_buf.cond_not_empty);

    // 스레드 종료 대기
    pthread_join(ctx->serial_thread, NULL);
    pthread_join(ctx->telnet_thread, NULL);

    // Mutex/condition variable 정리
    pthread_mutex_destroy(&ctx->state_mutex);
    pthread_mutex_destroy(&ctx->modem_mutex);
    ts_cbuf_destroy(&ctx->serial_to_telnet_buf);
    ts_cbuf_destroy(&ctx->telnet_to_serial_buf);

    // ... 기존 cleanup ...

    return SUCCESS;
}
```

## 구현 순서

상세한 구현 계획 및 완전한 코드는 `ARCH_IMPLEMENTATION_PLAN.md` 참조.

**구현 단계 요약:**
- **Phase 1**: Thread-Safe Buffer 구현
- **Phase 2**: Serial/Modem Thread (Level 1)
- **Phase 3**: Telnet Thread (Level 2)
- **Phase 4**: 통합 및 테스트
- **Phase 5**: 정리 및 최적화

## 성능 고려사항

### 장점
- **낮은 latency**: select() 타임아웃(1초) 없음
- **진정한 동시성**: Serial과 Telnet I/O가 독립적으로 처리
- **블로킹 격리**: 한쪽 I/O가 블록되어도 다른 쪽 영향 없음

### 주의사항
- **Mutex contention**: 버퍼 접근이 빈번하면 lock contention 발생 가능
  - **해결**: Critical section 최소화, lock 시간 단축
- **Context switching overhead**: 스레드 간 전환 비용
  - **해결**: 적절한 sleep으로 CPU 부하 조절 (1ms)
- **Deadlock 위험**: 여러 mutex를 사용할 경우
  - **해결**: Lock ordering rule 준수, timeout 사용

## 호환성

- **POSIX threads**: `pthread.h` (Ubuntu 22.04 LTS 기본 지원)
- **Compiler flags**: `-pthread` 추가 필요 (Makefile 수정)
- **Zero external dependencies**: glibc와 POSIX API만 사용 (기존 방침 유지)

## 마이그레이션 전략

1. **기존 코드 보존**: select() 기반 코드를 `#ifdef SINGLE_THREAD` 블록으로 보존
2. **점진적 마이그레이션**: Level 1 → Level 2 → Level 3 순차 구현
3. **Feature flag**: 컴파일 타임에 single-thread / multi-thread 선택 가능
4. **테스트**: 각 Phase마다 기존 기능 회귀 테스트

---

**작성일**: 2025-10-15
**버전**: 1.0
**상태**: 설계 완료, 구현 대기
