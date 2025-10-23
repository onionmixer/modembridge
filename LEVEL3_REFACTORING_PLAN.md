# Level3.c Refactoring Plan - Detailed Implementation Guide

## 목표 및 원칙
1. **기능 변경 없음**: 모든 기존 기능 100% 유지
2. **모듈화**: level3.c (3,693 lines) → 13개 하위 모듈로 분리
3. **테스트 가능성**: 각 단계마다 빌드 및 테스트 통과 보장
4. **단계적 접근**: 위험 최소화를 위한 5단계 점진적 리팩토링

## 현재 상태 분석
- **총 라인 수**: 3,693 lines
- **함수 개수**: 67개 (45 public, 22 static)
- **기능 영역**: 17개 섹션
- **복잡도**: 높음 (특히 scheduling 763 lines, buffer 699 lines)

## Phase별 상세 리팩토링 계획

### Phase 1: 기반 구조 분리 (위험도: 낮음)
**목표**: 핵심 구조체와 유틸리티 분리

#### 1.1 level3_types.h (새 파일)
```c
/* 모든 구조체 정의 이동 */
- l3_context_t
- l3_pipeline_t
- l3_state_t
- l3_stats_t
- l3_buffer_config_t
- 열거형 정의들
```

#### 1.2 level3_util.c (148 lines)
```c
/* 유틸리티 함수 이동 */
- l3_get_current_time_ms()
- l3_get_monotonic_ms()
- l3_format_throughput()
- l3_get_state_name()
- l3_get_direction_name()
```

#### 1.3 level3_core.c (기존 level3.c 유지)
```c
/* 메인 API 및 라이프사이클 */
- level3_init()
- level3_cleanup()
- level3_run()
- level3_shutdown()
```

**검증 방법**:
```bash
make clean && make
./tests/test_level3  # 기존 테스트 통과 확인
```

### Phase 2: 상태 관리 분리 (위험도: 중간)
**목표**: 상태 기계와 DCD 이벤트 처리 분리

#### 2.1 level3_state.c (413 lines)
```c
/* Enhanced State Machine Functions 섹션 이동 */
- level3_transition_to_state()
- l3_validate_state_transition()
- l3_handle_state_entry()
- l3_handle_state_exit()
- l3_get_state_name()

/* 상태 전이 매트릭스 */
- static const bool state_transition_matrix[10][10]
```

#### 2.2 level3_dcd.c (154 lines)
```c
/* DCD Event Bridge Functions 섹션 이동 */
- level3_handle_dcd_event()
- level3_on_dcd_high()
- level3_on_dcd_low()
- l3_handle_carrier_loss()
```

#### 2.3 level3_pipeline.c (132 lines)
```c
/* Pipeline Management 섹션 이동 */
- level3_process_pipelines()
- l3_process_single_pipeline()
- l3_update_pipeline_stats()
```

**인터페이스 헤더 추가**:
```c
/* level3_internal.h - 내부 모듈간 인터페이스 */
#ifndef LEVEL3_INTERNAL_H
#define LEVEL3_INTERNAL_H
#include "level3_types.h"

/* 내부 함수 프로토타입 */
int l3_validate_state_transition(l3_state_t from, l3_state_t to);
void l3_handle_state_entry(l3_context_t *ctx, l3_state_t state);
// ... 기타 내부 API
#endif
```

### Phase 3: 데이터 경로 분리 (위험도: 높음)
**목표**: 버퍼 관리와 프로토콜 필터링 분리

#### 3.1 level3_buffer.c (699 lines) - 가장 큰 모듈
```c
/* Enhanced Buffer Management 섹션 이동 */
- l3_enhanced_buffer_init()
- l3_enhanced_buffer_cleanup()
- l3_enhanced_buffer_write()
- l3_enhanced_buffer_read()
- l3_enhanced_buffer_peek()
- l3_memory_pool_init()
- l3_memory_pool_alloc()
- l3_memory_pool_free()

/* Double Buffer Management 섹션 이동 */
- level3_init_double_buffers()
- level3_cleanup_double_buffers()
- l3_switch_active_buffer()
- l3_get_inactive_buffer()
```

#### 3.2 level3_filter.c (490 lines)
```c
/* Protocol Filtering 섹션 이동 */
- level3_filter_hayes_commands()
- l3_is_hayes_command()
- l3_extract_hayes_command()
- level3_filter_telnet_iac()
- l3_detect_iac_sequence()

/* Multibyte Character Handling 섹션 이동 */
- l3_is_multibyte_start()
- l3_get_multibyte_length()
- l3_is_multibyte_complete()
- l3_handle_multibyte_boundary()

/* Hayes dictionary 이동 */
- static const hayes_dictionary_t hayes_dictionary = {...}
```

**테스트 추가**:
```c
/* tests/test_level3_buffer.c - 새 단위 테스트 */
void test_enhanced_buffer_operations();
void test_memory_pool();
void test_double_buffer_switching();
```

### Phase 4: 고급 기능 분리 (위험도: 높음)
**목표**: 스케줄링, 백프레셔, Half-duplex 제어 분리

#### 4.1 level3_schedule.c (763 lines) - 가장 복잡한 모듈
```c
/* Scheduling and Fairness 섹션 이동 */
- level3_init_scheduler()
- level3_schedule_next_pipeline()
- l3_enforce_fair_scheduling()
- l3_calculate_optimal_quantum()
- l3_update_fair_queue_weights()
- l3_is_direction_starving()
- l3_get_direction_wait_time()

/* Latency Bound Guarantee Functions 섹션 이동 */
- l3_enforce_latency_boundaries()
- l3_detect_latency_violation()
- l3_calculate_adaptive_quantum_with_latency()
- l3_update_direction_priorities()
- l3_should_force_direction_switch()
```

#### 4.2 level3_backpressure.c (154 lines)
```c
/* Backpressure Management 섹션 이동 */
- level3_apply_backpressure()
- level3_release_backpressure()
- l3_check_watermark_levels()
```

#### 4.3 level3_duplex.c (87 lines)
```c
/* Half-duplex Control 섹션 이동 */
- level3_enforce_half_duplex()
- l3_can_activate_pipeline()
```

### Phase 5: 운영 기능 분리 (위험도: 낮음)
**목표**: 모니터링과 스레드 관리 분리

#### 5.1 level3_monitor.c (109 lines)
```c
/* Statistics and Monitoring 섹션 이동 */
- level3_get_statistics()
- level3_log_performance_stats()
- l3_calculate_throughput()
```

#### 5.2 level3_thread.c (67 lines)
```c
/* Thread Functions 섹션 이동 */
- level3_thread_main()
- l3_thread_cleanup()
```

## 파일 구조 (리팩토링 후)

```
src/
├── level3.c (500 lines) - 메인 컨트롤러
├── level3_types.h - 공통 타입 정의
├── level3_internal.h - 내부 API
├── level3_core.c - 라이프사이클
├── level3_util.c - 유틸리티
├── level3_state.c - 상태 기계
├── level3_dcd.c - DCD 이벤트
├── level3_pipeline.c - 파이프라인
├── level3_buffer.c - 버퍼 관리
├── level3_filter.c - 프로토콜 필터
├── level3_schedule.c - 스케줄링
├── level3_backpressure.c - 백프레셔
├── level3_duplex.c - Half-duplex
├── level3_monitor.c - 모니터링
└── level3_thread.c - 스레드
```

## Makefile 수정

```makefile
LEVEL3_OBJS = \
    $(BUILD_DIR)/level3.o \
    $(BUILD_DIR)/level3_core.o \
    $(BUILD_DIR)/level3_util.o \
    $(BUILD_DIR)/level3_state.o \
    $(BUILD_DIR)/level3_dcd.o \
    $(BUILD_DIR)/level3_pipeline.o \
    $(BUILD_DIR)/level3_buffer.o \
    $(BUILD_DIR)/level3_filter.o \
    $(BUILD_DIR)/level3_schedule.o \
    $(BUILD_DIR)/level3_backpressure.o \
    $(BUILD_DIR)/level3_duplex.o \
    $(BUILD_DIR)/level3_monitor.o \
    $(BUILD_DIR)/level3_thread.o

$(BUILD_DIR)/modembridge: $(OBJS) $(LEVEL3_OBJS)
    $(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
```

## 검증 계획

### 각 Phase별 검증
1. **빌드 테스트**: `make clean && make`
2. **단위 테스트**: `./tests/test_level3`
3. **통합 테스트**: `./tests/test_level3_integration`
4. **메모리 검사**: `valgrind ./build/modembridge`
5. **성능 검증**: `./tests/benchmark_level3.sh`

### 회귀 테스트 체크리스트
- [ ] 모든 public API 동작 확인
- [ ] 상태 전이 정확성
- [ ] 버퍼 오버플로우 없음
- [ ] 스케줄링 공정성 유지
- [ ] 지연시간 < 100ms
- [ ] CPU 사용률 < 10%
- [ ] 메모리 누수 없음

## 위험 관리

### 높은 위험 영역
1. **스케줄링 로직** - 타이밍 민감, 철저한 테스트 필요
2. **버퍼 관리** - 멀티스레드 접근, 락 관리 주의
3. **프로토콜 필터** - 바이트 단위 정확성 필수

### 백업 계획
- 각 Phase 전 git branch 생성
- 원본 level3.c는 level3_original.c.bak으로 보관
- 문제 발생 시 즉시 롤백 가능

## 예상 결과

### 개선 효과
1. **유지보수성**: 단일 3,693 lines → 평균 284 lines × 13 파일
2. **테스트 용이성**: 모듈별 단위 테스트 가능
3. **컴파일 시간**: 변경된 모듈만 재컴파일
4. **코드 가독성**: 기능별 명확한 분리
5. **팀 협업**: 병렬 개발 가능

### 성능 영향
- **예상**: 변경 없음 (동일한 코드, 구조만 변경)
- **모니터링**: 각 Phase 후 벤치마크 실행

## 실행 일정

| Phase | 작업 내용 | 예상 시간 | 위험도 |
|-------|----------|-----------|--------|
| 1 | 기반 구조 | 4시간 | 낮음 |
| 2 | 상태 관리 | 6시간 | 중간 |
| 3 | 데이터 경로 | 10시간 | 높음 |
| 4 | 고급 기능 | 12시간 | 높음 |
| 5 | 운영 기능 | 3시간 | 낮음 |
| 검증 | 통합 테스트 | 5시간 | - |
| **합계** | | **40시간** | |

## 다음 단계

1. 이 계획 검토 및 승인
2. refactoring_level3 브랜치 생성
3. Phase 1부터 순차 실행
4. 각 Phase 완료 후 PR 생성 및 리뷰

---
*작성일: 2025-10-23*
*작성자: Claude Code Assistant*
*대상: level3.c v1.0.0*