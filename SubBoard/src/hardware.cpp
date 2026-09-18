/**
 * 子板硬件层：初始化、中断、串口环形缓冲、TB6612 发牌电机驱动，
 * 摄像头截图触发/回传解析（OpenMV 文本行）+ 指令下发（校准），光电门轮询去抖，
 * 以及开机自检（busy_self_test）。
 * 协议与时序见 docs/subboard_architecture.md、docs/camera_protocol.md。
 */
#include <Arduino.h>
#include <stdlib.h>
#include <string.h>

#include "pins_config.h"
#include "app_config.h"
#include "protocol.h"
#include "hardware.h"
#include "debug.h"

// ---- TB6612 发牌电机 PWM（20kHz，8bit，与 test1 调试台一致）----
#define MOTOR_PWM_CH      0
#define MOTOR_PWM_FREQ    20000
#define MOTOR_PWM_BITS    8

// ---- 串口接收环形缓冲（架构第四章：中断写入，主循环解析）----
static volatile uint8_t s_rx_ring[UART_RX_RING_SIZE];
static volatile uint16_t s_rx_head = 0;   // 写入位置
static volatile uint16_t s_rx_tail = 0;   // 读取位置

static void ring_write(uint8_t b) {
    uint16_t next = (s_rx_head + 1) % UART_RX_RING_SIZE;
    if (next == s_rx_tail) return;        // 满则丢弃（协议层会因帧不完整而丢弃）
    s_rx_ring[s_rx_head] = b;
    s_rx_head = next;
}

// 串口接收中断回调（Arduino-ESP32 v3：Serial1.onReceive）
void sub_uart_rx_isr(void) {
    while (Serial1.available() > 0) {
        ring_write((uint8_t)Serial1.read());
    }
}

bool sub_uart_read_byte(uint8_t *b) {
    if (s_rx_tail == s_rx_head) return false;
    *b = s_rx_ring[s_rx_tail];
    s_rx_tail = (s_rx_tail + 1) % UART_RX_RING_SIZE;
    return true;
}

// ---- 光电门（原来的光敏传感器）：有牌 = 低电平(GND)，无牌 = 高 ----
// 主循环轮询 + 去抖（不用中断：需要“该电平已稳定多久”这类判断，中断只能给边沿）。
static uint8_t  s_photo_level = 1;     // 去抖后的电平：1 = 无牌，0 = 有牌
static uint8_t  s_photo_raw = 1;       // 原始电平
static uint32_t s_photo_raw_ms = 0;    // 原始电平最近一次变化的时刻
static uint32_t s_photo_level_ms = 0;  // 去抖后电平最近一次变化的时刻

void sub_photo_init(void) {
    pinMode(PIN_PHOTO, INPUT_PULLUP);
    uint8_t raw = (digitalRead(PIN_PHOTO) == LOW) ? 0 : 1;
    s_photo_raw = s_photo_level = raw;
    s_photo_raw_ms = s_photo_level_ms = millis();
}

void sub_photo_update(void) {
    uint8_t raw = (digitalRead(PIN_PHOTO) == LOW) ? 0 : 1;
    uint32_t now = millis();

    if (raw != s_photo_raw) {            // 原始电平变化：重新计时
        s_photo_raw = raw;
        s_photo_raw_ms = now;
        return;
    }
    if ((uint32_t)(now - s_photo_raw_ms) < PHOTO_DEBOUNCE_MS) return;   // 尚未稳定
    if (raw == s_photo_level) return;    // 已认可，无变化

    s_photo_level = raw;                 // 认可新电平
    s_photo_level_ms = now;
    dbg_printf("[PG] %s\n", (raw == 0) ? "card present" : "card gone");
}

bool sub_photo_present(void) { return s_photo_level == 0; }

uint32_t sub_photo_stable_ms(void) {
    return (uint32_t)(millis() - s_photo_level_ms);
}

void sub_hardware_init(void) {
    Serial.begin(115200);                 // 调试串口

    // 发牌电机（TB6612FNG：PWM + AIN1/AIN2 + STBY）
    pinMode(PIN_MOTOR_PWM, OUTPUT);
    pinMode(PIN_MOTOR_AIN1, OUTPUT);
    pinMode(PIN_MOTOR_AIN2, OUTPUT);
    pinMode(PIN_MOTOR_STBY, OUTPUT);
    // 待机：STBY 拉低 = 输出关断（TB6612 待机电流 µA 级，见 app_config.h 低功耗一节）。
    // 要驱动时由 busy_motor_* 先拉高使能，停稳后 busy_motor_stop() 再拉回低。
    digitalWrite(PIN_MOTOR_STBY, LOW);
    digitalWrite(PIN_MOTOR_AIN1, LOW);
    digitalWrite(PIN_MOTOR_AIN2, LOW);
    ledcSetup(MOTOR_PWM_CH, MOTOR_PWM_FREQ, MOTOR_PWM_BITS);
    ledcAttachPin(PIN_MOTOR_PWM, MOTOR_PWM_CH);
    ledcWrite(MOTOR_PWM_CH, 0);

    // 光电门（有牌 = 低电平，无牌 = 高）
    sub_photo_init();

    // 摄像头：这里的初始化已挪到 sub_camera_trigger()（首次触发时才配置）。
    // 原因：TRIG 只在真正截图时才有用，平时保持高阻，免得开机就给摄像头发假触发。
    //       （TRIG 当前为 GPIO35，见 pins_config.h。）

    // 与底板通信串口（2 线 UART）
    Serial1.begin(SUB_UART_BAUD, SERIAL_8N1, SUB_UART_RX_PIN, SUB_UART_TX_PIN);
    Serial1.onReceive(sub_uart_rx_isr);   // 中断接收 → 环形缓冲

    // 打印当前固件使用的引脚（核对实际接线用）
    dbg_println("[SUB] ---- pin config ----");
    dbg_printf("[SUB] motor: PWMA=%d AIN1=%d AIN2=%d STBY=%d\n",
                  PIN_MOTOR_PWM, PIN_MOTOR_AIN1, PIN_MOTOR_AIN2, PIN_MOTOR_STBY);
    dbg_printf("[SUB] uart : RX=%d TX=%d (POS=%d NEG=%d)\n",
                  SUB_UART_RX_PIN, SUB_UART_TX_PIN, PIN_UART_POS, PIN_UART_NEG);
    dbg_printf("[SUB] photo: %d\n", PIN_PHOTO);
}

void sub_self_test(void) {
    // 自检（光电门电平 / 摄像头校准 / 电机微动 / 与底板串口）→ 上报 EVT_READY（架构第八章）
    // EVT_READY 的 data = 2 字节结果位图（SUB_ST_*，见 protocol.h）；旧底板按 len=0 兼容处理
    busy_self_test();
    uint16_t b = sub_selftest_bits();
    uint8_t d[2] = { (uint8_t)(b & 0xFF), (uint8_t)((b >> 8) & 0xFF) };
    proto_send(EVT_READY, d, sizeof(d));
}

// ================= 摄像头：截图触发 + 回传接收 =================
//
// 工作方式：子板不主动取图，只负责“把 TRIG 拉低（下降沿）→ 等摄像头把识别结果发回来”。
//
// 【摄像头 → 子板：OpenMV 原生 ASCII 文本行】（详见 docs/camera_protocol.md）
//   每条结果一行，以 \r\n 结束：RESULT:<结果>
//     普通牌   RESULT:heart_A / RESULT:club_10 / RESULT:spade_K
//     特殊     RESULT:joker_big / RESULT:joker_small / RESULT:back
//              RESULT:UNKNOWN（置信度不足） / RESULT:ERROR（摄像头异常）
//   子板把文本翻译成内部牌面编码（见 protocol.h），再用板间二进制帧发给底板。
//
// 【触发极性】OpenMV 的 P6 是**下降沿**触发：空闲高、拉低 ≥5ms 再拉高。
//
// 接收路径：截图等待窗口内，主循环 sub_camera_service() 轮询 Serial2 并解析（见下）。
// 结果去向：解析成功后发 EVT_CARD_VALUE（data = 识别载荷）；超时未回传时按 CAM_EMPTY_ON_TIMEOUT 处理。
//
// 【子板 → 摄像头：指令下发】反方向用同一根 UART（PIN_CAM_TX → 摄像头 RX），
//   格式与回传同风格：ASCII 文本行 + \r\n 结尾；当前只用于开机自检的校准指令（见下）。

// 接收方式：在截图等待窗口内轮询 Serial2（不用 onReceive 中断）。
// 原因：摄像头未接时 RX 引脚浮空，中断会被噪声反复触发；轮询只在等待窗口读，
//       天然规避这个问题。等摄像头接线确认后如需中断可再加回。

#define CAM_LINE_MAX  40           // 单行最大长度（"RESULT:diamond_10" 约 18）

// 截图会话状态：IDLE → PULSE（TRIG 拉低，触发中）→ WAIT（等回传）→ IDLE
typedef enum { CAM_IDLE = 0, CAM_PULSE, CAM_WAIT } cam_session_t;
static cam_session_t s_cam_state = CAM_IDLE;
static uint32_t      s_cam_t0 = 0;
static uint8_t       s_cam_payload[2];        // [card, src]
static uint8_t       s_cam_payload_len = 0;
static bool          s_cam_got_result = false;
static bool          s_cam_cam_error = false; // 收到 RESULT:ERROR
static bool          s_cam_uart_inited = false;  // Serial2 是否已初始化
static bool          s_cam_trig_inited = false;  // PIN_CAM_TRIG 是否已配置成输出
static char          s_cam_line[CAM_LINE_MAX];
static uint8_t       s_cam_line_len = 0;
static char          s_cam_last_line[CAM_LINE_MAX];  // 最后一条完整行原文（串口直接打印用）

// ---- 摄像头链路初始化（拆成两半，谁用谁初始化）----
// UART：发指令与收回传都要用，可以单独提前初始化。
static void cam_ensure_uart(void) {
    if (s_cam_uart_inited) return;
    s_cam_uart_inited = true;
    Serial2.begin(CAM_UART_BAUD, SERIAL_8N1, PIN_CAM_RX, PIN_CAM_TX);
}

// TRIG 脚：只在真正要截图时才配置成输出。
// 原因：浪费一个输出没有意义，而且开机就配成输出可能给摄像头发一个假触发。
// 所以开机自检里**不碰**这个脚——发校准指令只需要 UART。
static void cam_ensure_trig(void) {
    if (s_cam_trig_inited) return;
    s_cam_trig_inited = true;
    pinMode(PIN_CAM_TRIG, OUTPUT);
    digitalWrite(PIN_CAM_TRIG, HIGH);     // 空闲高电平（OpenMV P6 下降沿触发）
}

void sub_camera_trigger(void) {
    if (s_cam_state != CAM_IDLE) {
        // 上一次会话还没结束：这一次截图没有执行，必须明确告诉底板，
        // 不能让 ACK 造成"命令已生效"的错觉（否则底板会白等一个不会来的结果）。
        dbg_println("[CAM] trigger rejected (session busy) -> report CAM fail");
        proto_send(EVT_ERROR_CAM_FAIL, NULL, 0);
        return;
    }
    cam_ensure_uart();
    cam_ensure_trig();                        // 首次触发才把 PIN_CAM_TRIG 配成输出
    // 清掉上一次残留：迟到的旧结果不能被当成这一次的识别结果
    while (Serial2.available() > 0) (void)Serial2.read();

    s_cam_got_result = false;
    s_cam_cam_error = false;
    s_cam_payload_len = 0;
    s_cam_line_len = 0;
    s_cam_t0 = millis();
    s_cam_state = CAM_PULSE;
    digitalWrite(PIN_CAM_TRIG, LOW);          // 下降沿 → 触发拍照
    dbg_println("[CAM] TRIG low (trigger)");
}

// 终止当前截图会话（底板 CMD_STOP / CMD_RESET 时调用）。
// 为什么需要：复位/停机只复位状态机和电机的，如果这里不清会话，
// 摄像头迟到的 RESULT 仍会被 sub_camera_service() 解析成 EVT_CARD_VALUE 发上去，
// 而那已经和下一局无关了（下一局的等待方会把它当成自己的牌面）。
// 动作：TRIG 恢复空闲高（下次触发才有正确的下降沿）、丢掉 Serial2 残留、会话状态复位。
void sub_camera_cancel(void) {
    if (!s_cam_uart_inited && !s_cam_trig_inited) return;   // 摄像头链路还没用过，没什么可取消
    bool wasBusy = (s_cam_state != CAM_IDLE);

    if (s_cam_trig_inited) digitalWrite(PIN_CAM_TRIG, HIGH);  // 回空闲高，等下次下降沿
    if (s_cam_uart_inited) {
        while (Serial2.available() > 0) (void)Serial2.read();
    }
    s_cam_state       = CAM_IDLE;
    s_cam_got_result  = false;
    s_cam_cam_error   = false;
    s_cam_payload_len = 0;
    s_cam_line_len    = 0;

    if (wasBusy) dbg_println("[CAM] session cancelled by STOP/RESET");
}

// ---- 子板 → 摄像头：下发一行文本指令（ASCII + CRLF，与 RESULT: 回传同风格）----
static void cam_send_line(const char *line) {
    cam_ensure_uart();
    // 先清掉残留，避免把上一条旧行当成这次指令的回应
    while (Serial2.available() > 0) (void)Serial2.read();

    size_t n = strlen(line);
    Serial2.write((const uint8_t *)line, n);
    Serial2.flush();                     // 等字节真正发出去（一行很短，阻塞可忽略）

    dbg_printf("[CAM] TX cmd (%u bytes):", (unsigned)n);
    for (size_t i = 0; i < n; i++) dbg_printf(" %02X", (unsigned char)line[i]);
    dbg_println();                       // 十六进制便于核对结尾是不是 0D 0A（CRLF）
}

// 摄像头校准（开机自检用）：先发校准指令，再等摄像头回应。
// 返回 true = 收到回应且不是 RESULT:ERROR。
// 说明：OpenMV 端 main_standalone.py 目前不读 UART，因此“无回应”在摄像头改代码前属正常现象，
//       自检只把它记为“未回应”，不当成致命错误。
bool sub_camera_calibrate(uint32_t waitMs) {
    // 若上一次截图会话还没结束（比如触发后一直没等到结果），先取消：
    // 否则校准的回应会和"截图结果"混在一起，分不清是谁回的。
    if (s_cam_state != CAM_IDLE) sub_camera_cancel();

    dbg_println("[CAM] calibrate: send command, then wait for reply...");
    cam_send_line(CAM_CMD_CALIBRATE);

    char     line[CAM_LINE_MAX];
    uint8_t  n = 0;
    bool     got = false;
    bool     camErr = false;

    uint32_t t0 = millis();
    while ((uint32_t)(millis() - t0) < waitMs) {
        while (Serial2.available() > 0) {
            char c = (char)Serial2.read();
            if (c == '\n' || c == '\r') {
                if (n > 0) {                       // 收到整行（去掉 \r\n）
                    line[n] = '\0';
                    n = 0;
                    dbg_printf("[CAM] calib rx: %s\n", line);
                    if (strncmp(line, "RESULT:ERROR", 12) == 0) camErr = true;
                    else                                        got = true;
                }
            } else if (n < CAM_LINE_MAX - 1) {
                line[n++] = c;
            }
        }
        delay(2);
    }

    if (camErr)          dbg_println("[CAM] calibrate: camera reported RESULT:ERROR");
    else if (!got)       dbg_println("[CAM] calibrate: no reply (OpenMV 端尚未实现 UART 命令接收)");
    else                 dbg_println("[CAM] calibrate: reply received");
    return got && !camErr;
}

// OpenMV 文本结果 → 牌面编码。成功返回 true（card/src 已填好）。
static bool cam_text_to_card(const char *s, uint8_t *card, uint8_t *src) {
    if (strcmp(s, "joker_big") == 0)   { *card = CARD_JOKER_BIG;   *src = CARD_SRC_CAMERA;  return true; }
    if (strcmp(s, "joker_small") == 0) { *card = CARD_JOKER_SMALL; *src = CARD_SRC_CAMERA;  return true; }
    if (strcmp(s, "back") == 0)        { *card = CARD_BACK;        *src = CARD_SRC_CAMERA;  return true; }
    if (strcmp(s, "UNKNOWN") == 0)     { *card = CARD_UNKNOWN;     *src = CARD_SRC_LOWCONF; return true; }

    // 普通牌：<花色>_<点数>，例 spade_A / club_10 / diamond_K
    const char *us = strchr(s, '_');
    if (!us) return false;

    size_t n = (size_t)(us - s);
    uint8_t suit;
    if      (n == 5 && strncmp(s, "spade",   5) == 0) suit = CARD_SUIT_SPADE;
    else if (n == 5 && strncmp(s, "heart",   5) == 0) suit = CARD_SUIT_HEART;
    else if (n == 4 && strncmp(s, "club",    4) == 0) suit = CARD_SUIT_CLUB;
    else if (n == 7 && strncmp(s, "diamond", 7) == 0) suit = CARD_SUIT_DIAMOND;
    else return false;

    const char *r = us + 1;
    int8_t rank = -1;
    if      (strcmp(r, "A") == 0) rank = CARD_RANK_A;
    else if (strcmp(r, "J") == 0) rank = CARD_RANK_J;
    else if (strcmp(r, "Q") == 0) rank = CARD_RANK_Q;
    else if (strcmp(r, "K") == 0) rank = CARD_RANK_K;
    else {
        int v = atoi(r);
        if (v >= 2 && v <= 10) rank = (int8_t)(v - 1);   // 2→1 … 10→9
    }
    if (rank < 0) return false;

    *card = (uint8_t)CARD_CODE(suit, rank);
    *src  = CARD_SRC_CAMERA;
    return true;
}

// 按行解析 Serial2（轮询式）：识别 "RESULT:<值>" 并翻译成牌面编码。
// 成功返回 true；收到 RESULT:ERROR 时置 s_cam_cam_error 并返回 true。
static bool cam_parse(void) {
    while (Serial2.available() > 0) {
        char c = (char)Serial2.read();

        if (c != '\n' && c != '\r') {
            if (s_cam_line_len < CAM_LINE_MAX - 1) s_cam_line[s_cam_line_len++] = c;
            else s_cam_line_len = 0;          // 行太长：丢弃重来
            continue;
        }
        if (s_cam_line_len == 0) continue;    // 空行
        s_cam_line[s_cam_line_len] = '\0';
        s_cam_line_len = 0;

        // 留一份原文：上报时直接把摄像头发来的文本打出来（解码结果只在协议层用）
        strncpy(s_cam_last_line, s_cam_line, sizeof(s_cam_last_line) - 1);
        s_cam_last_line[sizeof(s_cam_last_line) - 1] = '\0';

        const char *prefix = "RESULT:";
        if (strncmp(s_cam_line, prefix, 7) != 0) {
            dbg_printf("[CAM] ignore line: %s\n", s_cam_line);
            continue;
        }
        const char *val = s_cam_line + 7;

        if (strcmp(val, "ERROR") == 0) {      // 摄像头自身异常
            s_cam_cam_error = true;
            return true;
        }
        uint8_t card = CARD_UNKNOWN, src = CARD_SRC_CAMERA;
        if (!cam_text_to_card(val, &card, &src)) {
            dbg_printf("[CAM] cannot parse: %s\n", val);
            continue;
        }
        s_cam_payload[0] = card;
        s_cam_payload[1] = src;
        s_cam_payload_len = 2;
        return true;
    }
    return false;
}

void sub_camera_service(void) {
    uint32_t now = millis();
    switch (s_cam_state) {
    case CAM_IDLE:
        break;

    case CAM_PULSE:
        if (now - s_cam_t0 >= CAM_TRIG_PULSE_MS) {
            digitalWrite(PIN_CAM_TRIG, HIGH);         // 恢复空闲高（≥5ms 后才允许下次触发）
            s_cam_state = CAM_WAIT;
            s_cam_t0 = now;
            dbg_println("[CAM] TRIG high, waiting result...");
        }
        break;

    case CAM_WAIT:
        if (!s_cam_got_result && cam_parse()) {
            s_cam_got_result = true;
            if (s_cam_cam_error) {
                dbg_printf("[CAM] %s -> report CAM fail\n", s_cam_last_line);
                proto_send(EVT_ERROR_CAM_FAIL, NULL, 0);
            } else {
                // 直接回显摄像头原文（不打印解码后的 card/src，便于对着摄像头端排查）
                dbg_printf("[CAM] %s\n", s_cam_last_line);
                proto_send(EVT_CARD_VALUE, s_cam_payload, s_cam_payload_len);
            }
            s_cam_state = CAM_IDLE;
        } else if (now - s_cam_t0 >= CAM_RESULT_TIMEOUT_MS) {
            // 摄像头未回传：保留“发空牌”调试路径，让整条流程还能跑通
#if CAM_EMPTY_ON_TIMEOUT
            uint8_t d[2] = { CARD_UNKNOWN, CARD_SRC_TIMEOUT };
            dbg_println("[CAM] timeout -> UNKNOWN src=TIMEOUT (debug)");
            proto_send(EVT_CARD_VALUE, d, sizeof(d));
#else
            dbg_println("[CAM] timeout -> report CAM fail");
            proto_send(EVT_ERROR_CAM_FAIL, NULL, 0);
#endif
            s_cam_state = CAM_IDLE;
        }
        break;
    }
}

// ================= 发牌电机驱动（TB6612）=================
void busy_motor_start(void) {
    // 正转出牌：AIN1=0, AIN2=1（实测转向相反，已对调）
    digitalWrite(PIN_MOTOR_STBY, HIGH);
    digitalWrite(PIN_MOTOR_AIN1, LOW);
    digitalWrite(PIN_MOTOR_AIN2, HIGH);
    ledcWrite(MOTOR_PWM_CH, MOTOR_DUTY);
}

void busy_motor_start_reverse(void) {
    // 反转回退：AIN1=1, AIN2=0（出牌后把下一张退到摄像头可拍位置）
    digitalWrite(PIN_MOTOR_STBY, HIGH);
    digitalWrite(PIN_MOTOR_AIN1, HIGH);
    digitalWrite(PIN_MOTOR_AIN2, LOW);
    ledcWrite(MOTOR_PWM_CH, MOTOR_REV_DUTY);
}

void busy_motor_brake(void) {
    // 短刹车：两端同高 + PWM=0，电机绕组短路制动，用于正转→反转过渡
    digitalWrite(PIN_MOTOR_STBY, HIGH);
    digitalWrite(PIN_MOTOR_AIN1, HIGH);
    digitalWrite(PIN_MOTOR_AIN2, HIGH);
    ledcWrite(MOTOR_PWM_CH, 0);
}

void busy_motor_stop(void) {
    // 停止 + 进待机：先 AIN1/2 全低、PWM=0（滑行自然停），再把 STBY 拉低关断输出。
    // 停机后驱动完全不受控于电机（TB6612 待机电流 µA 级）；下一次 busy_motor_* 会重新拉高 STBY。
    digitalWrite(PIN_MOTOR_AIN1, LOW);
    digitalWrite(PIN_MOTOR_AIN2, LOW);
    ledcWrite(MOTOR_PWM_CH, 0);
    digitalWrite(PIN_MOTOR_STBY, LOW);
}

// 电机自检：微动正转 SELFTEST_MOTOR_FWD_MS → 刹车 MOTOR_BRAKE_MS → 反转 SELFTEST_MOTOR_REV_MS → 停
// （开机自检与 mtest 命令用）
// 正转用 SELFTEST_MOTOR_FWD_MS 而**不用**出牌时间 MOTOR_FWD_MS：自检只抖一下，
// 避免真的把牌发出去；反转稍长，把可能被推出来的牌退回原位。
// 如果电机完全不转，说明引脚 / 驱动供电 / 接线有问题。
void motor_self_test(void) {
    // 先动电机再打印：即使调试串口异常也不会卡住自检动作
    busy_motor_start();
    dbg_printf("[MOT] self-test: forward %dms...\n", SELFTEST_MOTOR_FWD_MS);
    delay(SELFTEST_MOTOR_FWD_MS);
    if (MOTOR_BRAKE_MS > 0) {
        busy_motor_brake();
        dbg_println("[MOT] self-test: brake...");
        delay(MOTOR_BRAKE_MS);
    }
    busy_motor_start_reverse();
    dbg_printf("[MOT] self-test: reverse %dms...\n", SELFTEST_MOTOR_REV_MS);
    delay(SELFTEST_MOTOR_REV_MS);
    busy_motor_stop();
    dbg_println("[MOT] self-test done (motor should have twitched twice)");
}

// ================= 开机自检（子板，见 重要信息/自检流程方案.md）=================
// 2026-09-17 修订：**结果位图只收"固件自己能判定成败"的项目**。
//   以前电机微动、光电门静态电平、摄像头串口"初始化过"都无条件置 1，实际什么都没验，
//   报告里却显示 OK —— 这个假通过已经删掉：观察项只打印，不进位图。
//   现在留下的两项都能自动判定：
//     ① 摄像头校准：发出 CALIBRATE 且收到非 ERROR 回应（说明这条 UART 双向通）；
//     ② 与底板串口：自检期间收到过底板数据。
//   观察项（不进结果）：电机微动（听/看）、光电门静态电平（没接传感器时上拉也读高，判不出来）。
//   光电门的真实判定在发牌动作结束后的判定逻辑里（state_machine.cpp），不在自检里。
//   需要人配合的交互项（转编码器、手转电机试锁轴力矩）不做。
static uint16_t s_st_bits = 0;

uint16_t sub_selftest_bits(void) { return s_st_bits; }

void busy_self_test(void) {
    s_st_bits = 0;
    dbg_println("[ST] ------- sub self-test -------");
    dbg_println("[ST] rule: only auto-decidable items count; watch-only items are printed but not counted");

    // 0) 观察项：光电门静态电平。**判不出来**——没接传感器时 INPUT_PULLUP 也读高，
    //    与"出牌口没牌"完全一样，所以只打印、不置位。
    dbg_printf("[ST] photo gate : %s (PIN_PHOTO=%d) [watch only, not counted]\n",
               sub_photo_present() ? "CARD PRESENT (LOW)" : "clear (HIGH)", PIN_PHOTO);

    // 1) 摄像头校准：**可判定**。先发校准指令、再动电机——
    //    电机一转牌堆就错位，摄像头按当前画面做的校准就白做了；
    //    CAM_CALIB_WAIT_MS 既是等回应的时长，也保证校准排在电机前面。
    if (sub_camera_calibrate(CAM_CALIB_WAIT_MS)) s_st_bits |= SUB_ST_CAM_CALIB;

    // 2) 观察项：发牌电机微动（应看到/听到抖两下）。没有电流/转速反馈，固件判不了，不置位。
    motor_self_test();
    dbg_println("[ST] motor twitch: issued [watch only, not counted]");

    // 3) 与底板串口：**可判定**（自检期间是否已收到底板数据；底板自检会周期发 CMD_STATUS_QUERY）
    bool hostSeen = (s_rx_head != s_rx_tail);
    dbg_printf("[ST] host uart  : %s\n",
               hostSeen ? "data received" : "no data yet (底板可能还没启动)");
    if (hostSeen) s_st_bits |= SUB_ST_HOST_UART;

    // 汇总：只列可判定项
    s_st_bits |= SUB_ST_DONE;
    {
        const char    *names[] = { "cam-calib", "host-uart" };
        const uint16_t masks[] = { SUB_ST_CAM_CALIB, SUB_ST_HOST_UART };
        for (uint8_t i = 0; i < 2; i++) {
            dbg_printf("[ST]   %-9s : %s\n", names[i], (s_st_bits & masks[i]) ? "OK" : "--");
        }
    }
    dbg_println("[ST]   watch-only (not counted): motor twitch, photo level");
    dbg_printf("[ST] ------- done: bits=0x%04X -------\n", (unsigned)s_st_bits);
}

// ================= 占位函数（TODO：按架构实现具体逻辑）=================
void busy_error_handle(uint8_t errorType) {
    // TODO: 本地错误处理（上报底板 / 屏幕显示）
    (void)errorType;
}

// ================= 低功耗（A 级）=================
// 上电流程里最后调用：CPU 降频 + 打印电机待机状态（STBY 已在 sub_hardware_init 拉低）。
//
// 时序安全性（这是低功耗改动最需要确认的一点）：
//   ESP32-S3 的 APB 时钟恒为 80MHz —— Arduino 核心的 calculateApb() 对 S3 直接返回 APB_CLK_FREQ，
//   所以 240MHz→80MHz 只改 CPU 分频，APB 不变。挂 APB 的外设全部不受影响：
//     - UART：115200 波特率由 APB 分频得到 → 不变（子板↔底板、子板↔摄像头都是 115200）
//     - LEDC(PWM)：20kHz / 8bit 由 APB 分频得到 → 占空比与频率不变（发牌电机转速不变）
//   millis()/delay() 走 systimer + FreeRTOS tick（与 CPU 频率无关）→ 所有 ms 级时序不变：
//     触发脉冲 CAM_TRIG_PULSE_MS、出牌动作 MOTOR_FWD_MS/BRAKE/REV/PAUSE、光电门去抖
//     PHOTO_DEBOUNCE_MS、摄像头等待 CAM_RESULT_TIMEOUT_MS 全部照旧。
//   结论：降频不会引入时序错误；它只让"同样的代码跑得慢一点"，而这份负载本来就很轻。
void sub_power_setup(void) {
    uint32_t cpuBefore = getCpuFrequencyMhz();
#if SUB_CPU_MHZ > 0
    bool ok = setCpuFrequencyMhz(SUB_CPU_MHZ);
    dbg_printf("[PWR] cpu %u -> %u MHz (%s), apb %u MHz\n",
               (unsigned)cpuBefore, (unsigned)getCpuFrequencyMhz(), ok ? "ok" : "FAILED",
               (unsigned)(getApbFrequency() / 1000000UL));
    dbg_println("[PWR] apb unchanged -> uart 115200 / pwm 20kHz / all ms timings unchanged");
#else
    dbg_printf("[PWR] cpu stay at %u MHz (SUB_CPU_MHZ=0, 降频已关闭)\n", (unsigned)cpuBefore);
#endif
    dbg_println("[PWR] motor driver standby (STBY=LOW) until next deal action");
}
