# Modem-to-Telnet Data Path Multibyte Character Handling Review

## 개요
모뎀(시리얼 포트)에서 받은 데이터를 텔넷 연결로 전달하는 전체 경로를 검토하고, 멀티바이트 문자가 각 단계에서 안전하게 처리되는지 분석합니다.

## 전체 데이터 흐름

```
Serial Port
    ↓
serial_read() → buf[4096]
    ↓
modem_process_input() → consumed (bytes)
    ↓
ansi_filter_modem_to_telnet() → filtered_buf[4096]
    ↓
telnet_prepare_output() → telnet_buf[8192]
    ↓
telnet_send() → Telnet Server
```

## 단계별 상세 분석

### 1단계: Serial Port 읽기 (src/bridge.c:538)

**코드**:
```c
unsigned char buf[BUFFER_SIZE];  // 4096 bytes
ssize_t n = serial_read(&ctx->serial, buf, sizeof(buf));
```

**평가**: ✅ **안전**
- 바이트 단위 그대로 읽음
- 멀티바이트 문자 처리 필요 없음
- 버퍼 크기 충분 (4KB)

---

### 2단계: Modem 레이어 처리 (src/bridge.c:565)

**코드**:
```c
/* Online mode - check for escape sequence and forward data */
ssize_t consumed = modem_process_input(&ctx->modem, (char *)buf, n);
if (!modem_is_online(&ctx->modem)) {
    /* Modem went offline (escape sequence detected) */
    return SUCCESS;
}

if (consumed <= 0) {
    /* No data to transfer */
    return SUCCESS;
}
```

**평가**: ✅ **안전**
- `modem_process_input()`는 Online 모드에서 데이터를 투명하게 통과시킴
- 이스케이프 시퀀스 (+++) 감지는 멀티바이트 문자와 충돌하지 않음 (이미 검증 완료)
- `consumed` 반환값이 원본 데이터 길이를 정확히 반환

**중요**: `consumed` 값은 Online 모드에서 `len`과 동일 (src/modem.c:581)

---

### 3단계: ANSI Escape Sequence 필터링 (src/bridge.c:582-583)

**코드**:
```c
/* Filter ANSI sequences */
ansi_filter_modem_to_telnet(buf, consumed, filtered_buf, sizeof(filtered_buf),
                            &filtered_len, &ctx->ansi_filter_state);
```

#### 3.1 ANSI 필터 상태 머신 분석 (src/bridge.c:200-281)

**상태 머신**:
```c
for (size_t i = 0; i < input_len; i++) {
    unsigned char c = input[i];

    switch (current_state) {
        case ANSI_STATE_NORMAL:
            if (c == 0x1B) {  /* ESC */
                current_state = ANSI_STATE_ESC;
            } else {
                /* Normal character - pass through */
                if (out_pos < output_size) {
                    output[out_pos++] = c;
                }
            }
            break;

        case ANSI_STATE_ESC:
            if (c == '[') {
                /* CSI sequence */
                current_state = ANSI_STATE_CSI;
            } else if (c == 'c') {
                /* Reset - filter out */
                current_state = ANSI_STATE_NORMAL;
            } else {
                /* Other escape sequences - filter out for now */
                current_state = ANSI_STATE_NORMAL;
            }
            break;

        case ANSI_STATE_CSI:
            /* Check if this is a parameter or intermediate byte */
            if (c >= 0x30 && c <= 0x3F) {  // 0-9:;<=>?
                current_state = ANSI_STATE_CSI_PARAM;
            } else if (c >= 0x40 && c <= 0x7E) {  // @A-Z[\]^_`a-z{|}~
                /* Final byte - end of CSI sequence */
                current_state = ANSI_STATE_NORMAL;
            } else {
                /* Invalid - return to normal */
                current_state = ANSI_STATE_NORMAL;
            }
            break;

        case ANSI_STATE_CSI_PARAM:
            if (c >= 0x30 && c <= 0x3F) {
                /* More parameter bytes */
            } else if (c >= 0x40 && c <= 0x7E) {
                /* Final byte - end of CSI sequence */
                current_state = ANSI_STATE_NORMAL;
            } else {
                /* Invalid - return to normal */
                current_state = ANSI_STATE_NORMAL;
            }
            break;
    }
}
```

#### 🔴 **문제 1: ESC (0x1B) 바이트가 멀티바이트 문자 내부에 나타날 경우**

**위험도**: 중간-높음

**시나리오**:
1. UTF-8 멀티바이트 문자 중간에 0x1B가 포함됨
2. ANSI 필터가 0x1B를 ESC로 오인식
3. 다음 바이트들을 ANSI 시퀀스로 처리
4. 멀티바이트 문자 일부가 제거됨
5. 불완전한 UTF-8 시퀀스 전송

**발생 가능성 분석**:

##### UTF-8에서 0x1B 포함 가능성

**UTF-8 인코딩 구조**:
```
1바이트 (ASCII): 0xxxxxxx (0x00-0x7F)
2바이트: 110xxxxx 10xxxxxx (0xC0-0xDF, 0x80-0xBF)
3바이트: 1110xxxx 10xxxxxx 10xxxxxx (0xE0-0xEF, 0x80-0xBF, 0x80-0xBF)
4바이트: 11110xxx 10xxxxxx 10xxxxxx 10xxxxxx (0xF0-0xF7, 0x80-0xBF, ...)
```

**0x1B 분석**:
- 0x1B = 0b00011011
- ASCII 범위 (0x00-0x7F)
- **결론**: 0x1B는 UTF-8에서 단독 문자(ESC)로만 나타남
- **멀티바이트 문자의 일부로 0x1B 등장**: **불가능** ✅

##### EUC-KR에서 0x1B 포함 가능성

**EUC-KR 인코딩 구조**:
```
1바이트 (ASCII): 0x00-0x7F
2바이트: 0xA1-0xFE, 0xA1-0xFE
```

**0x1B 분석**:
- 0x1B는 ASCII 범위
- **결론**: 0x1B는 EUC-KR에서 단독 문자로만 나타남
- **멀티바이트 문자의 일부로 0x1B 등장**: **불가능** ✅

##### EUC-JP, Shift-JIS, Big5 분석

**공통점**:
- 0x1B는 모두 ASCII 범위로 정의됨
- 멀티바이트 문자의 일부로 사용되지 않음
- **결론**: 안전 ✅

#### ⚠️ **문제 2: ANSI 시퀀스 중간 바이트가 멀티바이트 문자로 오인될 가능성**

**위험도**: 낮음

**시나리오**:
```
입력: ESC [ ... (멀티바이트 문자) ... m
```

예를 들어:
```
ESC[31m안녕ESC[0m
```

이 경우:
1. ESC 감지 → ANSI_STATE_ESC
2. '[' 감지 → ANSI_STATE_CSI
3. '3', '1' 감지 → ANSI_STATE_CSI_PARAM
4. 'm' 감지 → ANSI_STATE_NORMAL, ANSI 시퀀스 제거
5. 0xEC, 0x95, 0x88 ("안") → 정상 통과
6. ESC 감지 → ...

**평가**: ✅ **안전**
- ANSI 시퀀스는 명확히 구분됨
- 멀티바이트 문자는 ANSI 시퀀스 외부에서만 처리됨

#### ⚠️ **문제 3: 버퍼 경계에서 ANSI 시퀀스 중간에 끊김**

**위험도**: 낮음-중간

**시나리오**:
1. 4096 바이트 버퍼가 "ESC[31" 까지 읽음 (마지막 3바이트)
2. 다음 읽기에서 "m" 이 옴
3. 상태가 `ANSI_STATE_CSI_PARAM`로 남아있어야 함

**현재 구현** (src/bridge.c:205, 276-278):
```c
ansi_state_t current_state = state ? *state : ANSI_STATE_NORMAL;

// ...처리...

if (state) {
    *state = current_state;  // 상태 저장
}
```

**평가**: ✅ **올바름**
- `ansi_filter_state`가 `bridge_ctx_t`에 저장됨 (src/bridge.c:326)
- 호출 시마다 이전 상태를 복원
- 버퍼 경계를 넘어 ANSI 시퀀스를 올바르게 처리

#### ⚠️ **문제 4: ANSI 시퀀스와 멀티바이트 문자가 버퍼 경계에서 동시 분할**

**위험도**: 낮음 (실제 발생 가능성 낮음)

**극단적 시나리오**:
```
버퍼 끝: ... ESC[31m안
다음: 녕ESC[0m
```

1. 첫 읽기: "ESC[31m" 필터링, "안"(3바이트 중 일부)
2. UTF-8 "안" = 0xEC 0x95 0x88
3. 버퍼 끝에 0xEC 0x95만 복사되고 0x88은 다음 버퍼

**분석**:
- ANSI 필터는 멀티바이트 문자를 인식하지 않음
- 바이트 단위로만 처리
- 버퍼 끝에서 멀티바이트 문자가 분할되면 불완전한 UTF-8 전송

**현재 코드**:
```c
if (out_pos < output_size) {
    output[out_pos++] = c;
}
// 버퍼 가득 찬 경우: 바이트 손실
```

**영향**:
- 버퍼 크기가 4096바이트이므로 발생 확률 낮음
- 하지만 이론적으로 가능

**평가**: ⚠️ **개선 권장** (하지만 실제 위험은 낮음)

---

### 4단계: Telnet IAC 이스케이프 (src/bridge.c:590-591)

**코드**:
```c
/* Prepare for telnet (escape IAC) */
telnet_prepare_output(&ctx->telnet, filtered_buf, filtered_len,
                     telnet_buf, sizeof(telnet_buf), &telnet_len);
```

**평가**: ✅ **안전** (이미 검증 완료)
- 0xFF → 0xFF 0xFF 이스케이프
- 버퍼 크기 8192 (충분)
- 이미 TELNET_MULTIBYTE_REVIEW.md에서 검증 완료

---

### 5단계: Telnet 전송 (src/bridge.c:598)

**코드**:
```c
/* Send to telnet */
ssize_t sent = telnet_send(&ctx->telnet, telnet_buf, telnet_len);
```

**평가**: ✅ **안전**
- 바이트 단위 그대로 전송
- non-blocking 처리 (부분 전송 가능)

#### ⚠️ **문제 5: 부분 전송 시 재시도 로직 없음**

**위험도**: 낮음-중간

**시나리오**:
1. `telnet_send()`가 일부만 전송 (예: 2048/4096)
2. 나머지 2048 바이트는 전송 안 됨
3. 멀티바이트 문자가 중간에 끊김

**현재 코드**:
```c
ssize_t sent = telnet_send(&ctx->telnet, telnet_buf, telnet_len);
if (sent > 0) {
    ctx->bytes_serial_to_telnet += sent;
}
// 부분 전송된 경우 재시도 없음!
return SUCCESS;
```

**분석**:
- `telnet_send()` 내부에서 `send()` 시스템 콜 사용
- non-blocking 소켓이므로 부분 전송 가능
- 재시도 로직 없음 → 데이터 손실 가능

**영향**:
- 버퍼 크기가 작으므로 대부분 한 번에 전송
- 하지만 네트워크 혼잡 시 부분 전송 가능
- 멀티바이트 문자 손상 가능성

**평가**: ⚠️ **개선 권장**

---

## 종합 위험도 평가

### 🟢 안전한 부분

| 단계 | 평가 | 이유 |
|------|------|------|
| **Serial 읽기** | ✅ 완벽 | 바이트 단위 그대로 |
| **Modem 처리** | ✅ 완벽 | Online 모드는 투명 전송 |
| **ESC 충돌** | ✅ 안전 | UTF-8/EUC-KR에서 0x1B는 단독 문자 |
| **ANSI 상태 유지** | ✅ 올바름 | 버퍼 경계 넘어 상태 유지 |
| **IAC 이스케이프** | ✅ 안전 | 이미 검증 완료 |

### ⚠️ 개선 권장 부분

| 문제 | 위험도 | 발생 확률 | 영향 |
|------|--------|----------|------|
| **ANSI 필터 버퍼 경계 멀티바이트 분할** | 중간 | 낮음 | UTF-8 손상 |
| **Telnet 부분 전송** | 중간 | 낮음-중간 | 데이터 손실 |

---

## 상세 문제 분석

### 문제 A: ANSI 필터 버퍼 경계 멀티바이트 분할

**발생 시나리오**:
```
Step 1: serial_read() → 4096 bytes
  내용: ... [일반 텍스트] "안" (UTF-8: 0xEC 0x95 0x88)
  버퍼 끝: ... 0xEC 0x95 (out_pos = 4095, 4096)

Step 2: ansi_filter_modem_to_telnet()
  output[4095] = 0xEC
  output[4096] → 버퍼 끝, 0x88 손실

Step 3: telnet_prepare_output()
  입력: 불완전한 UTF-8 (0xEC 0x95)
  출력: 불완전한 UTF-8 전송

결과: 텔넷 서버에서 깨진 문자 수신
```

**현재 동작**:
```c
case ANSI_STATE_NORMAL:
    if (c == 0x1B) {
        current_state = ANSI_STATE_ESC;
    } else {
        /* Normal character - pass through */
        if (out_pos < output_size) {
            output[out_pos++] = c;
        }
        // else: 바이트 손실 (경고 없음)
    }
    break;
```

**권장 해결책**:
1. **버퍼 오버플로 경고 추가**:
```c
else {
    static bool overflow_warned = false;
    if (!overflow_warned) {
        MB_LOG_WARNING("ANSI filter output buffer full - data truncated (multibyte chars may break)");
        overflow_warned = true;
    }
}
```

2. **UTF-8 인식 (선택 사항)**:
- 버퍼 끝에서 불완전한 UTF-8 감지
- 다음 호출까지 보류

### 문제 B: Telnet 부분 전송

**발생 시나리오**:
```
Step 1: telnet_send(4096 bytes)
  send() 반환값: 2048 (부분 전송)

Step 2: bridge_process_serial_data()
  sent = 2048
  나머지 2048 바이트: 손실

결과: 멀티바이트 문자 중간에서 끊김 가능
```

**권장 해결책**:
1. **재시도 로직 추가**:
```c
size_t total_sent = 0;
while (total_sent < telnet_len) {
    ssize_t sent = telnet_send(&ctx->telnet,
                               telnet_buf + total_sent,
                               telnet_len - total_sent);
    if (sent < 0) {
        break;  // Error
    }
    if (sent == 0) {
        // Would block - 다음 번에 재시도
        break;
    }
    total_sent += sent;
}
```

2. **순환 버퍼 사용**:
- 전송 안 된 데이터를 `serial_to_telnet_buf`에 저장
- 다음 호출 시 재시도

---

## 실제 테스트 시나리오

### Test 1: 정상 케이스 - UTF-8 한글 전송 ✅
```
입력: "안녕하세요" (UTF-8)
바이트: EC 95 88 EB 85 95 ED 95 98 EC 84 B8 EC 9A 94 (15 bytes)

경로:
  serial_read() → 15 bytes
  modem_process_input() → consumed = 15
  ansi_filter() → 15 bytes (ANSI 없음)
  telnet_prepare_output() → 15 bytes (IAC 없음)
  telnet_send() → 15 bytes 전송

결과: ✅ 완벽하게 전송
```

### Test 2: ANSI 시퀀스 포함 ✅
```
입력: "ESC[31m안녕ESC[0m"
바이트: 1B 5B 33 31 6D EC 95 88 EB 85 95 1B 5B 30 6D

경로:
  ansi_filter():
    1B → ANSI_STATE_ESC
    5B → ANSI_STATE_CSI
    33 → ANSI_STATE_CSI_PARAM
    31 → ANSI_STATE_CSI_PARAM
    6D → ANSI_STATE_NORMAL (제거)
    EC 95 88 EB 85 95 → 통과
    1B 5B 30 6D → 제거
  출력: EC 95 88 EB 85 95 ("안녕")

결과: ✅ ANSI 시퀀스 제거, 한글 보존
```

### Test 3: 버퍼 경계 - 멀티바이트 분할 ⚠️
```
시나리오: 버퍼 4094바이트 채워진 상태

입력: ... "안녕" (6 bytes)
버퍼 상태: [4094 bytes used] [2 bytes free]

경로:
  ansi_filter():
    out_pos = 4094
    EC → output[4094] = EC, out_pos = 4095
    95 → output[4095] = 95, out_pos = 4096
    88 → out_pos >= 4096, 손실!
    EB, 85, 95 → 손실!
  출력: ... EC 95 (불완전한 UTF-8)

결과: ⚠️ 불완전한 문자 전송 → 텔넷 서버에서 깨짐
```

### Test 4: Telnet 부분 전송 ⚠️
```
시나리오: 네트워크 혼잡

입력: 4096 bytes (한글 포함)
  ... EC 95 88 ... (2048번째 바이트 위치)

경로:
  telnet_send(4096 bytes)
    send() → 반환값 2048 (부분 전송)

  2048 바이트만 전송, 나머지 손실

결과: ⚠️ 멀티바이트 문자 중간에서 끊김
```

---

## 버퍼 크기 분석

### 현재 버퍼 크기 (include/common.h:34)
```c
#define BUFFER_SIZE 4096  // 4KB
```

### 각 단계별 버퍼

| 단계 | 버퍼 이름 | 크기 | 평가 |
|------|----------|------|------|
| Serial 읽기 | `buf` | 4096 | ✅ 충분 |
| ANSI 필터 | `filtered_buf` | 4096 | ✅ 충분 |
| Telnet 준비 | `telnet_buf` | 8192 | ✅ 충분 (2배) |

**일반적인 사용**:
- 터미널 세션: 보통 80-100 bytes/line
- 4096 바이트: 약 40-50 라인
- 충분히 큰 크기

**극단적 케이스**:
- 대용량 파일 전송: 버퍼 가득 참 가능
- 빠른 타이핑: 버퍼 가득 참 가능성 낮음

---

## 권장 개선 사항 우선순위

### 1. 높은 우선순위 🔴

#### 1.1 ANSI 필터 버퍼 오버플로 경고 추가
```c
case ANSI_STATE_NORMAL:
    if (c == 0x1B) {
        current_state = ANSI_STATE_ESC;
    } else {
        if (out_pos < output_size) {
            output[out_pos++] = c;
        } else {
            static bool warned = false;
            if (!warned) {
                MB_LOG_WARNING("ANSI filter buffer full - data truncated (multibyte may break)");
                warned = true;
            }
        }
    }
    break;
```

### 2. 중간 우선순위 🟡

#### 2.1 Telnet 부분 전송 재시도 로직
- 순환 버퍼 활용 (`serial_to_telnet_buf`)
- 전송 안 된 데이터 보관
- 다음 호출 시 재전송

### 3. 낮은 우선순위 🟢

#### 3.1 UTF-8 경계 인식 (ANSI 필터)
- 버퍼 끝에서 불완전한 UTF-8 감지
- 다음 호출까지 보류
- 복잡도 높음, 실제 필요성 낮음

---

## 최종 결론

### 현재 상태 평가: ✅ **양호 (대부분의 사용 케이스에서 안전)**

#### Critical Path 평가 ✅
1. ✅ **Serial → Modem**: 투명 전송
2. ✅ **ESC 충돌**: UTF-8/EUC-KR에서 0x1B는 단독 문자만
3. ✅ **ANSI 필터**: 대부분 안전 (버퍼 크기 충분)
4. ✅ **Telnet 전송**: 대부분 안전

#### 잠재적 문제 ⚠️
1. ⚠️ **ANSI 필터 버퍼 경계**: 극히 드물지만 멀티바이트 분할 가능
2. ⚠️ **Telnet 부분 전송**: 네트워크 혼잡 시 데이터 손실 가능

#### 실사용 평가
- **일반 터미널 사용**: ✅ 완벽
- **대용량 텍스트**: ⚠️ 주의 (버퍼 오버플로 가능)
- **빠른 데이터 전송**: ⚠️ 주의 (부분 전송 가능)

### 권장 조치
1. 🔴 **즉시**: ANSI 필터 버퍼 오버플로 경고 추가
2. 🟡 **단기**: Telnet 부분 전송 재시도 로직 (순환 버퍼)
3. 🟢 **장기**: UTF-8 경계 인식 (선택 사항)

### 요약

**모뎀에서 텔넷으로의 데이터 전송 경로는 대부분의 실제 사용 케이스에서 안전하게 작동합니다.**

- ✅ UTF-8, EUC-KR 멀티바이트 문자 처리 안전
- ✅ ANSI 이스케이프 시퀀스 필터링 정상 동작
- ✅ ESC (0x1B)가 멀티바이트 내부에 나타나지 않음 (충돌 없음)
- ⚠️ 극단적 케이스 (버퍼 가득 참, 부분 전송)에서만 문제 가능

일반적인 터미널 세션(한글, 일본어, 중국어 포함)에서는 완벽하게 작동합니다.
