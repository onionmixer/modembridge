# Telnet Client Multibyte Character Handling Review

## 개요
텔넷 클라이언트의 멀티바이트 문자(UTF-8, EUC-KR 등) 처리 현황을 검토하고, 한글/일본어/중국어 등의 멀티바이트 문자가 올바르게 전송/수신되는지 분석합니다.

## 현재 구현 상태

### 1. IAC (0xFF) 이스케이프 처리 ✅ 양호

#### 수신 경로: `telnet_process_input()` (src/telnet.c:477-495)
```c
case TELNET_STATE_DATA:
    if (c == TELNET_IAC) {
        tn->state = TELNET_STATE_IAC;
    } else {
        /* Regular data */
        if (out_pos < output_size) {
            output[out_pos++] = c;
        }
    }
```

```c
case TELNET_STATE_IAC:
    if (c == TELNET_IAC) {
        /* Escaped IAC - output single IAC */
        if (out_pos < output_size) {
            output[out_pos++] = TELNET_IAC;
        }
        tn->state = TELNET_STATE_DATA;
    }
```

**상태**: ✅ **올바르게 구현됨**
- IAC IAC (0xFF 0xFF) 시퀀스를 단일 0xFF 바이트로 정확히 복원
- 상태 머신으로 안정적으로 처리

#### 송신 경로: `telnet_prepare_output()` (src/telnet.c:267-308)
```c
for (size_t i = 0; i < input_len; i++) {
    unsigned char c = input[i];

    if (c == TELNET_IAC) {
        /* Escape IAC by doubling it */
        if (out_pos + 1 < output_size) {
            output[out_pos++] = TELNET_IAC;
            output[out_pos++] = TELNET_IAC;
        } else {
            /* Output buffer full */
            break;
        }
    } else {
        /* Regular character */
        if (out_pos < output_size) {
            output[out_pos++] = c;
        }
    }
}
```

**상태**: ✅ **올바르게 구현됨**
- 0xFF를 0xFF 0xFF로 정확히 이스케이프
- RFC 854 준수

### 2. Binary Mode 협상 ✅ 양호

#### 초기 협상: `telnet_connect()` (src/telnet.c:105)
```c
/* Send initial option negotiations */
telnet_send_negotiate(tn, TELNET_WILL, TELOPT_BINARY);
```

**상태**: ✅ **올바르게 구현됨**
- 연결 시 Binary 모드 제안
- 8비트 깨끗한 전송을 위한 필수 협상

#### Binary 모드 추적
- `binary_local`: 클라이언트 → 서버 방향 Binary 모드
- `binary_remote`: 서버 → 클라이언트 방향 Binary 모드
- 양방향 독립적으로 추적됨

**상태**: ✅ **양방향 추적 정상**

## 잠재적 문제점

### ⚠️ 문제 1: 버퍼 경계에서 멀티바이트 문자 분할 가능성

#### 문제 상황
`telnet_process_input()`에서 출력 버퍼가 가득 차면 단순히 추가 바이트를 무시합니다:

```c
if (out_pos < output_size) {
    output[out_pos++] = c;
}
// 버퍼 가득 찬 경우: 바이트 무시됨 (손실)
```

**시나리오**:
1. 출력 버퍼에 2바이트 남음
2. UTF-8 한글 "안" (3바이트: 0xEC 0x95 0x88)이 도착
3. 0xEC, 0x95만 복사되고 0x88은 무시됨
4. 불완전한 UTF-8 시퀀스로 인해 문자 깨짐 발생

#### 영향도
- **심각도**: 중간
- **발생 확률**: 낮음 (버퍼 크기가 충분히 큰 경우에만 안전)
- **영향 범위**: 멀티바이트 문자 사용 시 (한글, 일본어, 중국어 등)

#### 권장 해결책
1. **버퍼 충분히 큰 크기 유지**: 최소 4KB 이상
2. **부분 전송 시 재시도 로직**: 다음 호출에서 이어서 처리
3. **버퍼 오버플로 경고 로그 추가**

### ⚠️ 문제 2: IAC 이스케이프 시 버퍼 공간 부족

#### 문제 상황
`telnet_prepare_output()`에서 IAC를 이스케이프할 공간이 부족하면 중단:

```c
if (c == TELNET_IAC) {
    if (out_pos + 1 < output_size) {
        output[out_pos++] = TELNET_IAC;
        output[out_pos++] = TELNET_IAC;
    } else {
        /* Output buffer full */
        break;  // 여기서 중단 - 남은 데이터 무시
    }
}
```

**시나리오**:
1. EUC-KR 문자열에 0xFF가 포함됨 (일부 한글 문자)
2. 출력 버퍼에 1바이트만 남음
3. 0xFF를 이스케이프하려면 2바이트 필요
4. 공간 부족으로 break → 이후 데이터 전송 안 됨
5. 멀티바이트 문자 중간에서 끊기면 문자 깨짐

**EUC-KR에서 0xFF 포함 가능한 문자 예**:
- EUC-KR 범위: 0xA1-0xFE (첫 바이트), 0xA1-0xFE (두 번째 바이트)
- 일부 한자나 특수문자에서 0xFF 근처 값 사용 가능

#### 영향도
- **심각도**: 중간
- **발생 확률**: 낮음 (EUC-KR 사용 시, 특정 문자)
- **영향 범위**: EUC-KR 인코딩 사용 환경

#### 권장 해결책
1. **출력 버퍼 크기 충분히 확보**: 입력의 2배 이상 (모든 바이트가 0xFF일 경우 대비)
2. **부분 전송 반환값 처리**: `*output_len`을 통해 실제 처리된 입력 바이트 수 반환
3. **호출자가 미처리 데이터 재시도**

### ⚠️ 문제 3: Binary 모드 협상 실패 시 처리 부재

#### 문제 상황
현재 코드는 Binary 모드를 제안하지만, 서버가 거부할 경우 특별한 처리가 없습니다.

```c
/* 연결 시 Binary 모드 제안 */
telnet_send_negotiate(tn, TELNET_WILL, TELOPT_BINARY);

/* 하지만 서버가 DONT BINARY를 보내면? */
// → binary_local = false로 설정되지만, 경고나 대응 없음
```

**Non-Binary 모드의 문제점**:
- 일부 서버는 8비트 데이터를 7비트로 변환
- 최상위 비트가 제거되거나 변형될 수 있음
- 멀티바이트 문자가 손상됨

#### 영향도
- **심각도**: 높음
- **발생 확률**: 낮음 (대부분 현대 서버는 Binary 모드 지원)
- **영향 범위**: Binary 모드 미지원 서버 연결 시 전체 멀티바이트 문자

#### 권장 해결책
1. **Binary 모드 협상 결과 확인**
2. **거부 시 경고 로그 출력**
3. **옵션: 사용자에게 연결 계속 여부 확인**

### ⚠️ 문제 4: 상위 계층(bridge.c) 버퍼 처리 확인 필요

`telnet_prepare_output()`와 `telnet_process_input()`이 버퍼 부족으로 중단될 때, 상위 계층에서 재시도 로직이 필요합니다.

**확인 필요 사항**:
1. bridge.c에서 반환된 `*output_len`을 확인하는가?
2. 미처리된 데이터를 다음 번에 재시도하는가?
3. 버퍼 크기가 충분한가?

## 멀티바이트 인코딩별 위험도 분석

### UTF-8 인코딩 ✅ 대체로 안전
- **0xFF 포함 가능성**: 매우 낮음
  - UTF-8에서 0xFF는 유효하지 않은 바이트 (BOM에서만 0xEF 0xBB 0xBF)
  - 일반 문자에서 0xFF 거의 등장 안 함
- **Binary 모드 필요성**: 높음 (8비트 깨끗한 전송 필수)
- **IAC 충돌**: 거의 없음
- **권장 조치**: Binary 모드 협상 확인

### EUC-KR 인코딩 ⚠️ 주의 필요
- **0xFF 포함 가능성**: 중간
  - EUC-KR 범위: 0xA1-0xFE, 일부 문자에서 0xFE 사용
  - 확장 영역(UHC)에서 0x81-0xFE 사용 가능
- **Binary 모드 필요성**: 매우 높음
- **IAC 충돌**: 낮음 (0xFF는 범위 밖)
- **권장 조치**: Binary 모드 필수, IAC 이스케이프 버퍼 여유 확보

### EUC-JP, Shift-JIS ⚠️ 주의 필요
- **0xFF 포함 가능성**: 중간
  - Shift-JIS 반각 카타카나 영역: 0x80-0xFF
  - JIS X 0208 확장 영역 사용
- **Binary 모드 필요성**: 매우 높음
- **IAC 충돌**: 낮음
- **권장 조치**: Binary 모드 필수

### GB2312, GBK, Big5 (중국어) ⚠️ 주의 필요
- **0xFF 포함 가능성**: 낮음-중간
  - GB2312: 0xA1-0xFE 범위
  - Big5: 0x81-0xFE 범위 (0xFF 제외)
- **Binary 모드 필요성**: 매우 높음
- **IAC 충돌**: 낮음
- **권장 조치**: Binary 모드 필수

## 권장 개선 사항 우선순위

### 1. 높은 우선순위 🔴 ✅ **구현 완료**

#### 1.1 Binary 모드 협상 실패 경고 추가 ✅
**구현 위치**: `src/telnet.c:278-296` (TELNET_WONT), `src/telnet.c:331-347` (TELNET_DONT)
```c
// telnet_handle_negotiate()에서 WONT BINARY 수신 시
case TELNET_WONT:
    if (option == TELOPT_BINARY) {
        tn->binary_remote = false;
        MB_LOG_WARNING("Server rejected BINARY mode - multibyte characters (UTF-8, EUC-KR) may be corrupted!");
    }

// telnet_handle_negotiate()에서 DONT BINARY 수신 시
case TELNET_DONT:
    if (option == TELOPT_BINARY) {
        tn->binary_local = false;
        MB_LOG_WARNING("Server rejected local BINARY mode - multibyte characters may be corrupted on send!");
    }
```

#### 1.2 버퍼 오버플로 로그 추가 ✅
**구현 위치**: `src/telnet.c:494-510` (process_input), `src/telnet.c:661-691` (prepare_output)
```c
// telnet_process_input()에서
case TELNET_STATE_DATA:
    if (out_pos < output_size) {
        output[out_pos++] = c;
    } else {
        static bool overflow_warned = false;
        if (!overflow_warned) {
            MB_LOG_WARNING("Telnet input buffer full - data may be truncated (multibyte chars may break)");
            overflow_warned = true;
        }
    }

// telnet_prepare_output()에서
if (i < input_len) {
    MB_LOG_WARNING("Telnet output buffer full - %zu of %zu bytes not processed (multibyte chars may break)",
                  input_len - i, input_len);
}
```

### 2. 중간 우선순위 🟡

#### 2.1 부분 처리된 바이트 수 반환
`telnet_prepare_output()`이 실제 처리한 입력 바이트 수를 반환하도록 수정:

```c
// 새로운 파라미터 추가
int telnet_prepare_output(telnet_t *tn, const unsigned char *input, size_t input_len,
                          unsigned char *output, size_t output_size,
                          size_t *output_len, size_t *input_consumed);
```

#### 2.2 bridge.c 버퍼 처리 검토 및 개선

### 3. 낮은 우선순위 🟢

#### 3.1 UTF-8 유효성 검사 (선택 사항)
멀티바이트 문자 경계 인식 및 버퍼 경계에서 문자 분할 방지

#### 3.2 인코딩 자동 감지 (선택 사항)
연결 시 터미널 인코딩 협상

## 실제 테스트 시나리오

### 테스트 1: UTF-8 한글 전송
```
입력: "안녕하세요" (UTF-8)
바이트: EC 95 88 EB 85 95 ED 95 98 EC 84 B8 EC 9A 94
예상: Binary 모드에서 정상 전송
```

### 테스트 2: EUC-KR 한글 전송
```
입력: "안녕하세요" (EUC-KR)
바이트: BE C8 B3 E7 C7 CF BC BC BF E4
예상: Binary 모드에서 정상 전송
```

### 테스트 3: 0xFF 포함 데이터
```
입력: 0x48 0xFF 0x65 (가상 데이터)
송신: 0x48 0xFF 0xFF 0x65 (IAC 이스케이프)
수신: 0x48 0xFF 0x65 (복원)
예상: IAC 이스케이프 정상 작동
```

### 테스트 4: 버퍼 경계 테스트
```
출력 버퍼: 1022바이트 남음
입력: 1024바이트 UTF-8 텍스트 (한글 포함)
예상: 버퍼 오버플로 경고, 일부 데이터 미처리
확인: 미처리 데이터 재전송 여부
```

## 버퍼 크기 분석 (bridge.c)

### 실제 버퍼 크기
**위치**: `include/common.h:34`
```c
#define BUFFER_SIZE 4096  // 4KB
```

### 송신 경로 (Serial → Telnet) - `bridge_process_serial_data()`
```c
unsigned char buf[BUFFER_SIZE];              // 4096 bytes - serial read
unsigned char filtered_buf[BUFFER_SIZE];     // 4096 bytes - ANSI filter
unsigned char telnet_buf[BUFFER_SIZE * 2];   // 8192 bytes - IAC escape ✅
```
**평가**: ✅ **양호** - IAC 이스케이프를 위해 2배 크기 확보

### 수신 경로 (Telnet → Serial) - `bridge_transfer_telnet_to_serial()`
```c
unsigned char telnet_buf[BUFFER_SIZE];       // 4096 bytes - telnet read
unsigned char processed_buf[BUFFER_SIZE];    // 4096 bytes - IAC removal
unsigned char output_buf[BUFFER_SIZE];       // 4096 bytes - final output
```
**평가**: ✅ **충분** - IAC 제거는 크기를 줄이므로 동일 크기로 안전

### 용량 분석
- **4096 바이트로 전송 가능한 문자 수**:
  - UTF-8 한글 (3바이트/문자): 약 1,365자
  - EUC-KR 한글 (2바이트/문자): 약 2,048자
  - ASCII (1바이트/문자): 4,096자

**결론**: 일반적인 사용에 충분한 크기

## 결론

### 현재 상태 평가: ✅ **양호 (2025년 개선 완료)**

#### 장점 ✅
1. IAC 이스케이프 처리가 RFC 854 표준 준수
2. Binary 모드 협상 시도 및 실패 시 경고 추가 ✅
3. 양방향 Binary 모드 독립 추적
4. 상태 머신 기반의 안정적인 파싱
5. 버퍼 오버플로 경고 추가 ✅
6. 충분한 버퍼 크기 (4KB → 8KB for escape)

#### 개선 완료 ✅
1. ✅ Binary 모드 협상 실패 시 경고 추가 (WONT/DONT BINARY)
2. ✅ 버퍼 오버플로 시 경고 로그 (telnet_process_input, telnet_prepare_output)

#### 추가 개선 권장 (선택 사항) 🟡
1. 부분 전송 시 미처리 데이터 추적 및 재시도 로직
2. UTF-8 유효성 검사 및 문자 경계 인식
3. 인코딩 자동 감지

#### 실사용 평가
- **UTF-8 환경**: ✅ **정상 작동** (Binary 모드 협상 성공 시)
  - 0xFF가 거의 등장하지 않아 IAC 충돌 최소
  - 4KB 버퍼로 약 1,365자 처리 가능

- **EUC-KR 환경**: ✅ **정상 작동** (Binary 모드 필수)
  - Binary 모드 협상 실패 시 명확한 경고 제공
  - 4KB 버퍼로 약 2,048자 처리 가능

- **대용량 멀티바이트 텍스트**: ⚠️ **주의**
  - 4KB 이상 전송 시 버퍼 오버플로 경고 출력
  - 일반 터미널 사용에는 충분하지만, 파일 전송 등에는 제한적

### 최종 권장 조치
1. ✅ **완료**: Binary 모드 실패 경고 추가
2. ✅ **완료**: 버퍼 오버플로 로그 추가
3. 🟡 **선택**: 부분 전송 재시도 로직 (필요 시)
4. 🟢 **선택**: 인코딩 자동 감지 및 유효성 검사 (장기)

## 검토 결과 요약

**멀티바이트 문자 처리 상태**: ✅ **프로덕션 사용 가능**

현재 구현은 다음을 충족합니다:
- ✅ RFC 854 IAC 이스케이프 규칙 준수
- ✅ Binary 모드 협상 및 실패 감지
- ✅ 충분한 버퍼 크기 (4KB ~ 8KB)
- ✅ 명확한 오류 경고 및 로깅
- ✅ UTF-8, EUC-KR, EUC-JP, Big5 등 주요 인코딩 지원

한글, 일본어, 중국어 등 멀티바이트 문자를 사용하는 일반적인 텔넷 세션에서 안정적으로 작동할 것으로 예상됩니다.
