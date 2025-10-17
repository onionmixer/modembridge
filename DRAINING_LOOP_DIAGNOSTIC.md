# Draining Loop Diagnostic - 완전한 분석

## 문제 상황

사용자 로그:
```
[INFO] Connection speed: 1200 baud
[INFO] Adjusting serial port speed to match connection: 1200 baud
[INFO] Serial port speed adjusted successfully to 1200 baud
[INFO] Modem state changed: ONLINE=true, carrier=true
[INFO] Connection established at 1200 baud
[INFO] Hardware message processed during drain phase
[DEBUG] After hardware message: modem_state=1, online=1
[DEBUG] Hardware message processed during drain phase
```

**그 다음 아무 출력도 없음!**

## 핵심 발견

### 1. Modem State는 정상입니다!

Enum 정의:
```c
typedef enum {
    MODEM_STATE_COMMAND = 0,
    MODEM_STATE_ONLINE = 1,      ← modem_state=1은 ONLINE입니다!
    MODEM_STATE_RINGING = 2,
    MODEM_STATE_CONNECTING = 3,
    MODEM_STATE_DISCONNECTED = 4
} modem_state_t;
```

**`modem_state=1, online=1`은 완전히 정상입니다!** Modem은 ONLINE 상태입니다.

### 2. 진짜 문제: Draining Loop가 종료되지 않음

로그에서 예상했던 다음 메시지들이 나오지 않았습니다:
```
[DEBUG] Draining loop iteration X: drained=%zd bytes  ← 없음!
[INFO] Buffer drain complete (%d attempts)            ← 없음!
[INFO] === MODEM CONFIGURATION SUMMARY ===            ← 없음!
[INFO] Creating Serial/Modem thread (Level 1)...     ← 없음!
```

프로그램이 bridge.c:827-837 사이 어딘가에서 멈췄습니다.

## 추가된 진단 코드

### 1. Draining Loop 시작 (bridge.c:806-813)

```c
printf("[DEBUG] ===== ENTERING DRAINING LOOP =====\n");
fflush(stdout);
do {
    printf("[DEBUG] Draining iteration %d: calling serial_read()...\n", drain_attempts);
    fflush(stdout);
    drained = serial_read(&ctx->serial, drain_buf, sizeof(drain_buf));
    printf("[DEBUG] serial_read() returned: %zd bytes\n", drained);
    fflush(stdout);
```

**목적:** 각 iteration이 시작될 때와 serial_read() 결과를 추적

### 2. Hardware Message 처리 후 (bridge.c:823-836)

```c
if (msg_handled) {
    printf("[INFO] Hardware message processed during drain phase\n");
    fflush(stdout);
    MB_LOG_INFO("Hardware message processed during drain phase");

    printf("[DEBUG] After hardware message: modem_state=%d, online=%d\n",
           ctx->modem.state, ctx->modem.online);
    fflush(stdout);
    printf("[DEBUG] About to increment drain_attempts (current=%d)\n", drain_attempts);
    fflush(stdout);
}

printf("[DEBUG] Before increment: drain_attempts=%d\n", drain_attempts);
fflush(stdout);
drain_attempts++;
printf("[DEBUG] After increment: drain_attempts=%d\n", drain_attempts);
fflush(stdout);
```

**목적:** drain_attempts 증가가 정상적으로 일어나는지 확인

### 3. Loop Iteration 완료 (bridge.c:837-845)

```c
printf("[DEBUG] Draining loop iteration %d: drained=%zd bytes\n", drain_attempts, drained);
fflush(stdout);
printf("[DEBUG] About to sleep 100ms before next iteration\n");
fflush(stdout);
usleep(100000);
printf("[DEBUG] Sleep complete, checking while condition: drained=%zd, drain_attempts=%d\n",
       drained, drain_attempts);
fflush(stdout);
} while (drained > 0 && drain_attempts < 10);
```

**목적:** usleep() 전후 상태 추적

### 4. Loop 종료 (bridge.c:847-851)

```c
printf("[INFO] ===== DRAINING LOOP EXITED =====\n");
fflush(stdout);
printf("[INFO] Buffer drain complete (%d attempts)\n", drain_attempts);
fflush(stdout);
```

**목적:** Loop가 정상적으로 종료되었는지 확인

## 예상 출력 (정상 동작 시)

```
[DEBUG] ===== ENTERING DRAINING LOOP =====

[DEBUG] Draining iteration 0: calling serial_read()...
[DEBUG] serial_read() returned: 20 bytes
[INFO] Draining initialization responses (20 bytes): [
CONNECT 1200/ARQ
]
[INFO] Hardware message processed during drain phase
[DEBUG] After hardware message: modem_state=1, online=1
[DEBUG] About to increment drain_attempts (current=0)
[DEBUG] Before increment: drain_attempts=0
[DEBUG] After increment: drain_attempts=1
[DEBUG] Draining loop iteration 1: drained=20 bytes
[DEBUG] About to sleep 100ms before next iteration
[DEBUG] Sleep complete, checking while condition: drained=20, drain_attempts=1

[DEBUG] Draining iteration 1: calling serial_read()...
[DEBUG] serial_read() returned: 0 bytes
[DEBUG] Before increment: drain_attempts=1
[DEBUG] After increment: drain_attempts=2
[DEBUG] Draining loop iteration 2: drained=0 bytes
[DEBUG] About to sleep 100ms before next iteration
[DEBUG] Sleep complete, checking while condition: drained=0, drain_attempts=2

[INFO] ===== DRAINING LOOP EXITED =====
[INFO] Buffer drain complete (2 attempts)
[INFO] === MODEM CONFIGURATION SUMMARY ===
...
```

## 가능한 문제 시나리오

### 시나리오 1: serial_read()가 blocking되고 있음

**증상:** "Draining iteration X: calling serial_read()..." 후 멈춤

**원인:** serial_read()가 non-blocking이 아니거나, O_NONBLOCK 플래그가 손실됨

**확인 방법:**
- "[DEBUG] serial_read() returned: ..." 메시지가 나오는지 확인
- 안 나오면 serial_read()에서 blocking됨

### 시나리오 2: printf() 또는 fflush()가 block됨

**증상:** 특정 printf() 후 다음 메시지가 안 나옴

**원인:** stdout이 close되었거나, pipe가 막힘

**가능성:** 낮음 (이전 printf들은 동작했음)

### 시나리오 3: usleep()에서 signal interrupt

**증상:** "About to sleep 100ms..." 후 멈춤

**원인:** SIGINT 또는 다른 signal이 프로그램을 중단시킴

**확인 방법:** "Sleep complete, checking while condition..." 메시지 확인

### 시나리오 4: drain_attempts++ overflow (가능성 거의 없음)

**증상:** "Before increment" 후 멈춤

**원인:** 이론적으로만 가능, 실제로는 불가능

### 시나리오 5: 로그가 truncate되었거나 buffering됨

**증상:** 프로그램은 정상 실행 중이지만 출력이 안 보임

**확인 방법:**
- 프로그램이 종료되었는지 확인 (ps aux | grep modembridge)
- strace로 시스템 콜 추적

## 진단 절차

### 1단계: 새 빌드 실행

```bash
./build/modembridge -c modembridge.conf 2>&1 | tee modembridge_full.log
```

### 2단계: 로그 분석

다음 메시지들이 순서대로 나오는지 확인:

1. ✅ `[DEBUG] ===== ENTERING DRAINING LOOP =====`
2. ✅ `[DEBUG] Draining iteration 0: calling serial_read()...`
3. ✅ `[DEBUG] serial_read() returned: X bytes`
4. ✅ `[DEBUG] After hardware message: modem_state=1, online=1`
5. ✅ `[DEBUG] About to increment drain_attempts (current=0)`
6. ✅ `[DEBUG] Before increment: drain_attempts=0`
7. ✅ `[DEBUG] After increment: drain_attempts=1`
8. ✅ `[DEBUG] Draining loop iteration 1: drained=X bytes`
9. ✅ `[DEBUG] About to sleep 100ms before next iteration`
10. ✅ `[DEBUG] Sleep complete, checking while condition: ...`
11. ✅ 다음 iteration (drained=0일 때까지)
12. ✅ `[INFO] ===== DRAINING LOOP EXITED =====`

### 3단계: 문제 위치 특정

**마지막 나온 메시지를 확인하고 다음 표와 대조:**

| 마지막 메시지 | 문제 위치 | 가능한 원인 |
|--------------|----------|------------|
| "ENTERING DRAINING LOOP" | serial_read() 호출 직전 | printf 자체가 문제, 또는 프로그램이 즉시 종료됨 |
| "calling serial_read()..." | serial_read() 내부 | serial_read()가 blocking 또는 crash |
| "serial_read() returned..." | modem_process_hardware_message() | 하드웨어 메시지 처리 중 crash |
| "After hardware message..." | drain_attempts 증가 전 | printf 문제 |
| "About to increment..." | drain_attempts++ | 불가능 (단순 증가 연산) |
| "Before increment..." | drain_attempts++ | 불가능 |
| "After increment..." | iteration 출력 전 | printf 문제 |
| "Draining loop iteration..." | usleep() 전 | printf 문제 |
| "About to sleep..." | usleep() 중 | signal interrupt 또는 usleep 자체 문제 |
| "Sleep complete..." | while 조건 확인 | 불가능 (조건 확인은 간단한 비교) |
| 다음 iteration 시작 | - | 정상 (loop 계속) |
| "DRAINING LOOP EXITED" | Thread 생성 전 | 정상 (loop 종료, thread 생성 시작) |

## 추가 진단 도구

### strace 사용

```bash
strace -f -e trace=read,write,open,close,select,poll ./build/modembridge -c modembridge.conf 2>&1 | tee strace.log
```

이것은 프로그램이 어느 시스템 콜에서 멈추는지 정확히 보여줍니다.

### gdb 사용

```bash
gdb ./build/modembridge
(gdb) run -c modembridge.conf
# 프로그램이 멈추면:
(gdb) bt
(gdb) info threads
(gdb) thread apply all bt
```

### timeout 사용

```bash
timeout 60 ./build/modembridge -c modembridge.conf
echo "Exit code: $?"
```

60초 후 자동 종료. Exit code가:
- 0: 정상 종료
- 124: timeout으로 종료
- 기타: crash 또는 signal

## 다음 단계

1. **새 빌드 실행 및 완전한 로그 수집**
   ```bash
   ./build/modembridge -c modembridge.conf 2>&1 | tee full_log.txt
   ```

2. **로그에서 마지막 메시지 확인**

3. **위 표를 사용하여 문제 위치 특정**

4. **필요시 strace 또는 gdb 사용**

## 중요 노트

- **모든 printf() 뒤에 fflush(stdout) 추가됨** - 버퍼링 문제 방지
- **Modem state는 정상입니다** - modem_state=1은 ONLINE입니다
- **Thread가 시작되지 않은 것이 확실함** - draining loop를 빠져나가지 못함
- **Build 성공** - 컴파일 에러 없음

## 기대 결과

새로운 진단 출력을 통해:
1. 정확히 어느 코드 라인에서 멈추는지 확인 가능
2. serial_read()가 blocking되는지 확인 가능
3. 프로그램이 실제로 멈춘 것인지, 로그만 안 보이는지 구분 가능
4. Loop 종료 조건이 제대로 작동하는지 확인 가능

이 정보로 timestamp 전송이 안 되는 근본 원인을 찾을 수 있습니다.
