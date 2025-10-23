# Modem Layer Multibyte Character Handling Review

## 개요
모뎀 레이어(Hayes AT 명령 에뮬레이션)에서 시리얼 포트로부터 받는 데이터의 멀티바이트 문자 처리를 검토합니다. 특히 Online 모드와 Command 모드에서의 처리 차이를 분석합니다.

## 데이터 흐름

```
Serial Port → modem_process_input() → {
    Online Mode: 이스케이프 시퀀스 감지(+++) → 텔넷으로 투명 전송
    Command Mode: AT 명령어 파싱 → 응답 생성
}
```

## 현재 구현 분석

### 1. Online 모드 (데이터 투명 전송) - `modem_process_input()` (src/modem.c:550-582)

#### 1.1 이스케이프 시퀀스 감지 (+++)

**코드 분석** (src/modem.c:550-578):
```c
if (modem->online) {
    int escape_char = modem->settings.s_registers[SREG_ESCAPE_CHAR];  // 기본값: '+'

    for (size_t i = 0; i < len; i++) {
        if (data[i] == escape_char) {
            /* 가드 타임 체크 */
            if (modem->escape_count == 0 || (now - modem->last_escape_time) <= 2) {
                modem->escape_count++;
                modem->last_escape_time = now;

                if (modem->escape_count >= 3) {
                    /* +++ 감지 - 명령 모드로 전환 */
                    modem_go_offline(modem);
                    modem_send_response(modem, MODEM_RESP_OK);
                    consumed = i + 1;
                    return consumed;
                }
            } else {
                /* 가드 타임 위반 - 리셋 */
                modem->escape_count = 1;
                modem->last_escape_time = now;
            }
        } else {
            /* Non-escape 문자 - 카운터 리셋 */
            modem->escape_count = 0;
        }
    }

    /* Online 모드에서는 데이터 그대로 통과 */
    return len;
}
```

#### 🔴 **문제 1: 멀티바이트 문자 내부의 0x2B ('+') 오감지**

**위험도**: 낮음-중간

**시나리오**:
1. UTF-8/EUC-KR 데이터가 전송됨
2. 멀티바이트 문자의 일부 바이트가 우연히 0x2B ('+')
3. 연속으로 3번 나타나면 이스케이프 시퀀스로 오감지
4. 의도치 않게 명령 모드로 전환

**발생 가능성 분석**:

| 인코딩 | '+' (0x2B) 포함 가능성 | 연속 3회 확률 | 위험도 |
|--------|----------------------|--------------|--------|
| **UTF-8** | 매우 낮음 | 극히 낮음 | 🟢 낮음 |
| **EUC-KR** | 낮음 | 매우 낮음 | 🟡 낮음-중간 |
| **ASCII** | 있음 | 사용자 입력 | ✅ 정상 |

**UTF-8 분석**:
- UTF-8 멀티바이트 문자 구조:
  - 2바이트: 110xxxxx 10xxxxxx (0xC0-0xDF, 0x80-0xBF)
  - 3바이트: 1110xxxx 10xxxxxx 10xxxxxx (0xE0-0xEF, 0x80-0xBF, 0x80-0xBF)
  - 4바이트: 11110xxx 10xxxxxx 10xxxxxx 10xxxxxx
- 0x2B (00101011)는 ASCII 범위이므로 UTF-8에서 단독으로만 나타남
- 멀티바이트 문자의 일부로 0x2B 등장 가능성: **없음**

**EUC-KR 분석**:
- EUC-KR 범위: 첫 바이트 0xA1-0xFE, 두 번째 바이트 0xA1-0xFE
- 0x2B는 범위 밖이므로 멀티바이트 문자의 일부로 나타날 수 없음
- EUC-KR에서 0x2B 등장: ASCII '+' 문자일 때만

**결론**: ✅ **UTF-8/EUC-KR에서 안전**
- UTF-8과 EUC-KR 모두 0x2B가 멀티바이트 문자의 일부로 나타날 수 없음
- 0x2B는 항상 실제 '+' 문자를 의미
- 이스케이프 시퀀스 감지는 멀티바이트 문자와 충돌하지 않음

#### 1.2 데이터 투명 전송

**코드** (src/modem.c:581):
```c
/* In online mode, data passes through */
return len;
```

**평가**: ✅ **완벽**
- Online 모드에서는 이스케이프 시퀀스 체크 외 아무 처리도 하지 않음
- 모든 데이터가 투명하게(바이트 단위 그대로) 전달됨
- 멀티바이트 문자가 안전하게 통과

### 2. Command 모드 (AT 명령어 파싱) - `modem_process_input()` (src/modem.c:584-655)

#### 2.1 바이트 단위 처리

**코드 분석** (src/modem.c:585-607):
```c
for (size_t i = 0; i < len; i++) {
    char c = data[i];
    int cr_char = modem->settings.s_registers[SREG_CR_CHAR];     // 기본값: '\r'
    int bs_char = modem->settings.s_registers[SREG_BS_CHAR];     // 기본값: '\b'

    /* Echo if enabled */
    if (modem->settings.echo && c != '\0') {
        serial_write(modem->serial, &c, 1);
    }

    /* Handle backspace */
    if (c == bs_char || c == 127) {
        if (modem->cmd_len > 0) {
            modem->cmd_len--;
            /* Echo backspace sequence if echo enabled */
            if (modem->settings.echo) {
                const char bs_seq[] = "\b \b";
                serial_write(modem->serial, bs_seq, 3);
            }
        }
        consumed++;
        continue;
    }
```

#### ⚠️ **문제 2: Backspace 처리 시 멀티바이트 문자 분할**

**위험도**: 중간

**시나리오**:
1. 사용자가 AT 명령어 입력 중 실수로 한글 입력 (예: "AT안")
2. UTF-8 "안" = 0xEC 0x95 0x88 (3바이트)
3. cmd_buffer에 "AT" + 0xEC 0x95 0x88 저장 (총 5바이트)
4. 사용자가 Backspace 누름
5. cmd_len이 1 감소 → 0x88만 제거
6. cmd_buffer에 "AT" + 0xEC 0x95 남음 (불완전한 UTF-8)
7. 이후 Enter를 누르면 불완전한 문자가 AT 명령어로 파싱 시도

**영향**:
- AT 명령어 파싱 실패 → ERROR 응답
- 실제 위험은 낮음 (단순히 명령어 실패)
- 사용자 경험 저하 (한글 지우려면 3번 Backspace 필요)

**발생 빈도**: 낮음 (일반적으로 AT 명령어는 ASCII만 사용)

#### 2.2 CR/LF 처리 및 AT 명령어 파싱

**코드 분석** (src/modem.c:609-640):
```c
/* Handle carriage return - execute command */
if (c == cr_char || c == '\n') {
    consumed++;

    if (modem->cmd_len == 0) {
        /* Empty line - send OK */
        modem_send_response(modem, MODEM_RESP_OK);
        continue;
    }

    /* Null-terminate command */
    modem->cmd_buffer[modem->cmd_len] = '\0';

    /* Check for AT prefix */
    if (modem->cmd_len >= 2 &&
        toupper(modem->cmd_buffer[0]) == 'A' &&
        toupper(modem->cmd_buffer[1]) == 'T') {
        /* Process AT command (skip "AT" prefix) */
        modem_process_command(modem, modem->cmd_buffer + 2);
    } else if (modem->cmd_len == 1 && toupper(modem->cmd_buffer[0]) == 'A') {
        /* Just "A" - repeat last command or answer */
        modem_answer(modem);
    } else {
        /* Invalid command */
        MB_LOG_WARNING("Invalid command: %s", modem->cmd_buffer);
        modem_send_response(modem, MODEM_RESP_ERROR);
    }

    /* Clear command buffer */
    modem->cmd_len = 0;
    continue;
}
```

#### ⚠️ **문제 3: toupper() 함수의 멀티바이트 문자 오처리**

**위험도**: 낮음

**시나리오**:
1. 사용자가 실수로 "안T" 입력 (UTF-8: 0xEC 0x95 0x88 0x54)
2. cmd_buffer[0] = 0xEC (signed char: -20)
3. `toupper(0xEC)` 호출
4. toupper()는 unsigned char (0-255) 또는 EOF를 기대
5. 음수 값 전달 시 **Undefined Behavior** (UB)

**C 표준 (7.4.2.2)**:
> The behavior of the toupper function is undefined if the argument is neither EOF nor representable as unsigned char.

**실제 영향**:
- 대부분의 libc 구현에서는 범위 외 값을 그대로 반환
- 실제로 충돌이나 오동작은 드뭄
- 하지만 UB이므로 이론적으로는 모든 것이 가능

**잠재적 결과**:
- 0xEC가 'A' (0x41)와 일치하지 않음 → "Invalid command" ERROR
- 실제 위험은 낮지만 표준 위반

#### 2.3 명령어 버퍼에 문자 추가

**코드** (src/modem.c:642-651):
```c
/* Add character to command buffer */
if (modem->cmd_len < sizeof(modem->cmd_buffer) - 1) {
    modem->cmd_buffer[modem->cmd_len++] = c;
} else {
    MB_LOG_WARNING("Command buffer overflow");
    modem->cmd_len = 0;
    modem_send_response(modem, MODEM_RESP_ERROR);
}
```

**평가**: ⚠️ **주의 필요**
- 버퍼 오버플로는 방지됨
- 하지만 멀티바이트 문자 경계는 고려하지 않음
- 버퍼가 가득 차면 멀티바이트 문자 중간에서 끊길 수 있음

**시나리오**:
1. LINE_BUFFER_SIZE = 256이라고 가정
2. cmd_buffer에 254바이트 저장됨
3. UTF-8 한글 "안" (3바이트) 입력 시작
4. 0xEC, 0x95는 추가되지만 0x88은 버퍼 오버플로로 거부
5. 불완전한 UTF-8 시퀀스가 버퍼에 남음

**영향**: 낮음 (AT 명령어는 일반적으로 짧고 ASCII만 사용)

### 3. AT 명령어 파싱 - `modem_process_command()` (src/modem.c:357-535)

**코드 분석** (src/modem.c:368-371):
```c
/* Convert to uppercase for easier parsing */
for (size_t i = 0; i < sizeof(cmd_upper) - 1 && command[i]; i++) {
    cmd_upper[i] = toupper(command[i]);
    cmd_upper[i + 1] = '\0';
}
```

#### ⚠️ **문제 4: toupper() 반복 사용 (Undefined Behavior)**

**위험도**: 낮음

동일한 문제:
- 멀티바이트 문자의 각 바이트가 toupper()에 전달
- signed char 음수 값 → UB
- 실제 영향은 미미하지만 표준 위반

## 버퍼 크기 분석

### Command Buffer 크기

**정의** (include/common.h):
```c
#define LINE_BUFFER_SIZE 256
```

**사용 위치** (include/modem.h:37):
```c
char cmd_buffer[LINE_BUFFER_SIZE];  /* Command buffer */
```

**평가**: ✅ **충분**
- AT 명령어는 일반적으로 매우 짧음 (< 50 bytes)
- 256 바이트는 넉넉함
- 멀티바이트 문자가 버퍼에 들어갈 가능성 낮음 (AT 명령어는 ASCII)

## 종합 위험도 평가

### 🟢 안전한 부분 (Critical Path)

#### 1. Online 모드 데이터 전송 ✅
- **평가**: 완벽하게 안전
- **이유**: 데이터가 투명하게 통과, 바이트 단위 처리 없음
- **멀티바이트 지원**: UTF-8, EUC-KR, 모든 인코딩 완벽 지원

#### 2. 이스케이프 시퀀스 감지 (+++감지) ✅
- **평가**: 안전
- **이유**:
  - UTF-8에서 0x2B는 멀티바이트 문자 내부에 나타날 수 없음
  - EUC-KR에서도 0x2B는 ASCII 범위로 독립적
  - 오감지 가능성 없음

### ⚠️ 개선 권장 부분 (Non-Critical)

#### 1. Command 모드 Backspace 처리 ⚠️
- **위험도**: 중간 (사용성 문제)
- **영향**: 멀티바이트 문자 삭제 시 바이트 단위로 지워짐
- **발생 빈도**: 낮음 (AT 명령어는 ASCII)
- **실제 위험**: 낮음 (단순히 ERROR 응답)

#### 2. toupper() Undefined Behavior ⚠️
- **위험도**: 낮음 (이론적 문제)
- **영향**: 멀티바이트 문자가 AT 명령어 버퍼에 들어갈 경우 UB
- **발생 빈도**: 매우 낮음
- **실제 영향**: 미미 (대부분 libc는 안전하게 처리)
- **권장**: `toupper((unsigned char)c)` 캐스팅

#### 3. 버퍼 경계 멀티바이트 분할 ⚠️
- **위험도**: 낮음
- **영향**: 버퍼 가득 참 시 멀티바이트 문자 중간에서 끊김
- **발생 빈도**: 극히 낮음 (256 바이트 버퍼, AT 명령어는 짧음)

## 권장 개선 사항

### 1. 높은 우선순위 (선택 사항) 🟡

#### 1.1 toupper() 안전한 사용

**현재 코드**:
```c
toupper(modem->cmd_buffer[0])
```

**개선 코드**:
```c
toupper((unsigned char)modem->cmd_buffer[0])
```

**이유**: C 표준 준수, UB 방지

**적용 위치**:
- `modem_process_input()`: src/modem.c:624, 628
- `modem_process_command()`: src/modem.c:369

### 2. 중간 우선순위 (선택 사항) 🟡

#### 2.1 Backspace 처리 개선 (UTF-8 인식)

**개선 방향**:
- UTF-8 문자 경계 인식
- Backspace 시 완전한 UTF-8 문자 단위로 삭제
- bridge.c에 이미 UTF-8 헬퍼 함수 존재:
  - `is_utf8_start()`
  - `is_utf8_continuation()`
  - `utf8_sequence_length()`

**구현 복잡도**: 중간
**실제 필요성**: 낮음 (AT 명령어는 ASCII)

### 3. 낮은 우선순위 (선택 사항) 🟢

#### 3.1 버퍼 오버플로 시 UTF-8 경계 고려

**개선 방향**:
- 버퍼 가득 참 시 마지막 불완전한 UTF-8 문자 제거

**실제 필요성**: 매우 낮음

## 실제 사용 시나리오 분석

### 시나리오 1: 정상적인 AT 명령어 사용 ✅
```
입력: "ATZ\r"
처리: ASCII만 사용, 완벽하게 동작
결과: OK
```
**평가**: 완벽

### 시나리오 2: Online 모드에서 한글 전송 ✅
```
입력: UTF-8 "안녕하세요"
처리: 투명 전송, 바이트 단위 그대로 통과
결과: 텔넷 서버에 완벽하게 전달
```
**평가**: 완벽

### 시나리오 3: Online 모드에서 이스케이프 시퀀스 ✅
```
입력: "+++" (가드 타임 내)
처리: escape_count = 3 감지
결과: 명령 모드 전환, "OK" 응답
```
**평가**: 완벽 (멀티바이트 문자와 충돌 없음)

### 시나리오 4: 실수로 Command 모드에서 한글 입력 ⚠️
```
입력: "AT안\r"
처리:
  1. cmd_buffer = "AT" + 0xEC 0x95 0x88
  2. toupper(0xEC) → UB (실제로는 그대로)
  3. 0xEC != 'A' → Invalid command
결과: "ERROR" 응답
```
**평가**: 안전 (ERROR로 처리됨, 충돌 없음)

### 시나리오 5: Backspace로 한글 삭제 시도 ⚠️
```
입력: "AT안"
Backspace 1회: "AT" + 0xEC 0x95 (불완전)
Backspace 1회: "AT" + 0xEC (불완전)
Backspace 1회: "AT" (완전)
Enter: "AT\r" → OK
```
**평가**: 동작하지만 3번 눌러야 함 (사용성 저하)

## 멀티바이트 문자가 포함된 데이터 흐름 테스트

### Test 1: UTF-8 한글 "안녕" Online 전송
```
Serial → modem_process_input(online=true)
  입력: 0xEC 0x95 0x88 0xEB 0x85 0x95

  이스케이프 체크:
    0xEC == '+'? No → escape_count = 0
    0x95 == '+'? No → escape_count = 0
    0x88 == '+'? No → escape_count = 0
    0xEB == '+'? No → escape_count = 0
    0x85 == '+'? No → escape_count = 0
    0x95 == '+'? No → escape_count = 0

  return len (6) → 모든 데이터 통과

→ bridge_process_serial_data()
  → telnet_prepare_output() (IAC 이스케이프)
    → telnet_send() → 텔넷 서버
```
**결과**: ✅ 완벽하게 전송

### Test 2: EUC-KR 한글 "안녕" Online 전송
```
Serial → modem_process_input(online=true)
  입력: 0xBE 0xC8 0xB3 0xE7

  이스케이프 체크:
    0xBE == 0x2B? No
    0xC8 == 0x2B? No
    0xB3 == 0x2B? No
    0xE7 == 0x2B? No

  return len (4) → 모든 데이터 통과
```
**결과**: ✅ 완벽하게 전송

## 최종 결론

### 멀티바이트 문자 처리 상태: ✅ **프로덕션 사용 가능**

#### Critical Path (Online 모드) ✅
- **평가**: 완벽
- **이유**:
  - 데이터 투명 전송
  - 이스케이프 시퀀스 감지 안전 (UTF-8/EUC-KR과 충돌 없음)
  - 멀티바이트 문자가 안전하게 통과

#### Non-Critical Path (Command 모드) ⚠️
- **평가**: 양호 (개선 권장)
- **이유**:
  - AT 명령어는 ASCII만 사용하므로 실제 문제 없음
  - toupper() UB는 이론적 문제 (실제 영향 미미)
  - Backspace 처리는 사용성 문제 (안전성 문제 아님)

### 실사용 평가

| 사용 케이스 | 평가 | 비고 |
|-------------|------|------|
| **Online 모드 데이터 전송** | ✅ 완벽 | UTF-8, EUC-KR 모두 안전 |
| **이스케이프 시퀀스 (+++)** | ✅ 완벽 | 멀티바이트와 충돌 없음 |
| **AT 명령어 입력 (ASCII)** | ✅ 완벽 | 정상 사용 시 문제 없음 |
| **AT 명령어에 멀티바이트 입력** | ⚠️ 양호 | ERROR 응답, 충돌 없음 |
| **Backspace로 한글 삭제** | ⚠️ 양호 | 3번 눌러야 함 (사용성) |

### 권장 조치 우선순위

1. 🟢 **현재 상태 유지**: 실제 사용에 문제 없음
2. 🟡 **선택적 개선**: toupper() 캐스팅 추가 (표준 준수)
3. 🟡 **선택적 개선**: Backspace UTF-8 인식 (사용성 향상)

### 요약

**모뎀 레이어의 멀티바이트 문자 처리는 프로덕션 환경에서 안전하게 사용 가능합니다.**

- ✅ Online 모드 (실제 데이터 전송): 완벽
- ✅ 이스케이프 시퀀스 감지: 안전 (오감지 없음)
- ⚠️ Command 모드 (AT 명령어): 양호 (개선 권장하지만 필수 아님)

한글, 일본어, 중국어 등 멀티바이트 문자를 사용하는 텔넷 세션에서 정상적으로 작동합니다.
