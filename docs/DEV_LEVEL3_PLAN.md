# DEV_LEVEL3_PLAN - Level 3 개발 계획

## 개요
Level 3 파이프라인 관리 시스템을 단계적으로 구현하여 Level 1과 Level 2 사이의 효율적인 데이터 브리지를 구축합니다.

## 전제조건

### Level 1 강화 (필수)
- [x] DCD (Data Carrier Detect) 신호 처리
- [x] 하드웨어 흐름 제어 (RTS/CTS)
- [x] Hayes 필터 모듈 분리
- [x] 에러 복구 메커니즘

### Level 2 강화 (필수)
- [x] Telnet 옵션 협상 완성
- [x] IAC 명령 완전 처리
- [x] 바이너리 모드 지원
- [x] 트래픽 쉐이핑 준비

## Phase 1: 핵심 데이터 구조

### 목표
Level 3 시스템의 기본 데이터 구조 구현

### 구현 항목

#### 1.1 파이프라인 구조체
```c
// level3.h
typedef struct {
    circular_buffer_t buffer;      // 4096 bytes
    size_t bytes_processed;        // 통계
    size_t bytes_dropped;          // 오버플로우 카운트
    uint64_t last_activity_ms;     // 마지막 활동 시간
    bool is_active;                // 활성 상태
    pipeline_direction_t direction; // SERIAL_TO_TELNET or TELNET_TO_SERIAL
} pipeline_t;

typedef struct {
    pipeline_t pipeline1;          // Serial → Telnet
    pipeline_t pipeline2;          // Telnet → Serial
    level3_state_t state;          // 현재 상태
    pthread_mutex_t state_mutex;   // 상태 보호
    level3_config_t config;       // 구성
    level3_stats_t stats;         // 통계
} level3_t;
```

#### 1.2 워터마크 시스템
```c
// level3.c
typedef struct {
    size_t critical;  // 95%
    size_t high;      // 80%
    size_t low;       // 20%
    size_t empty;     // 5%
} watermark_levels_t;

bool check_watermark(pipeline_t *pipeline, watermark_level_t level) {
    size_t usage = cbuf_used(&pipeline->buffer);
    size_t capacity = cbuf_capacity(&pipeline->buffer);
    size_t threshold = (capacity * watermark_percentage[level]) / 100;
    return usage >= threshold;
}
```

### 테스트
- 버퍼 경계 조건 테스트
- 워터마크 트리거 확인
- 멀티스레드 접근 테스트

## Phase 2: 상태 기계 구현

### 목표
10개 상태와 전이 규칙 구현

### 구현 항목

#### 2.1 상태 전이 함수
```c
// level3.c
int level3_transition(level3_t *ctx, level3_state_t new_state) {
    pthread_mutex_lock(&ctx->state_mutex);

    // 유효한 전이인지 확인
    if (!is_valid_transition(ctx->state, new_state)) {
        MB_LOG_ERROR("Invalid transition: %s -> %s",
                     state_to_string(ctx->state),
                     state_to_string(new_state));
        pthread_mutex_unlock(&ctx->state_mutex);
        return ERROR_INVALID_STATE;
    }

    // 상태별 정리 작업
    state_cleanup(ctx, ctx->state);

    // 상태 변경
    level3_state_t old_state = ctx->state;
    ctx->state = new_state;

    // 상태별 초기화 작업
    state_initialize(ctx, new_state);

    MB_LOG_INFO("State transition: %s -> %s",
                state_to_string(old_state),
                state_to_string(new_state));

    pthread_mutex_unlock(&ctx->state_mutex);
    return SUCCESS;
}
```

#### 2.2 상태별 핸들러
```c
// level3.c
int level3_handle_state(level3_t *ctx) {
    switch (ctx->state) {
        case L3_STATE_READY:
            return handle_ready_state(ctx);
        case L3_STATE_CONNECTING:
            return handle_connecting_state(ctx);
        case L3_STATE_DATA_TRANSFER:
            return handle_data_transfer_state(ctx);
        case L3_STATE_FLUSHING:
            return handle_flushing_state(ctx);
        // ... 기타 상태
    }
}
```

### 테스트
- 모든 상태 도달 테스트
- 비정상 전이 차단 확인
- 상태 지속성 테스트

## Phase 3: 스케줄링 엔진

### 목표
Quantum 기반 공정 스케줄링 구현

### 구현 항목

#### 3.1 스케줄러 구조
```c
// level3.c
typedef struct {
    uint32_t quantum_ms;           // 기본 50ms
    uint32_t starvation_threshold; // 500ms
    uint64_t pipeline1_last_run;
    uint64_t pipeline2_last_run;
    pipeline_t *next_pipeline;     // 다음 실행할 파이프라인
} scheduler_t;

pipeline_t* schedule_next(level3_t *ctx) {
    scheduler_t *sched = &ctx->scheduler;
    uint64_t now = get_monotonic_ms();

    // Anti-starvation 체크
    if (now - sched->pipeline1_last_run > sched->starvation_threshold) {
        return &ctx->pipeline1;
    }
    if (now - sched->pipeline2_last_run > sched->starvation_threshold) {
        return &ctx->pipeline2;
    }

    // Round-robin
    if (sched->next_pipeline == &ctx->pipeline1) {
        sched->next_pipeline = &ctx->pipeline2;
        return &ctx->pipeline1;
    } else {
        sched->next_pipeline = &ctx->pipeline1;
        return &ctx->pipeline2;
    }
}
```

#### 3.2 Quantum 실행
```c
// level3.c
int execute_quantum(level3_t *ctx, pipeline_t *pipeline) {
    uint64_t start = get_monotonic_ms();
    uint64_t deadline = start + ctx->scheduler.quantum_ms;
    size_t bytes_processed = 0;

    while (get_monotonic_ms() < deadline) {
        // 데이터 처리
        int ret = process_pipeline_data(ctx, pipeline);
        if (ret == ERROR_NO_DATA) {
            break;  // 데이터 없음, quantum 조기 종료
        }
        if (ret < 0) {
            return ret;  // 에러
        }
        bytes_processed += ret;

        // 워터마크 체크
        if (check_watermark(pipeline, WATERMARK_CRITICAL)) {
            apply_backpressure(ctx, pipeline);
            break;
        }
    }

    // 통계 업데이트
    pipeline->bytes_processed += bytes_processed;
    pipeline->last_activity_ms = get_monotonic_ms();

    return SUCCESS;
}
```

### 테스트
- Quantum 시간 준수 확인
- Anti-starvation 동작 검증
- 공정성 측정 (50:50 분배)

## Phase 4: 필터링 모듈

### 목표
Hayes AT 명령 및 Telnet 제어 코드 필터링

### 구현 항목

#### 4.1 Hayes 필터 (Pipeline 1)
```c
// level3.c
int filter_hayes_commands(level3_t *ctx, uint8_t *data, size_t *len) {
    if (ctx->modem_state != MODEM_STATE_ONLINE) {
        // Command 모드: AT 명령 필터링
        for (size_t i = 0; i < *len; i++) {
            if (is_at_command_start(data, i, *len)) {
                // AT 명령 감지, Level 1로 전달
                size_t cmd_len = extract_at_command(data + i, *len - i);
                send_to_level1(ctx, data + i, cmd_len);

                // 데이터에서 제거
                memmove(data + i, data + i + cmd_len, *len - i - cmd_len);
                *len -= cmd_len;
                i--;  // 재검사
            }
        }
    }
    return SUCCESS;
}
```

#### 4.2 Telnet 필터 (Pipeline 2)
```c
// level3.c
int filter_telnet_control(level3_t *ctx, uint8_t *data, size_t *len) {
    size_t write_pos = 0;

    for (size_t i = 0; i < *len; i++) {
        if (data[i] == TELNET_IAC) {
            // IAC 시퀀스는 Level 2에서 처리되어야 함
            // 여기서는 통과시키지 않음
            continue;
        }
        data[write_pos++] = data[i];
    }

    *len = write_pos;
    return SUCCESS;
}
```

### 테스트
- AT 명령 정확한 필터링
- 이스케이프 시퀀스 처리
- Telnet IAC 차단 확인

## Phase 5: 통합 및 최적화

### 목표
전체 시스템 통합 및 성능 최적화

### 구현 항목

#### 5.1 메인 루프 통합
```c
// level3.c
int level3_run(level3_t *ctx) {
    while (ctx->state != L3_STATE_TERMINATED) {
        // 상태별 처리
        int ret = level3_handle_state(ctx);
        if (ret < 0 && ret != ERROR_NO_DATA) {
            MB_LOG_ERROR("State handler error: %d", ret);
            level3_transition(ctx, L3_STATE_ERROR);
        }

        // 데이터 전송 상태에서만 스케줄링
        if (ctx->state == L3_STATE_DATA_TRANSFER) {
            pipeline_t *pipe = schedule_next(ctx);
            execute_quantum(ctx, pipe);
        }

        // 통계 업데이트
        update_statistics(ctx);

        // CPU 양보
        if (ctx->state == L3_STATE_READY) {
            usleep(1000);  // 1ms sleep when idle
        }
    }

    return SUCCESS;
}
```

#### 5.2 성능 최적화
```c
// 최적화 기법들
- Zero-copy 버퍼 전달
- 배치 처리 (최소 64 바이트)
- 캐시 친화적 데이터 구조
- Branch prediction 힌트
- SIMD 명령어 활용 (옵션)
```

### 테스트
- 종단 간 데이터 전송
- 성능 벤치마크
- 장시간 안정성 테스트

## 테스트 시나리오

### 시나리오 1: 기본 동작
1. ModemBridge 시작
2. 설정 로드 확인
3. Level 3 초기화 확인
4. 상태 READY 확인

### 시나리오 2: 연결 수립
1. Serial 포트 열기
2. Telnet 연결
3. 상태 CONNECTING → NEGOTIATING → DATA_TRANSFER
4. 양방향 통신 확인

### 시나리오 3: 데이터 전송
1. ASCII 텍스트 전송 ("Hello World")
2. 멀티바이트 문자 전송 ("한글", "日本語")
3. 바이너리 데이터 전송
4. 대용량 파일 전송 (1MB)

### 시나리오 4: 부하 테스트
1. 최대 속도 전송 (115200 bps)
2. 버퍼 오버플로우 유도
3. 백프레셔 동작 확인
4. 복구 확인

### 시나리오 5: 에러 처리
1. 연결 끊김 시뮬레이션
2. 버퍼 가득 상황
3. 잘못된 데이터 주입
4. 자동 복구 확인

### 시나리오 6: 성능 측정
1. 지연시간 측정 (ping-pong)
2. 처리량 측정 (bulk transfer)
3. CPU 사용률 측정
4. 메모리 사용량 측정

### 시나리오 7: 장시간 테스트
1. 24시간 연속 운영
2. 100만 바이트 전송
3. 1000회 재연결
4. 메모리 누수 검사

### 시나리오 8: 구성 테스트
1. 저속 모드 (300 bps)
2. 표준 모드 (9600 bps)
3. 고속 모드 (115200 bps)
4. 채팅 서버 모드
5. 파일 전송 모드

## 구현 일정

| Phase | 기간 | 상태 | 우선순위 |
|-------|------|------|----------|
| Phase 1: 데이터 구조 | 3일 | ✅ | P0 |
| Phase 2: 상태 기계 | 4일 | ✅ | P0 |
| Phase 3: 스케줄링 | 5일 | ✅ | P0 |
| Phase 4: 필터링 | 3일 | ✅ | P1 |
| Phase 5: 통합 | 5일 | 🔄 | P1 |
| 테스트 | 5일 | ⏸️ | P2 |
| 문서화 | 2일 | ⏸️ | P3 |

## 위험 관리

| 위험 | 영향 | 확률 | 대응 방안 |
|------|------|------|----------|
| 버퍼 오버플로우 | 높음 | 중간 | 워터마크 시스템, 백프레셔 |
| 데드락 | 치명 | 낮음 | Timeout, 순서 보장 |
| 성능 저하 | 중간 | 중간 | 프로파일링, 최적화 |
| 메모리 누수 | 높음 | 낮음 | Valgrind, 정적 분석 |
| 상태 불일치 | 중간 | 중간 | 상태 검증, 로깅 |

## 검증 체크리스트

### 기능 검증
- [x] 파이프라인 생성/소멸
- [x] 상태 전이 정확성
- [x] 스케줄링 공정성
- [x] 필터링 정확성
- [ ] 에러 복구

### 성능 검증
- [x] 지연시간 < 100ms
- [x] 오버플로우 < 1%
- [ ] CPU < 10%
- [ ] 메모리 안정성

### 안정성 검증
- [ ] 24시간 테스트
- [ ] 100만 바이트
- [ ] 1000회 재연결
- [ ] 에러 주입 테스트

## 성공 기준

1. **기능**: 모든 요구사항 구현
2. **성능**: 목표 메트릭 달성
3. **안정성**: 장시간 무중단 운영
4. **품질**: 코드 커버리지 80%+
5. **문서**: 완전한 사용자 가이드

---
*최종 업데이트: 2025-10-23*