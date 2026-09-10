/**
 * 子板硬件层：初始化、中断、串口环形缓冲、TB6612 发牌电机驱动。
 * 摄像头 / 光敏按 docs/subboard_architecture.md 后续实现。
 */
#include <Arduino.h>
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
static void cam_uart_rx_isr(void);   // 摄像头回传串口中断（定义见下方“摄像头”一节）
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

// ---- 光敏中断（去抖占位，按实际传感器极性调整）----
static volatile bool s_photo_flag = false;

static void IRAM_ATTR photo_isr(void) {
    static uint32_t last = 0;
    uint32_t now = millis();
    if (now - last < 5) return;           // TODO: 5ms 消抖（参考底板编码器）
    last = now;
    s_photo_flag = true;
}

bool sub_photo_take(void) {
    if (!s_photo_flag) return false;
    s_photo_flag = false;
    return true;
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

    // 光敏
    pinMode(PIN_PHOTO, INPUT_PULLUP);     // TODO: 按传感器电平配置

    // 摄像头：截图触发脚先初始化；识别接口（UART）待模组确定
    pinMode(PIN_CAM_TRIG, OUTPUT);
    digitalWrite(PIN_CAM_TRIG, LOW);
    // 摄像头回传串口（只接收：摄像头 TX → PIN_CAM_RX；PIN_CAM_TX 备用）
    Serial2.begin(CAM_UART_BAUD, SERIAL_8N1, PIN_CAM_RX, PIN_CAM_TX);
    Serial2.onReceive(cam_uart_rx_isr);

    // 与底板通信串口（2 线 UART）
    Serial1.begin(SUB_UART_BAUD, SERIAL_8N1, SUB_UART_RX_PIN, SUB_UART_TX_PIN);
    Serial1.onReceive(sub_uart_rx_isr);   // 中断接收 → 环形缓冲

    attachInterrupt(digitalPinToInterrupt(PIN_PHOTO), photo_isr, CHANGE); // TODO: 按实际沿配置

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
// 【回传帧格式（占位，模组确定后按手册改这一段即可）】
//   byte0  帧头     0x5A
//   byte1  类型     0x01 = 识别结果
//   byte2  长度 n   data 字节数（n ≤ CARD_DATA_MAX）
//   byte3.. 数据    识别结果载荷，约定牌面编码为 花色(1B) + 点数(1B)，其余待定
//   末尾   校验     从 byte1 到 data 末字节的累加和低 8 位
//   解析失败（帧头/长度/校验不符）→ 丢弃并重新找帧头
//
// 接收路径：Serial2 收到字节 → onReceive 中断写入环形缓冲 → 主循环 sub_camera_service() 解析。
// 结果去向：解析成功后发 EVT_CARD_VALUE（data = 识别载荷）；超时未回传时按 CAM_EMPTY_ON_TIMEOUT 处理。

#define CAM_FRAME_HEADER 0x5A
#define CAM_TYPE_RESULT  0x01

static volatile uint8_t  s_cam_ring[CAM_RX_RING_SIZE];
static volatile uint16_t s_cam_head = 0;
static volatile uint16_t s_cam_tail = 0;

static void cam_ring_write(uint8_t b) {
    uint16_t next = (s_cam_head + 1) % CAM_RX_RING_SIZE;
    if (next == s_cam_tail) return;      // 满则丢弃
    s_cam_ring[s_cam_head] = b;
    s_cam_head = next;
}

static bool cam_ring_read(uint8_t *b) {
    if (s_cam_tail == s_cam_head) return false;
    *b = s_cam_ring[s_cam_tail];
    s_cam_tail = (s_cam_tail + 1) % CAM_RX_RING_SIZE;
    return true;
}

static void cam_uart_rx_isr(void) {
    while (Serial2.available() > 0) {
        cam_ring_write((uint8_t)Serial2.read());
    }
}

// 截图会话状态：IDLE → PULSE（TRIG 高）→ WAIT（等回传）→ IDLE
typedef enum { CAM_IDLE = 0, CAM_PULSE, CAM_WAIT } cam_session_t;
static cam_session_t s_cam_state = CAM_IDLE;
static uint32_t      s_cam_t0 = 0;
static uint8_t       s_cam_payload[CARD_DATA_MAX];
static uint8_t       s_cam_payload_len = 0;
static bool          s_cam_got_result = false;

void sub_camera_trigger(void) {
    if (s_cam_state != CAM_IDLE) {
        dbg_println("[CAM] trigger ignored (session busy)");
        return;
    }
    s_cam_got_result = false;
    s_cam_payload_len = 0;
    s_cam_t0 = millis();
    s_cam_state = CAM_PULSE;
    digitalWrite(PIN_CAM_TRIG, HIGH);
    dbg_println("[CAM] TRIG high");
}

// 解析环形缓冲中的一帧；成功则把载荷存入 s_cam_payload 并返回 true
static bool cam_parse(void) {
    uint8_t b;
    while (cam_ring_read(&b)) {
        if (b != CAM_FRAME_HEADER) continue;          // 找帧头
        uint8_t type = 0, len = 0;
        if (!cam_ring_read(&type)) return false;      // 帧不完整，等下次
        if (!cam_ring_read(&len)) return false;
        if (type != CAM_TYPE_RESULT || len == 0 || len > CARD_DATA_MAX) continue;
        uint8_t data[CARD_DATA_MAX];
        bool complete = true;
        for (uint8_t i = 0; i < len; i++) {
            if (!cam_ring_read(&data[i])) { complete = false; break; }
        }
        if (!complete) return false;
        uint8_t sum = 0;
        if (!cam_ring_read(&sum)) return false;
        uint8_t calc = (uint8_t)(type + len);
        for (uint8_t i = 0; i < len; i++) calc = (uint8_t)(calc + data[i]);
        if (calc != sum) {
            dbg_printf("[CAM] checksum mismatch (got 0x%02X want 0x%02X)\n", sum, calc);
            continue;
        }
        memcpy(s_cam_payload, data, len);
        s_cam_payload_len = len;
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
            digitalWrite(PIN_CAM_TRIG, LOW);          // 摄像头检测上升沿，脉冲结束拉低
            s_cam_state = CAM_WAIT;
            s_cam_t0 = now;
            dbg_println("[CAM] TRIG low, waiting result...");
        }
        break;

    case CAM_WAIT:
        if (!s_cam_got_result && cam_parse()) {
            s_cam_got_result = true;
            dbg_printf("[CAM] result len=%u\n", (unsigned)s_cam_payload_len);
            proto_send(EVT_CARD_VALUE, s_cam_payload, s_cam_payload_len);
            s_cam_state = CAM_IDLE;
        } else if (now - s_cam_t0 >= CAM_RESULT_TIMEOUT_MS) {
            // 摄像头未回传：保留“发空牌”调试路径，让整条流程还能跑通
#if CAM_EMPTY_ON_TIMEOUT
            dbg_println("[CAM] timeout, report EMPTY card (debug)");
            proto_send(EVT_CARD_VALUE, NULL, 0);
#else
            dbg_println("[CAM] timeout, report CAM fail");
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
    dbg_println("[MOT] self-test: forward 300ms...");
    busy_motor_start();
    delay(300);
    if (MOTOR_BRAKE_MS > 0) {
        dbg_println("[MOT] self-test: brake...");
        busy_motor_brake();
        delay(MOTOR_BRAKE_MS);
    }
    dbg_println("[MOT] self-test: reverse 300ms...");
    busy_motor_start_reverse();
    delay(300);
    busy_motor_stop();
    dbg_println("[MOT] self-test done (motor should have twitched twice)");
}

// ================= 占位函数（TODO：按架构实现具体逻辑）=================
void busy_camera_capture(uint8_t *cardData, uint8_t *cardLen) {
    // TODO: 触发摄像头拍照并识别牌面（花色、点数）
    // 成功：填充 cardData 并设置 *cardLen > 0
    // 失败：保持 *cardLen = 0（状态机会在 2s 后按未知牌处理）
    (void)cardData; (void)cardLen;
}

void busy_self_test(void) {
    // 上电自检：先让电机正/反转各抖一下，肉眼确认驱动链路正常
    motor_self_test();
}

void busy_error_handle(uint8_t errorType) {
    // TODO: 本地错误处理（上报底板 / 屏幕显示）
    (void)errorType;
}




