# ModemBridge 멀티스레드 구현 계획

## 개요

이 문서는 ModemBridge를 단일 스레드에서 멀티스레드 구조로 변경하기 위한 상세 구현 계획입니다.

## 전체 구현 단계

```
Phase 1: Thread-Safe Buffer 구현
    ↓
Phase 2: Level 1 구현 (Serial/Modem Thread)
    ↓
Phase 3: Level 2 구현 (Telnet Thread)
    ↓
Phase 4: Level 3 통합 및 테스트
    ↓
Phase 5: 정리 및 최적화
```

---

## Phase 1: Thread-Safe Buffer 구현

### 목표
기존 `circular_buffer_t`를 래핑하여 mutex와 condition variable로 보호하는 thread-safe 버전 구현

### 작업 파일

#### 1.1. `include/bridge.h` 수정

**추가할 내용:**
```c
#include <pthread.h>

/* Thread-safe circular buffer */
typedef struct {
    circular_buffer_t cbuf;             /* 기존 circular buffer 재사용 */
    pthread_mutex_t mutex;              /* 버퍼 접근 동기화 */
    pthread_cond_t cond_not_empty;      /* 데이터 도착 알림 */
    pthread_cond_t cond_not_full;       /* 공간 확보 알림 */
} ts_circular_buffer_t;

/* Bridge context 수정 */
typedef struct {
    // ... 기존 필드 ...

    /* Thread handles */
    pthread_t serial_thread;
    pthread_t telnet_thread;

    /* Thread-safe buffers (기존 circular_buffer_t를 대체) */
    ts_circular_buffer_t serial_to_telnet_buf;
    ts_circular_buffer_t telnet_to_serial_buf;

    /* Shared state protection */
    pthread_mutex_t state_mutex;        /* connection_state_t 보호 */
    pthread_mutex_t modem_mutex;        /* modem_t 보호 */

    /* Thread control */
    bool thread_running;                /* 스레드 실행 플래그 */
} bridge_ctx_t;

/* Thread-safe circular buffer API */
void ts_cbuf_init(ts_circular_buffer_t *tsbuf);
void ts_cbuf_destroy(ts_circular_buffer_t *tsbuf);
size_t ts_cbuf_write(ts_circular_buffer_t *tsbuf, const unsigned char *data, size_t len);
size_t ts_cbuf_read(ts_circular_buffer_t *tsbuf, unsigned char *data, size_t len);
size_t ts_cbuf_write_timeout(ts_circular_buffer_t *tsbuf, const unsigned char *data, size_t len, int timeout_ms);
size_t ts_cbuf_read_timeout(ts_circular_buffer_t *tsbuf, unsigned char *data, size_t len, int timeout_ms);
bool ts_cbuf_is_empty(ts_circular_buffer_t *tsbuf);
size_t ts_cbuf_available(ts_circular_buffer_t *tsbuf);
```

#### 1.2. `src/bridge.c` 수정

**추가할 함수:**
```c
/**
 * Initialize thread-safe circular buffer
 */
void ts_cbuf_init(ts_circular_buffer_t *tsbuf)
{
    if (tsbuf == NULL) {
        return;
    }

    cbuf_init(&tsbuf->cbuf);
    pthread_mutex_init(&tsbuf->mutex, NULL);
    pthread_cond_init(&tsbuf->cond_not_empty, NULL);
    pthread_cond_init(&tsbuf->cond_not_full, NULL);
}

/**
 * Destroy thread-safe circular buffer
 */
void ts_cbuf_destroy(ts_circular_buffer_t *tsbuf)
{
    if (tsbuf == NULL) {
        return;
    }

    pthread_mutex_destroy(&tsbuf->mutex);
    pthread_cond_destroy(&tsbuf->cond_not_empty);
    pthread_cond_destroy(&tsbuf->cond_not_full);
}

/**
 * Write data to thread-safe circular buffer
 */
size_t ts_cbuf_write(ts_circular_buffer_t *tsbuf, const unsigned char *data, size_t len)
{
    if (tsbuf == NULL || data == NULL) {
        return 0;
    }

    pthread_mutex_lock(&tsbuf->mutex);

    /* Non-blocking: write what we can */
    size_t written = cbuf_write(&tsbuf->cbuf, data, len);

    /* Signal waiting readers if we wrote data */
    if (written > 0) {
        pthread_cond_signal(&tsbuf->cond_not_empty);
    }

    pthread_mutex_unlock(&tsbuf->mutex);
    return written;
}

/**
 * Read data from thread-safe circular buffer
 */
size_t ts_cbuf_read(ts_circular_buffer_t *tsbuf, unsigned char *data, size_t len)
{
    if (tsbuf == NULL || data == NULL) {
        return 0;
    }

    pthread_mutex_lock(&tsbuf->mutex);

    /* Non-blocking: read what's available */
    size_t read = cbuf_read(&tsbuf->cbuf, data, len);

    /* Signal waiting writers if we freed space */
    if (read > 0) {
        pthread_cond_signal(&tsbuf->cond_not_full);
    }

    pthread_mutex_unlock(&tsbuf->mutex);
    return read;
}

/**
 * Write with timeout (blocking until space available or timeout)
 */
size_t ts_cbuf_write_timeout(ts_circular_buffer_t *tsbuf, const unsigned char *data, size_t len, int timeout_ms)
{
    if (tsbuf == NULL || data == NULL) {
        return 0;
    }

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeout_ms / 1000;
    ts.tv_nsec += (timeout_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }

    pthread_mutex_lock(&tsbuf->mutex);

    /* Wait for space if buffer is full */
    while (cbuf_free(&tsbuf->cbuf) == 0) {
        int ret = pthread_cond_timedwait(&tsbuf->cond_not_full, &tsbuf->mutex, &ts);
        if (ret == ETIMEDOUT) {
            pthread_mutex_unlock(&tsbuf->mutex);
            return 0; /* Timeout */
        }
    }

    size_t written = cbuf_write(&tsbuf->cbuf, data, len);

    if (written > 0) {
        pthread_cond_signal(&tsbuf->cond_not_empty);
    }

    pthread_mutex_unlock(&tsbuf->mutex);
    return written;
}

/**
 * Read with timeout (blocking until data available or timeout)
 */
size_t ts_cbuf_read_timeout(ts_circular_buffer_t *tsbuf, unsigned char *data, size_t len, int timeout_ms)
{
    if (tsbuf == NULL || data == NULL) {
        return 0;
    }

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeout_ms / 1000;
    ts.tv_nsec += (timeout_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }

    pthread_mutex_lock(&tsbuf->mutex);

    /* Wait for data if buffer is empty */
    while (cbuf_available(&tsbuf->cbuf) == 0) {
        int ret = pthread_cond_timedwait(&tsbuf->cond_not_empty, &tsbuf->mutex, &ts);
        if (ret == ETIMEDOUT) {
            pthread_mutex_unlock(&tsbuf->mutex);
            return 0; /* Timeout */
        }
    }

    size_t read = cbuf_read(&tsbuf->cbuf, data, len);

    if (read > 0) {
        pthread_cond_signal(&tsbuf->cond_not_full);
    }

    pthread_mutex_unlock(&tsbuf->mutex);
    return read;
}

/**
 * Check if buffer is empty (thread-safe)
 */
bool ts_cbuf_is_empty(ts_circular_buffer_t *tsbuf)
{
    if (tsbuf == NULL) {
        return true;
    }

    pthread_mutex_lock(&tsbuf->mutex);
    bool empty = cbuf_is_empty(&tsbuf->cbuf);
    pthread_mutex_unlock(&tsbuf->mutex);

    return empty;
}

/**
 * Get available data (thread-safe)
 */
size_t ts_cbuf_available(ts_circular_buffer_t *tsbuf)
{
    if (tsbuf == NULL) {
        return 0;
    }

    pthread_mutex_lock(&tsbuf->mutex);
    size_t available = cbuf_available(&tsbuf->cbuf);
    pthread_mutex_unlock(&tsbuf->mutex);

    return available;
}
```

#### 1.3. `Makefile` 수정

**추가할 링크 플래그:**
```makefile
# Existing LDFLAGS
LDFLAGS := -pthread  # pthread 라이브러리 링크 추가
```

### 테스트 계획

1. **단위 테스트**: `ts_cbuf_*` 함수들의 정확성 검증
2. **멀티스레드 테스트**: 여러 스레드에서 동시 read/write
3. **스트레스 테스트**: 버퍼 full/empty 경계 조건

---

## Phase 2: Level 1 구현 (Serial/Modem Thread)

### 목표
Serial I/O와 Modem 처리를 전담하는 스레드 구현

### 작업 파일

#### 2.1. `include/bridge.h` 수정

**추가할 함수 선언:**
```c
/* Thread entry points */
void *serial_modem_thread_func(void *arg);
void *telnet_thread_func(void *arg);  /* Phase 3에서 구현 */
```

#### 2.2. `src/bridge.c` 구현

**2.2.1. Serial/Modem Thread 함수 구현**

```c
/**
 * Serial/Modem thread - Level 1
 * Handles:
 * - Serial I/O (reading from serial port)
 * - Modem command processing (AT commands)
 * - Serial → Telnet data buffering
 * - Telnet → Serial data transmission
 */
void *serial_modem_thread_func(void *arg)
{
    bridge_ctx_t *ctx = (bridge_ctx_t *)arg;
    unsigned char serial_buf[BUFFER_SIZE];
    unsigned char filtered_buf[BUFFER_SIZE];
    unsigned char telnet_buf[BUFFER_SIZE * 2];
    unsigned char tx_buf[BUFFER_SIZE];

    MB_LOG_INFO("[Thread 1] Serial/Modem thread started");

    while (ctx->thread_running) {
        /* Check if serial port is ready */
        if (!ctx->serial_ready) {
            usleep(100000);  /* 100ms */
            continue;
        }

        /* === Part 1: Serial → Telnet direction === */

        /* Read from serial port */
        ssize_t n = serial_read(&ctx->serial, serial_buf, sizeof(serial_buf));

        if (n < 0) {
            /* I/O error - handle disconnection */
            MB_LOG_ERROR("[Thread 1] Serial I/O error: %s", strerror(errno));
            serial_close(&ctx->serial);
            ctx->serial_ready = false;
            ctx->modem_ready = false;
            usleep(100000);
            continue;
        }

        if (n > 0) {
            /* Log data */
            datalog_write(&ctx->datalog, DATALOG_DIR_FROM_MODEM, serial_buf, n);

            /* Process through modem layer */
            pthread_mutex_lock(&ctx->modem_mutex);
            bool modem_online = modem_is_online(&ctx->modem);

            if (!modem_online) {
                /* COMMAND mode: process AT commands */
                modem_process_input(&ctx->modem, (char *)serial_buf, n);
                pthread_mutex_unlock(&ctx->modem_mutex);
            } else {
                /* ONLINE mode: forward data to telnet */
                ssize_t consumed = modem_process_input(&ctx->modem, (char *)serial_buf, n);
                pthread_mutex_unlock(&ctx->modem_mutex);

                if (consumed > 0 && telnet_is_connected(&ctx->telnet)) {
                    /* Filter ANSI sequences */
                    size_t filtered_len;
                    ansi_filter_modem_to_telnet(serial_buf, consumed,
                                                filtered_buf, sizeof(filtered_buf),
                                                &filtered_len, &ctx->ansi_filter_state);

                    if (filtered_len > 0) {
                        /* Prepare for telnet (escape IAC) */
                        size_t telnet_len;
                        telnet_prepare_output(&ctx->telnet, filtered_buf, filtered_len,
                                            telnet_buf, sizeof(telnet_buf), &telnet_len);

                        if (telnet_len > 0) {
                            /* Write to serial→telnet buffer */
                            size_t written = ts_cbuf_write(&ctx->serial_to_telnet_buf,
                                                          telnet_buf, telnet_len);
                            if (written > 0) {
                                ctx->bytes_serial_to_telnet += written;
                            }
                        }
                    }
                }
            }
        }

        /* === Part 2: Telnet → Serial direction === */

        /* Read from telnet→serial buffer */
        size_t tx_len = ts_cbuf_read(&ctx->telnet_to_serial_buf, tx_buf, sizeof(tx_buf));
        if (tx_len > 0) {
            /* Log data */
            datalog_write(&ctx->datalog, DATALOG_DIR_TO_MODEM, tx_buf, tx_len);

            /* Write to serial port */
            ssize_t sent = serial_write(&ctx->serial, tx_buf, tx_len);
            if (sent > 0) {
                ctx->bytes_telnet_to_serial += sent;
            }
        }

        /* Short sleep to avoid busy-waiting */
        usleep(1000);  /* 1ms */
    }

    MB_LOG_INFO("[Thread 1] Serial/Modem thread exiting");
    return NULL;
}
```

**2.2.2. `bridge_init()` 수정**

```c
void bridge_init(bridge_ctx_t *ctx, config_t *cfg)
{
    if (ctx == NULL) {
        return;
    }

    memset(ctx, 0, sizeof(bridge_ctx_t));

    ctx->config = cfg;
    ctx->state = STATE_IDLE;
    ctx->running = false;

    /* Initialize components */
    serial_init(&ctx->serial);
    telnet_init(&ctx->telnet);
    ctx->telnet.datalog = &ctx->datalog;

    /* Initialize thread-safe buffers */
    ts_cbuf_init(&ctx->serial_to_telnet_buf);
    ts_cbuf_init(&ctx->telnet_to_serial_buf);

    /* Initialize mutexes */
    pthread_mutex_init(&ctx->state_mutex, NULL);
    pthread_mutex_init(&ctx->modem_mutex, NULL);

    /* Initialize ANSI filter state */
    ctx->ansi_filter_state = ANSI_STATE_NORMAL;

    /* Initialize statistics */
    ctx->bytes_serial_to_telnet = 0;
    ctx->bytes_telnet_to_serial = 0;
    ctx->connection_start_time = 0;

    /* Initialize data logger */
    datalog_init(&ctx->datalog);

    MB_LOG_DEBUG("Bridge context initialized (multithread mode)");
}
```

**2.2.3. `bridge_start()` 수정**

```c
int bridge_start(bridge_ctx_t *ctx)
{
    if (ctx == NULL || ctx->config == NULL) {
        return ERROR_INVALID_ARG;
    }

    MB_LOG_INFO("Starting bridge (multithread mode)");

    /* Initialize retry state */
    ctx->serial_ready = false;
    ctx->modem_ready = false;
    ctx->last_serial_retry = 0;
    ctx->serial_retry_interval = 10;
    ctx->serial_retry_count = 0;

    /* Try to open serial port */
    int ret = serial_open(&ctx->serial, ctx->config->serial_port, ctx->config);
    if (ret == SUCCESS) {
        ctx->serial_ready = true;
        serial_flush(&ctx->serial, TCIOFLUSH);
        modem_init(&ctx->modem, &ctx->serial);
        ctx->modem_ready = true;
        MB_LOG_INFO("Serial port opened: %s", ctx->config->serial_port);
    } else {
        ctx->serial_ready = false;
        ctx->modem_ready = false;
        ctx->last_serial_retry = time(NULL);
        MB_LOG_WARNING("Serial port not available (will retry): %s",
                      ctx->config->serial_port);
    }

    /* Open data log if enabled */
    if (ctx->config->data_log_enabled) {
        int ret_log = datalog_open(&ctx->datalog, ctx->config->data_log_file);
        if (ret_log == SUCCESS) {
            datalog_session_start(&ctx->datalog);
            MB_LOG_INFO("Data logging enabled: %s", ctx->config->data_log_file);
        }
    }

    ctx->state = STATE_IDLE;
    ctx->running = true;
    ctx->thread_running = true;

    /* Start threads */
    pthread_create(&ctx->serial_thread, NULL, serial_modem_thread_func, ctx);
    /* telnet_thread will be created in Phase 3 */

    MB_LOG_INFO("Bridge started (threads running)");

    return SUCCESS;
}
```

**2.2.4. `bridge_stop()` 수정**

```c
int bridge_stop(bridge_ctx_t *ctx)
{
    if (ctx == NULL) {
        return ERROR_INVALID_ARG;
    }

    MB_LOG_INFO("Stopping bridge");

    /* Signal threads to stop */
    ctx->running = false;
    ctx->thread_running = false;

    /* Wake up any blocked threads */
    pthread_cond_broadcast(&ctx->serial_to_telnet_buf.cond_not_empty);
    pthread_cond_broadcast(&ctx->serial_to_telnet_buf.cond_not_full);
    pthread_cond_broadcast(&ctx->telnet_to_serial_buf.cond_not_empty);
    pthread_cond_broadcast(&ctx->telnet_to_serial_buf.cond_not_full);

    /* Wait for threads to exit */
    pthread_join(ctx->serial_thread, NULL);
    /* pthread_join(ctx->telnet_thread, NULL); - Phase 3 */

    /* Disconnect telnet if connected */
    if (telnet_is_connected(&ctx->telnet)) {
        telnet_disconnect(&ctx->telnet);
    }

    /* Hang up modem (if ready) */
    if (ctx->modem_ready && modem_is_online(&ctx->modem)) {
        modem_hangup(&ctx->modem);
    }

    /* Close serial port (if ready) */
    if (ctx->serial_ready) {
        serial_close(&ctx->serial);
    }

    /* Close data log */
    if (datalog_is_enabled(&ctx->datalog)) {
        datalog_close(&ctx->datalog);
    }

    /* Cleanup thread resources */
    pthread_mutex_destroy(&ctx->state_mutex);
    pthread_mutex_destroy(&ctx->modem_mutex);
    ts_cbuf_destroy(&ctx->serial_to_telnet_buf);
    ts_cbuf_destroy(&ctx->telnet_to_serial_buf);

    /* Print statistics */
    bridge_print_stats(ctx);

    MB_LOG_INFO("Bridge stopped");

    return SUCCESS;
}
```

**2.2.5. `bridge_run()` 수정 (Phase 2에서는 기존 유지, Phase 4에서 제거)**

Phase 2에서는 main loop를 단순화하여 스레드가 실행 중인지만 확인:

```c
int bridge_run(bridge_ctx_t *ctx)
{
    if (ctx == NULL) {
        return ERROR_INVALID_ARG;
    }

    if (!ctx->running) {
        return ERROR_GENERAL;
    }

    /* In multithread mode, main loop just sleeps */
    /* Threads handle all I/O */
    usleep(100000);  /* 100ms */

    return SUCCESS;
}
```

### 테스트 계획

1. **Serial I/O 테스트**: AT 명령 처리 확인
2. **Thread 생성/종료 테스트**: 정상 시작 및 종료
3. **버퍼링 테스트**: Serial → Telnet 방향 데이터 전송

---

## Phase 3: Level 2 구현 (Telnet Thread)

### 목표
Telnet I/O와 프로토콜 처리를 전담하는 스레드 구현

### 작업 파일

#### 3.1. `src/bridge.c` 구현

**3.1.1. Telnet Thread 함수 구현**

```c
/**
 * Telnet thread - Level 2
 * Handles:
 * - Telnet I/O (reading from telnet server)
 * - IAC protocol processing
 * - Telnet → Serial data buffering
 * - Serial → Telnet data transmission
 */
void *telnet_thread_func(void *arg)
{
    bridge_ctx_t *ctx = (bridge_ctx_t *)arg;
    unsigned char telnet_buf[BUFFER_SIZE];
    unsigned char processed_buf[BUFFER_SIZE];
    unsigned char output_buf[BUFFER_SIZE];
    unsigned char tx_buf[BUFFER_SIZE];

    MB_LOG_INFO("[Thread 2] Telnet thread started");

    while (ctx->thread_running) {
        /* Check if telnet is connected */
        if (!telnet_is_connected(&ctx->telnet)) {
            usleep(100000);  /* 100ms */
            continue;
        }

        /* === Part 1: Telnet → Serial direction === */

        /* Read from telnet */
        ssize_t n = telnet_recv(&ctx->telnet, telnet_buf, sizeof(telnet_buf));

        if (n < 0) {
            /* I/O error */
            MB_LOG_ERROR("[Thread 2] Telnet connection error");
            telnet_disconnect(&ctx->telnet);

            /* Notify serial thread to hang up modem */
            pthread_mutex_lock(&ctx->modem_mutex);
            if (modem_is_online(&ctx->modem)) {
                modem_hangup(&ctx->modem);
                modem_send_no_carrier(&ctx->modem);
            }
            pthread_mutex_unlock(&ctx->modem_mutex);

            pthread_mutex_lock(&ctx->state_mutex);
            ctx->state = STATE_IDLE;
            pthread_mutex_unlock(&ctx->state_mutex);

            continue;
        }

        if (n == 0) {
            /* Check if connection closed */
            if (!telnet_is_connected(&ctx->telnet)) {
                MB_LOG_INFO("[Thread 2] Telnet disconnected");
                telnet_disconnect(&ctx->telnet);

                pthread_mutex_lock(&ctx->modem_mutex);
                if (modem_is_online(&ctx->modem)) {
                    modem_hangup(&ctx->modem);
                    modem_send_no_carrier(&ctx->modem);
                }
                pthread_mutex_unlock(&ctx->modem_mutex);

                pthread_mutex_lock(&ctx->state_mutex);
                ctx->state = STATE_IDLE;
                pthread_mutex_unlock(&ctx->state_mutex);
            }
            usleep(1000);
            continue;
        }

        if (n > 0) {
            /* Log data from telnet */
            datalog_write(&ctx->datalog, DATALOG_DIR_FROM_TELNET, telnet_buf, n);

            /* Process telnet protocol (remove IAC) */
            size_t processed_len;
            telnet_process_input(&ctx->telnet, telnet_buf, n,
                                processed_buf, sizeof(processed_buf), &processed_len);

            if (processed_len > 0) {
                /* Pass through ANSI sequences to modem client */
                size_t output_len;
                ansi_passthrough_telnet_to_modem(processed_buf, processed_len,
                                                output_buf, sizeof(output_buf), &output_len);

                if (output_len > 0) {
                    /* Write to telnet→serial buffer */
                    size_t written = ts_cbuf_write(&ctx->telnet_to_serial_buf,
                                                  output_buf, output_len);
                    /* Stats tracked in serial thread when actually sent */
                }
            }
        }

        /* === Part 2: Serial → Telnet direction === */

        /* Read from serial→telnet buffer */
        size_t tx_len = ts_cbuf_read(&ctx->serial_to_telnet_buf, tx_buf, sizeof(tx_buf));
        if (tx_len > 0) {
            /* Log data to telnet */
            datalog_write(&ctx->datalog, DATALOG_DIR_TO_TELNET, tx_buf, tx_len);

            /* Send to telnet server */
            ssize_t sent = telnet_send(&ctx->telnet, tx_buf, tx_len);
            /* Stats already tracked in serial thread */
        }

        /* Short sleep to avoid busy-waiting */
        usleep(1000);  /* 1ms */
    }

    MB_LOG_INFO("[Thread 2] Telnet thread exiting");
    return NULL;
}
```

**3.1.2. `bridge_start()` 수정 - Telnet 스레드 시작 추가**

```c
/* Start threads */
pthread_create(&ctx->serial_thread, NULL, serial_modem_thread_func, ctx);
pthread_create(&ctx->telnet_thread, NULL, telnet_thread_func, ctx);  /* 추가 */
```

**3.1.3. `bridge_stop()` 수정 - Telnet 스레드 종료 대기 추가**

```c
/* Wait for threads to exit */
pthread_join(ctx->serial_thread, NULL);
pthread_join(ctx->telnet_thread, NULL);  /* 추가 */
```

### 테스트 계획

1. **Telnet 연결 테스트**: telnet server 연결 확인
2. **IAC 처리 테스트**: option negotiation 동작 확인
3. **양방향 데이터 전송 테스트**: Serial ↔ Telnet 데이터 흐름

---

## Phase 4: Level 3 통합 및 테스트

### 목표
전체 시스템 통합 및 멀티스레드 안정성 검증

### 작업 내역

#### 4.1. 연결 흐름 구현

**`bridge_handle_modem_connect()` 수정 - Thread-safe 버전**

```c
int bridge_handle_modem_connect(bridge_ctx_t *ctx)
{
    if (ctx == NULL) {
        return ERROR_INVALID_ARG;
    }

    MB_LOG_INFO("=== Hardware modem CONNECT received - starting telnet connection ===");

    /* Connect to telnet server */
    int ret = telnet_connect(&ctx->telnet, ctx->config->telnet_host, ctx->config->telnet_port);
    if (ret != SUCCESS) {
        MB_LOG_ERROR("Failed to connect to telnet server");

        pthread_mutex_lock(&ctx->modem_mutex);
        modem_hangup(&ctx->modem);
        modem_send_no_carrier(&ctx->modem);
        pthread_mutex_unlock(&ctx->modem_mutex);

        pthread_mutex_lock(&ctx->state_mutex);
        ctx->state = STATE_IDLE;
        pthread_mutex_unlock(&ctx->state_mutex);

        return ret;
    }

    MB_LOG_INFO("Telnet connection established");

    pthread_mutex_lock(&ctx->modem_mutex);
    if (!modem_is_online(&ctx->modem)) {
        modem_go_online(&ctx->modem);
    }
    modem_send_connect(&ctx->modem, ctx->config->baudrate_value);
    pthread_mutex_unlock(&ctx->modem_mutex);

    pthread_mutex_lock(&ctx->state_mutex);
    ctx->state = STATE_CONNECTED;
    ctx->connection_start_time = time(NULL);
    pthread_mutex_unlock(&ctx->state_mutex);

    MB_LOG_INFO("=== Bridge connection FULLY established ===");

    return SUCCESS;
}
```

#### 4.2. 기존 select() 코드 제거

**`bridge_run()` 최종 버전 (멀티스레드 전용)**

```c
int bridge_run(bridge_ctx_t *ctx)
{
    if (ctx == NULL) {
        return ERROR_INVALID_ARG;
    }

    if (!ctx->running) {
        return ERROR_GENERAL;
    }

    /* In multithread mode, main loop just sleeps and lets threads work */
    usleep(100000);  /* 100ms */

    return SUCCESS;
}
```

기존 select() 관련 코드는 모두 제거 (또는 `#ifdef SINGLE_THREAD` 블록으로 이동)

### 테스트 계획

#### 4.2.1. 기능 테스트
- [ ] AT 명령 처리 (ATE, ATZ, ATDT, etc.)
- [ ] 모뎀 연결 및 해제 (CONNECT, NO CARRIER)
- [ ] Telnet 연결 및 IAC negotiation
- [ ] 양방향 데이터 전송
- [ ] ANSI 필터링 (modem → telnet)
- [ ] UTF-8 multibyte 문자 처리

#### 4.2.2. 멀티스레드 안정성 테스트
- [ ] 데드락 검출 (Valgrind helgrind)
- [ ] 레이스 컨디션 검출 (ThreadSanitizer)
- [ ] 메모리 누수 검사 (Valgrind memcheck)
- [ ] 스트레스 테스트 (장시간 실행)

#### 4.2.3. 성능 테스트
- [ ] Latency 측정 (serial → telnet, telnet → serial)
- [ ] Throughput 측정 (대용량 데이터 전송)
- [ ] CPU 사용률 측정
- [ ] Context switching 측정

---

## Phase 5: 정리 및 최적화

### 목표
코드 정리, 문서화, 최적화

### 작업 내역

#### 5.1. 코드 정리
- [ ] 사용하지 않는 함수 제거 (`bridge_process_serial_data()`, `bridge_process_telnet_data()` 등)
- [ ] 주석 업데이트 (멀티스레드 동작 설명)
- [ ] 일관된 에러 처리 및 로깅

#### 5.2. 문서 업데이트
- [ ] `README.md` 업데이트 (멀티스레드 아키텍처 설명)
- [ ] `CLAUDE.md` 업데이트 (새로운 아키텍처 가이드)
- [ ] 주석 업데이트

#### 5.3. 최적화
- [ ] Critical section 최소화 (mutex lock 시간 단축)
- [ ] Condition variable 활용 (busy-waiting 제거)
- [ ] Buffer 크기 튜닝
- [ ] Thread affinity 설정 (선택적)

---

## 구현 체크리스트

### Phase 1: Thread-Safe Buffer
- [ ] `ts_circular_buffer_t` 구조체 정의
- [ ] `ts_cbuf_init()` 구현
- [ ] `ts_cbuf_destroy()` 구현
- [ ] `ts_cbuf_write()` 구현
- [ ] `ts_cbuf_read()` 구현
- [ ] `ts_cbuf_write_timeout()` 구현
- [ ] `ts_cbuf_read_timeout()` 구현
- [ ] `ts_cbuf_is_empty()` 구현
- [ ] `ts_cbuf_available()` 구현
- [ ] Makefile에 `-pthread` 플래그 추가
- [ ] 단위 테스트 작성 및 실행

### Phase 2: Serial/Modem Thread
- [ ] `serial_modem_thread_func()` 구현
- [ ] `bridge_init()` 수정 (mutex 초기화)
- [ ] `bridge_start()` 수정 (스레드 생성)
- [ ] `bridge_stop()` 수정 (스레드 종료)
- [ ] `bridge_run()` 수정 (단순화)
- [ ] Serial I/O 테스트
- [ ] AT 명령 처리 테스트

### Phase 3: Telnet Thread
- [ ] `telnet_thread_func()` 구현
- [ ] `bridge_start()`에 telnet 스레드 생성 추가
- [ ] `bridge_stop()`에 telnet 스레드 종료 추가
- [ ] Telnet 연결 테스트
- [ ] IAC 처리 테스트
- [ ] 양방향 데이터 전송 테스트

### Phase 4: 통합 및 테스트
- [ ] `bridge_handle_modem_connect()` thread-safe 버전
- [ ] 기존 select() 코드 제거
- [ ] 기능 테스트 전체 실행
- [ ] 멀티스레드 안정성 테스트
- [ ] 성능 측정

### Phase 5: 정리 및 최적화
- [ ] 불필요한 함수 제거
- [ ] 주석 업데이트
- [ ] 문서 업데이트
- [ ] 최적화 적용

---

## 위험 요소 및 대응 방안

| 위험 요소 | 가능성 | 영향 | 대응 방안 |
|----------|--------|------|----------|
| 데드락 | 중간 | 높음 | Lock ordering rule 준수, timeout 사용 |
| 레이스 컨디션 | 높음 | 높음 | 모든 공유 자원에 mutex 적용, ThreadSanitizer 사용 |
| 성능 저하 | 낮음 | 중간 | 프로파일링, critical section 최소화 |
| 메모리 누수 | 낮음 | 중간 | Valgrind 정기 실행, 명확한 cleanup 로직 |
| 기존 기능 회귀 | 중간 | 높음 | 각 Phase마다 전체 기능 테스트 |

---

**작성일**: 2025-10-15
**버전**: 1.0
**상태**: 계획 완료, 구현 시작 준비 완료
