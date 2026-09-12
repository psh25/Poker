/**
 * 子板硬件层：初始化、中断、串口环形缓冲、TB6612 发牌电机驱动。
 * 摄像头 / 光敏按 docs/subboard_architecture.md 后续实现。
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
    digitalWrite(PIN_MOTOR_STBY, HIGH);   // 使能（保持高，便于随时启动）
    digitalWrite(PIN_MOTOR_AIN1, LOW);
    digitalWrite(PIN_MOTOR_AIN2, LOW);
    ledcSetup(MOTOR_PWM_CH, MOTOR_PWM_FREQ, MOTOR_PWM_BITS);
    ledcAttachPin(PIN_MOTOR_PWM, MOTOR_PWM_CH);
    ledcWrite(MOTOR_PWM_CH, 0);

    // 光电门（有牌 = 低电平，无牌 = 高）
    sub_photo_init();

    // 摄像头：这里的初始化已挪到 sub_camera_trigger()（首次触发时才配置）。
    // 原因：PIN_CAM_TRIG = GPIO20 是 ESP32-S3 原生 USB 的 D+，
    //       开机就把它配成输出会破坏原生 USB 口。平时不碰，避免影响开机自检。

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
    // 自检：光敏电平、电机驱动、摄像头可初始化 → 上报 EVT_READY（架构第八章）
    busy_self_test();
    proto_send(EVT_READY, NULL, 0);
}

// ================= 摄像头：截图触发 + 回传接收 =================
//
// 工作方式：子板不主动取图，只负责“拉高 TRIG → 等摄像头把识别结果发回来”。
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

// 接收方式：在截图等待窗口内轮询 Serial2（不用 onReceive 中断）。
// 原因：摄像头未接时 RX 引脚浮空，中断会被噪声反复触发；轮询只在等待窗口读，
//       天然规避这个问题。等摄像头接线确认后如需中断可再加回。

#define CAM_LINE_MAX  40           // 单行最大长度（"RESULT:diamond_10" 约 18）

// 截图会话状态：IDLE → PULSE（TRIG 高）→ WAIT（等回传）→ IDLE
typedef enum { CAM_IDLE = 0, CAM_PULSE, CAM_WAIT } cam_session_t;
static cam_session_t s_cam_state = CAM_IDLE;
static uint32_t      s_cam_t0 = 0;
static uint8_t       s_cam_payload[2];        // [card, src]
static uint8_t       s_cam_payload_len = 0;
static bool          s_cam_got_result = false;
static bool          s_cam_cam_error = false; // 收到 RESULT:ERROR
static bool          s_cam_inited = false;
static char          s_cam_line[CAM_LINE_MAX];
static uint8_t       s_cam_line_len = 0;

void sub_camera_trigger(void) {
    if (s_cam_state != CAM_IDLE) {
        dbg_println("[CAM] trigger ignored (session busy)");
        return;
    }
    // 首次触发才配置：GPIO20 平时保持原生 USB 状态，不碰它
    if (!s_cam_inited) {
        s_cam_inited = true;
        pinMode(PIN_CAM_TRIG, OUTPUT);
        digitalWrite(PIN_CAM_TRIG, HIGH);     // 空闲高电平（OpenMV P6 下降沿触发）
        Serial2.begin(CAM_UART_BAUD, SERIAL_8N1, PIN_CAM_RX, PIN_CAM_TX);
    }
    s_cam_got_result = false;
    s_cam_cam_error = false;
    s_cam_payload_len = 0;
    s_cam_line_len = 0;
    s_cam_t0 = millis();
    s_cam_state = CAM_PULSE;
    digitalWrite(PIN_CAM_TRIG, LOW);          // 下降沿 → 触发拍照
    dbg_println("[CAM] TRIG low (trigger)");
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
                dbg_println("[CAM] RESULT:ERROR -> report CAM fail");
                proto_send(EVT_ERROR_CAM_FAIL, NULL, 0);
            } else {
                dbg_printf("[CAM] result card=%u src=%u\n",
                           (unsigned)s_cam_payload[0], (unsigned)s_cam_payload[1]);
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
    // 停止：AIN1/2 全低 = 滑行（自然停）；如需立即停可改 AIN1/2 全高 = 短刹车
    digitalWrite(PIN_MOTOR_AIN1, LOW);
    digitalWrite(PIN_MOTOR_AIN2, LOW);
    ledcWrite(MOTOR_PWM_CH, 0);
}

// 电机自检：正转 300ms → 刹车 MOTOR_BRAKE_MS → 反转 300ms → 停（开机与 mtest 命令用）
// 如果电机完全不转，说明引脚 / 驱动供电 / 接线有问题
void motor_self_test(void) {
    // 先动电机再打印：即使调试串口异常也不会卡住自检动作
    busy_motor_start();
    dbg_println("[MOT] self-test: forward 300ms...");
    delay(300);
    if (MOTOR_BRAKE_MS > 0) {
        busy_motor_brake();
        dbg_println("[MOT] self-test: brake...");
        delay(MOTOR_BRAKE_MS);
    }
    busy_motor_start_reverse();
    dbg_println("[MOT] self-test: reverse 300ms...");
    delay(300);
    busy_motor_stop();
    dbg_println("[MOT] self-test done (motor should have twitched twice)");
}

// ================= 占位函数（TODO：按架构实现具体逻辑）=================
void busy_self_test(void) {
    // 上电自检：先让电机正/反转各抖一下，肉眼确认驱动链路正常
    motor_self_test();
}

void busy_error_handle(uint8_t errorType) {
    // TODO: 本地错误处理（上报底板 / 屏幕显示）
    (void)errorType;
}




