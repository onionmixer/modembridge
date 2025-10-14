# ModemBridge 연결 처리 로직 검토 보고서

## 검토 일자
2025년 기준

## 발견된 주요 문제점

### 1. 심각: 온라인 모드 데이터 처리 중복 및 데이터 손실
**위치**: `src/bridge.c:501-552` (`bridge_process_serial_data()`)

**문제**:
```c
/* Line 511: 첫 번째 read */
n = serial_read(&ctx->serial, buf, sizeof(buf));

/* Line 523-536: Command 모드 처리 */
if (!modem_is_online(&ctx->modem)) {
    ssize_t consumed = modem_process_input(&ctx->modem, (char *)buf, n);
    // ... 여기서 ATA 처리하고 return
    return SUCCESS;
}

/* Line 539-550: Online 모드 처리 - 문제! */
ssize_t consumed = modem_process_input(&ctx->modem, (char *)buf, n);  // 중복 호출
if (!modem_is_online(&ctx->modem)) {
    return SUCCESS;
}

/* Line 548-550: 데이터 전송 시도 */
if (telnet_is_connected(&ctx->telnet) && consumed > 0) {
    bridge_transfer_serial_to_telnet(ctx);  // ❌ 읽은 버퍼를 전달하지 않음!
}
```

**결과**:
- Online 모드에서 `modem_process_input()`이 escape sequence 감지만 하고 데이터를 반환하지만 (line 265: `return len`), 이 데이터를 사용하지 않음
- `bridge_transfer_serial_to_telnet()`은 자체적으로 다시 `serial_read()`를 호출 (line 95)
- **첫 번째 읽은 데이터가 완전히 손실됨**

**권장 수정**:
```c
int bridge_process_serial_data(bridge_ctx_t *ctx)
{
    unsigned char buf[BUFFER_SIZE];
    ssize_t n;

    n = serial_read(&ctx->serial, buf, sizeof(buf));
    if (n <= 0) return SUCCESS;

    if (!modem_is_online(&ctx->modem)) {
        /* Command mode */
        modem_process_input(&ctx->modem, (char *)buf, n);
        if (modem_get_state(&ctx->modem) == MODEM_STATE_CONNECTING) {
            bridge_handle_modem_connect(ctx);
        }
    } else {
        /* Online mode - check escape sequence */
        ssize_t consumed = modem_process_input(&ctx->modem, (char *)buf, n);
        if (!modem_is_online(&ctx->modem)) {
            return SUCCESS;  // Escaped to command mode
        }

        /* Transfer actual data to telnet */
        if (telnet_is_connected(&ctx->telnet)) {
            bridge_transfer_data_to_telnet(ctx, buf, consumed);  // 읽은 데이터 전달
        }
    }
    return SUCCESS;
}
```

### 2. 심각: ATA 명령 후 OK 응답 누락
**위치**: `src/modem.c:71-78` (`modem_process_command()`)

**문제**:
```c
/* ATA - Answer */
if (*p == 'A' && *(p + 1) != 'T') {
    MB_LOG_INFO("AT command: ATA (Answer)");
    ret = modem_answer(modem);
    if (ret == SUCCESS) {
        /* Answer will be handled by bridge connecting to telnet */
        return SUCCESS;  // ❌ OK 응답을 보내지 않고 return
    }
    p++;
}
```

**결과**:
- ATA 명령에 대한 OK 응답이 없음
- 실제 모뎀 동작과 불일치 (정상: `ATA` → `OK` → 연결 시도 → `CONNECT`)

**권장 수정**:
```c
if (*p == 'A' && *(p + 1) != 'T') {
    MB_LOG_INFO("AT command: ATA (Answer)");
    ret = modem_answer(modem);
    /* ATA 명령은 OK를 먼저 보낸 후 연결 처리는 bridge에서 수행 */
    p++;
}
// 루프 끝에서 OK 응답
```

### 3. 중간: ATH 명령 후 bridge 상태 동기화 실패
**위치**: `src/modem.c:100-108` (`modem_process_command()`)

**문제**:
```c
/* ATH - Hang up */
else if (*p == 'H') {
    MB_LOG_INFO("AT command: ATH (Hang up)");
    p++;
    if (isdigit(*p)) p++;
    ret = modem_hangup(modem);
    if (ret == SUCCESS) {
        return modem_send_response(modem, MODEM_RESP_OK);  // OK 전송 후 종료
    }
}
```

**결과**:
- `modem_hangup()`이 modem 상태만 DISCONNECTED로 변경
- bridge에서 이 상태 변화를 감지하지 못함
- telnet 연결이 그대로 유지됨

**권장 수정**:
`bridge_process_serial_data()`에서 상태 감지 추가:
```c
modem_process_input(&ctx->modem, (char *)buf, n);

/* Check for state changes */
if (modem_get_state(&ctx->modem) == MODEM_STATE_CONNECTING) {
    bridge_handle_modem_connect(ctx);
} else if (modem_get_state(&ctx->modem) == MODEM_STATE_DISCONNECTED) {
    bridge_handle_modem_disconnect(ctx);
}
```

### 4. 중간: Escape sequence (+++ATH) 후 데이터 처리
**위치**: `src/modem.c:233-265` (`modem_process_input()` online mode)

**문제**:
```c
if (modem->escape_count >= 3) {
    MB_LOG_INFO("Escape sequence detected (+++), entering command mode");
    modem_go_offline(modem);
    modem_send_response(modem, MODEM_RESP_OK);
    consumed = i + 1;
    return consumed;  // escape 문자 이후 데이터는 어떻게?
}
```

**고려사항**:
- Escape 시퀀스 감지 후 남은 데이터 처리가 불명확
- `consumed`를 리턴하지만 bridge에서 이를 활용하지 않음

### 5. 경미: modem_go_offline() 후 carrier 상태 불일치
**위치**: `src/modem.c:227-243` (`modem_go_offline()`)

**문제**:
```c
int modem_go_offline(modem_t *modem)
{
    modem->state = MODEM_STATE_COMMAND;
    modem->online = false;
    modem->escape_count = 0;
    modem->last_escape_time = 0;
    modem->cmd_len = 0;
    return SUCCESS;
}
```

**결과**:
- `carrier` 플래그가 여전히 true로 유지
- `ATO` 명령 시 다시 온라인 모드로 복귀 가능 (이것이 의도된 동작인가?)

**검토 필요**:
- Escape sequence로 command mode 진입 시 carrier를 유지할지 여부
- 실제 Hayes 모뎀 동작: Escape 후에도 carrier 유지, ATO로 복귀 가능

### 6. 경미: 초기 시작 시 불필요한 OK 전송
**위치**: `src/bridge.c:362-363` (`bridge_start()`)

**문제**:
```c
/* Send initial modem response */
modem_send_response(&ctx->modem, MODEM_RESP_OK);
```

**고려사항**:
- 실제 모뎀은 전원 투입/리셋 시 `OK`를 보내지 않음
- 첫 AT 명령 입력 시에만 응답
- 클라이언트가 혼란스러울 수 있음

## 연결 흐름 분석

### 정상 연결 시나리오 (현재 구현)
```
1. Client → Serial: "ATA\r"
2. modem_process_input() 처리
3. modem_process_command() → modem_answer()
4. modem state = CONNECTING (OK 없이 return)  ❌
5. bridge_process_serial_data()에서 CONNECTING 감지
6. bridge_handle_modem_connect() 호출
7. telnet_connect() 수행
8. modem_go_online() 호출
9. Client ← Serial: "CONNECT 57600\r\n"
10. 데이터 전송 시작 (하지만 첫 데이터 손실 가능)  ❌
```

### 예상되는 정상 흐름
```
1. Client → Serial: "ATA\r"
2. modem_process_input() 처리
3. modem_process_command() → modem_answer()
4. Client ← Serial: "OK\r\n"  ✓
5. modem state = CONNECTING
6. bridge에서 CONNECTING 감지
7. telnet_connect() 수행
8. modem_go_online() 호출
9. Client ← Serial: "CONNECT 57600\r\n"
10. 데이터 전송 시작 (버퍼 관리 개선 필요)
```

### 종료 시나리오 (현재 구현)
```
A. ATH 명령:
   1. Client → Serial: "ATH\r"
   2. modem_hangup() 호출
   3. modem state = DISCONNECTED
   4. Client ← Serial: "OK\r\n"
   5. telnet 연결은 그대로 유지됨  ❌

B. +++ escape:
   1. Client → Serial: "+++" (guard time 내)
   2. modem_go_offline() 호출
   3. modem state = COMMAND (carrier 유지)
   4. Client ← Serial: "OK\r\n"
   5. telnet 연결 유지 (정상)
   6. 이후 "ATH\r" → 위 A 시나리오
```

## 권장 수정 우선순위

### 우선순위 1 (긴급):
1. **데이터 전송 로직 재설계**: bridge_process_serial_data()와 bridge_transfer_serial_to_telnet() 간 데이터 흐름 수정
2. **상태 변화 감지 개선**: CONNECTING, DISCONNECTED 상태 변화 시 bridge 동작 추가

### 우선순위 2 (중요):
3. **ATA 응답 수정**: OK 응답 전송 후 연결 처리
4. **ATH 처리 개선**: bridge 상태 동기화

### 우선순위 3 (개선):
5. Escape sequence 후 데이터 처리 명확화
6. 초기 OK 메시지 제거
7. carrier 상태 관리 정책 결정

## 테스트 권장 사항

1. **ATA 명령 테스트**:
   - 정확한 응답 시퀀스 확인 (OK → CONNECT)
   - 첫 데이터 손실 여부 확인

2. **ATH 명령 테스트**:
   - telnet 연결 종료 여부
   - NO CARRIER 전송 여부

3. **Escape sequence 테스트**:
   - +++ 후 ATO 명령으로 복귀
   - +++ 후 ATH로 완전 종료

4. **데이터 전송 테스트**:
   - 대용량 데이터 전송 시 손실 확인
   - 멀티바이트 문자 경계 처리 확인

## 결론

현재 구현은 기본 구조는 양호하나, **데이터 흐름 관리**와 **상태 동기화**에 중요한 결함이 있습니다. 특히 온라인 모드에서의 데이터 손실 가능성이 높으므로 우선적으로 수정이 필요합니다.
