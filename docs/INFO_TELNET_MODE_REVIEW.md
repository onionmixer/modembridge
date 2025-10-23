# Telnet Line Mode vs Character Mode 지원 검토 보고서

## 검토 일자
2025년 10월 15일

## 요약

현재 ModemBridge의 telnet 구현은 **기본적인 프로토콜 처리는 정상**이지만, **Line mode와 Character mode 구분 및 적절한 모드 전환에 중대한 결함**이 있습니다.

## Telnet Mode 개념

### Character Mode (Raw Mode)
서버가 각 키스트로크를 즉시 처리하는 모드:
- **Server WILL ECHO**: 서버가 에코 담당
- **Server WILL SGA**: Go-Ahead 신호 억제
- **사용 예**: MUD, 실시간 게임, 일부 BBS, Unix 쉘

### Line Mode (Canonical Mode)
클라이언트가 라인 편집을 수행하고 엔터 시 전송하는 모드:
- **Server WONT ECHO** (또는 협상 없음): 클라이언트가 로컬 에코
- **클라이언트가 라인 버퍼링**: CR/LF까지 편집 가능
- **사용 예**: 일부 BBS, 전통적인 라인 기반 시스템

## 발견된 문제점

### 1. 심각: linemode 플래그가 업데이트되지 않음

**위치**: `src/telnet.c:190-276` (`telnet_handle_negotiate()`)

**문제**:
```c
case TELNET_WILL:
    if (option == TELOPT_BINARY || option == TELOPT_SGA || option == TELOPT_ECHO) {
        tn->remote_options[option] = true;
        telnet_send_negotiate(tn, TELNET_DO, option);

        if (option == TELOPT_BINARY) {
            tn->binary_mode = true;
        } else if (option == TELOPT_SGA) {
            tn->sga_mode = true;
        } else if (option == TELOPT_ECHO) {
            tn->echo_mode = true;  // ✓ 플래그만 설정
            // ✗ linemode 플래그를 업데이트하지 않음!
        }
    }
    break;
```

**결과**:
- `tn->linemode` 플래그는 초기화되지 않아 항상 `false` (0)
- `telnet_is_linemode()` 함수가 의미 없는 값 반환
- ECHO와 SGA 옵션 조합으로 character mode를 감지해야 하는데 하지 않음

**권장 수정**:
```c
// telnet_handle_negotiate() 함수 끝에 추가
static void telnet_update_mode(telnet_t *tn)
{
    /* Character mode: Server echoes (WILL ECHO) and SGA enabled
     * Line mode: Client echoes (WONT ECHO) or no echo negotiation */
    if (tn->remote_options[TELOPT_ECHO] && tn->remote_options[TELOPT_SGA]) {
        /* Character mode - server handles echo and SGA */
        tn->linemode = false;
        MB_LOG_INFO("Telnet mode: CHARACTER MODE (server echo)");
    } else {
        /* Line mode - client handles echo */
        tn->linemode = true;
        MB_LOG_INFO("Telnet mode: LINE MODE (client echo)");
    }
}

// telnet_handle_negotiate()의 각 case 끝에서 호출
case TELNET_WILL:
    // ... 기존 코드 ...
    telnet_update_mode(tn);
    break;

case TELNET_WONT:
    // ... 기존 코드 ...
    telnet_update_mode(tn);
    break;
```

### 2. 중간: 초기 옵션 협상에 ECHO 없음

**위치**: `src/telnet.c:98-102` (`telnet_connect()`)

**문제**:
```c
/* Send initial option negotiations */
telnet_send_negotiate(tn, TELNET_WILL, TELOPT_BINARY);
telnet_send_negotiate(tn, TELNET_WILL, TELOPT_SGA);
telnet_send_negotiate(tn, TELNET_DO, TELOPT_SGA);
// ✗ ECHO에 대한 협상이 없음
```

**결과**:
- 일부 서버는 클라이언트가 먼저 ECHO 관련 신호를 보내야 character mode로 전환
- 클라이언트 의도를 서버에 명확히 전달하지 못함
- 서버가 먼저 WILL ECHO를 보내지 않으면 character mode로 전환 안 될 수 있음

**권장 수정**:
```c
/* Send initial option negotiations */
telnet_send_negotiate(tn, TELNET_WILL, TELOPT_BINARY);
telnet_send_negotiate(tn, TELNET_WILL, TELOPT_SGA);
telnet_send_negotiate(tn, TELNET_DO, TELOPT_SGA);
telnet_send_negotiate(tn, TELNET_DO, TELOPT_ECHO);  // 추가: ECHO 지원 의사 표시
```

### 3. 경미: telnet_init()에서 linemode 초기화 누락

**위치**: `src/telnet.c:10-30` (`telnet_init()`)

**문제**:
```c
void telnet_init(telnet_t *tn)
{
    // ...
    memset(tn, 0, sizeof(telnet_t));
    tn->fd = -1;
    tn->is_connected = false;
    tn->state = TELNET_STATE_DATA;
    // ✗ linemode 명시적 초기화 없음 (memset으로 0이 되지만 의도 불명확)
}
```

**권장 수정**:
```c
void telnet_init(telnet_t *tn)
{
    // ...
    memset(tn, 0, sizeof(telnet_t));
    tn->fd = -1;
    tn->is_connected = false;
    tn->state = TELNET_STATE_DATA;

    /* Default to line mode until server requests character mode */
    tn->linemode = true;  // 추가: 명시적 초기화
}
```

### 4. 정보: ModemBridge의 특수한 상황

**ModemBridge의 동작 방식**:
```
Terminal/BBS Client → Serial Port → ModemBridge → Telnet → Server
                     (Raw mode)                  (Protocol)
```

**실제 동작**:
- Serial port는 raw mode로 설정 (termios ICANON off)
- 클라이언트가 타이핑한 각 문자가 즉시 serial port로 전송됨
- ModemBridge는 받은 문자를 즉시 telnet으로 전달
- **결과적으로 character mode처럼 동작**

**그러나 문제점**:
1. **에코 처리 불명확**:
   - Server WILL ECHO → 서버가 에코 전송
   - Serial 클라이언트(터미널 프로그램)가 로컬 에코를 해야 하는지 알 수 없음
   - 현재는 클라이언트 설정에만 의존 (ATE1/ATE0)

2. **이중 에코 가능성**:
   - Server WILL ECHO + 클라이언트 ATE1 → 이중 에코 발생
   - 각 타이핑이 두 번 표시됨

3. **모뎀 에뮬레이션 로직과의 통합 필요**:
   - `modem_t` 구조체의 `settings.echo`와 telnet `echo_mode` 동기화 필요

### 5. 중간: 모뎀 에코와 텔넷 에코 동기화 부재

**위치**: `include/modem.h`, `src/bridge.c`

**문제**:
- 모뎀 에뮬레이터는 `ATE0/ATE1` 명령으로 에코 설정 가능
- 텔넷 서버가 `WILL ECHO`를 보내면 서버가 에코 담당
- 이 두 설정이 충돌할 수 있음

**시나리오**:
```
1. 사용자: ATE1 (모뎀 에코 ON)
2. 서버: IAC WILL ECHO (서버 에코 ON)
3. 결과: 이중 에코 (모뎀이 한 번, 서버가 한 번)
```

**권장 동작**:
```c
// bridge_handle_modem_connect() 또는 새로운 함수에서:
if (telnet_is_connected(&ctx->telnet)) {
    if (ctx->telnet.remote_options[TELOPT_ECHO]) {
        /* Server will echo - disable modem echo to prevent double echo */
        if (ctx->modem.settings.echo) {
            MB_LOG_INFO("Server WILL ECHO, disabling modem local echo");
            ctx->modem.settings.echo = false;

            /* Optionally notify user */
            modem_send_response_fmt(&ctx->modem,
                "Server echo mode - local echo disabled");
        }
    } else {
        /* Server won't echo - use modem echo setting (from ATE command) */
        MB_LOG_INFO("Server WONT ECHO, using modem echo setting (ATE)");
    }
}
```

## 테스트 시나리오

### Character Mode 서버 테스트 (예: MUD)

**예상 협상 순서**:
```
Client → Server: IAC WILL BINARY
Client → Server: IAC WILL SGA
Client → Server: IAC DO SGA
Client → Server: IAC DO ECHO

Server → Client: IAC DO BINARY
Server → Client: IAC WILL ECHO   ← Character mode 시작
Server → Client: IAC WILL SGA
Server → Client: IAC DO SGA

Expected: linemode = false
```

**현재 동작**:
```
✓ BINARY, SGA 협상 성공
✓ ECHO 협상 수락
✗ linemode 플래그 업데이트 안 됨 (항상 false)
✗ 모뎀 에코 설정과 충돌 가능성
```

### Line Mode 서버 테스트 (예: 일부 BBS)

**예상 협상 순서**:
```
Client → Server: IAC WILL BINARY
Client → Server: IAC WILL SGA
Client → Server: IAC DO SGA

Server → Client: IAC DO BINARY
Server → Client: IAC WONT ECHO   ← Line mode (또는 ECHO 협상 없음)
Server → Client: IAC WILL SGA

Expected: linemode = true
```

**현재 동작**:
```
✓ BINARY, SGA 협상 성공
✓ WONT ECHO 처리
✗ linemode 플래그 업데이트 안 됨
✓ 실제로는 raw serial로 character-by-character 전송됨
```

## 실제 영향 평가

### 현재 구현으로 동작하는 경우:
1. **Raw serial 특성으로 인한 우연한 호환성**:
   - Serial port가 raw mode라서 각 문자가 즉시 전송됨
   - Character mode 서버도 대부분 정상 동작할 것

2. **이중 에코가 발생하지 않는 경우**:
   - 사용자가 `ATE0` (에코 OFF) 설정
   - 또는 터미널 프로그램의 로컬 에코 OFF

### 문제가 발생할 수 있는 경우:

1. **이중 에코**:
   - Server WILL ECHO + Modem ATE1
   - 각 타이핑이 두 번 표시됨
   - 사용자 경험 저해

2. **에코 없음**:
   - Server WONT ECHO + Modem ATE0
   - 타이핑한 내용이 화면에 안 보임
   - 비밀번호 입력 외에는 문제

3. **서버가 모드 전환을 요구하는 경우**:
   - 일부 서버는 동적으로 ECHO 모드 변경
   - 예: 로그인 시 line mode, 게임 중 character mode
   - 현재 구현은 이를 추적하지 못함

## 권장 수정 우선순위

### 우선순위 1 (중요):
1. **linemode 플래그 업데이트 로직 추가**
   - `telnet_update_mode()` 함수 구현
   - ECHO와 SGA 옵션 조합으로 모드 판별
   - 모드 변경 시 로그 출력

2. **모뎀 에코와 텔넷 에코 동기화**
   - Server WILL ECHO 시 모뎀 로컬 에코 자동 비활성화
   - Server WONT ECHO 시 모뎀 에코 설정 유지
   - 사용자에게 모드 변경 알림 (선택적)

### 우선순위 2 (개선):
3. **초기 ECHO 협상 추가**
   - `telnet_connect()`에 `IAC DO ECHO` 추가
   - 서버에 character mode 지원 의사 명확히 전달

4. **linemode 명시적 초기화**
   - `telnet_init()`에서 `linemode = true` 설정
   - 기본값이 line mode임을 명확히 표시

### 우선순위 3 (향후):
5. **동적 모드 전환 지원**
   - 서버가 실행 중 WILL/WONT ECHO 변경 시 대응
   - 모뎀 에코 설정 자동 조정
   - 사용자 알림

6. **LINEMODE 옵션 (TELOPT 34) 지원**
   - RFC 1184 준수
   - 더 정교한 line mode 협상

## 코드 수정 예시

### 수정 1: telnet_update_mode() 함수 추가

```c
/**
 * Update line mode vs character mode based on current options
 */
static void telnet_update_mode(telnet_t *tn)
{
    bool old_linemode = tn->linemode;

    /* Character mode: Server echoes (WILL ECHO) and SGA enabled
     * Line mode: Client echoes (WONT ECHO) or no echo negotiation */
    if (tn->remote_options[TELOPT_ECHO] && tn->remote_options[TELOPT_SGA]) {
        /* Character mode - server handles echo */
        tn->linemode = false;
        if (old_linemode != tn->linemode) {
            MB_LOG_INFO("Telnet mode changed to CHARACTER MODE (server echo, SGA enabled)");
        }
    } else {
        /* Line mode - client handles echo */
        tn->linemode = true;
        if (old_linemode != tn->linemode) {
            MB_LOG_INFO("Telnet mode changed to LINE MODE (client echo)");
        }
    }
}
```

### 수정 2: telnet_handle_negotiate() 업데이트

```c
int telnet_handle_negotiate(telnet_t *tn, unsigned char command, unsigned char option)
{
    // ... 기존 코드 유지 ...

    switch (command) {
        case TELNET_WILL:
            // ... 기존 처리 ...
            telnet_update_mode(tn);  // 추가
            break;

        case TELNET_WONT:
            // ... 기존 처리 ...
            telnet_update_mode(tn);  // 추가
            break;

        // ... 나머지 case ...
    }

    return SUCCESS;
}
```

### 수정 3: bridge에서 에코 동기화

```c
/**
 * Synchronize modem echo with telnet echo mode
 */
static void bridge_sync_echo_mode(bridge_ctx_t *ctx)
{
    if (!telnet_is_connected(&ctx->telnet)) {
        return;
    }

    /* If server will echo, disable modem local echo to prevent double echo */
    if (ctx->telnet.remote_options[TELOPT_ECHO]) {
        if (ctx->modem.settings.echo) {
            MB_LOG_INFO("Server WILL ECHO - disabling modem local echo to prevent double echo");
            ctx->modem.settings.echo = false;
        }
    }
    /* If server won't echo, keep modem echo setting (from ATE command) */
    else {
        MB_LOG_INFO("Server WONT ECHO - using modem echo setting (ATE command)");
    }
}

// bridge_handle_modem_connect()에서 호출:
int bridge_handle_modem_connect(bridge_ctx_t *ctx)
{
    // ... 기존 코드 ...

    /* Connect to telnet server */
    ret = telnet_connect(&ctx->telnet, ctx->config->telnet_host, ctx->config->telnet_port);
    if (ret != SUCCESS) {
        // ... 에러 처리 ...
    }

    /* Wait a bit for initial telnet negotiations */
    usleep(100000);  // 100ms - allows time for WILL ECHO negotiation

    /* Synchronize echo settings */
    bridge_sync_echo_mode(ctx);

    // ... 나머지 코드 ...
}
```

## 결론

현재 ModemBridge의 telnet 구현은:

1. **프로토콜 처리 자체는 정상**: IAC, WILL/WONT/DO/DONT, 옵션 협상 모두 정상 동작
2. **Raw serial의 특성으로 우연히 호환**: Character-by-character 전송은 동작
3. **Mode 감지 및 관리 부재**: Line mode vs character mode 구분 안 됨
4. **에코 동기화 부재**: 모뎀 에코와 텔넷 에코 충돌 가능성
5. **대부분의 서버에서 동작 가능**: 하지만 이중 에코 문제 발생 가능

**권장 조치**:
- 우선순위 1, 2 수정 사항 적용
- 다양한 telnet 서버(MUD, BBS, Unix 쉘)로 테스트
- 이중 에코 발생 여부 확인
- 동적 모드 전환 테스트

**테스트 추천 서버**:
- MUD: `discworld.starturtle.net:4242` (character mode)
- BBS: `bbs.fozztexx.com:23` (mixed mode)
- Unix shell: `telehack.com:23` (line mode)
