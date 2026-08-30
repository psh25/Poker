/**
 * 子板硬件层：初始化、中断、串口环形缓冲、占位函数。
 * 具体驱动（电机 / 光敏 / 摄像头）按 docs/subboard_architecture.md 后续实现。
 */
#include <Arduino.h>

#include "pins_config.h"
#include "app_config.h"
#include "protocol.h"
#include "hardware.h"

// ---- RGB LED debug indicator (common cathode: '-' -> GND) ----
static uint8_t s_led_r = 0, s_led_g = 0, s_led_b = 0;
static uint32_t s_led_until = 0;

static void led_set(uint8_t r, uint8_t g, uint8_t b, uint32_t ms) {
    s_led_r = r; s_led_g = g; s_led_b = b;
    s_led_until = millis() + ms;
    sub_led_rgb(r, g, b);
}

void sub_led_rgb(bool red, bool green, bool blue) {
    digitalWrite(PIN_LED_R, red   ? HIGH : LOW);
    digitalWrite(PIN_LED_G, green ? HIGH : LOW);
    digitalWrite(PIN_LED_B, blue  ? HIGH : LOW);
}

void sub_led_flash_rx_byte(void) { led_set(1, 0, 0, 150); }   // red 150ms
void sub_led_flash_frame_ok(void) { led_set(0, 1, 0, 800); }  // green 800ms
void sub_led_flash_ack_tx(void)   { led_set(0, 0, 1, 300); }  // blue 300ms

void sub_led_tick(void) {
    if (s_led_until && (int32_t)(millis() - s_led_until) >= 0) {
        s_led_until = 0;
        sub_led_rgb(0, 0, 0);
    }
}

void sub_led_init(void) {
    pinMode(PIN_LED_R, OUTPUT);
    pinMode(PIN_LED_G, OUTPUT);
    pinMode(PIN_LED_B, OUTPUT);
    sub_led_rgb(0, 0, 0);
}

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

    // 电机（占位）
    pinMode(PIN_MOTOR_PWM, OUTPUT);
    digitalWrite(PIN_MOTOR_PWM, LOW);
    pinMode(PIN_MOTOR_DIR, OUTPUT);
    digitalWrite(PIN_MOTOR_DIR, LOW);
    pinMode(PIN_MOTOR_CURRENT, INPUT);    // TODO: 电流检测 ADC

    // 光敏
    pinMode(PIN_PHOTO, INPUT_PULLUP);     // TODO: 按传感器电平配置

    // 摄像头（占位）
    pinMode(PIN_CAM_SCK, OUTPUT);
    pinMode(PIN_CAM_MOSI, OUTPUT);
    pinMode(PIN_CAM_MISO, INPUT);
    pinMode(PIN_CAM_CS, OUTPUT);
    digitalWrite(PIN_CAM_CS, HIGH);
    pinMode(PIN_CAM_RDY, INPUT);

    // 指示灯
    sub_led_init();   // RGB LED (common cathode: R=15 G=16 B=8)

    // 与底板通信串口（2 线 UART）
    Serial1.begin(SUB_UART_BAUD, SERIAL_8N1, SUB_UART_RX_PIN, SUB_UART_TX_PIN);
    Serial1.onReceive(sub_uart_rx_isr);   // 中断接收 → 环形缓冲

    attachInterrupt(digitalPinToInterrupt(PIN_PHOTO), photo_isr, CHANGE); // TODO: 按实际沿配置
}

void sub_self_test(void) {
    // 自检：光敏电平、电机驱动、摄像头可初始化 → 上报 EVT_READY（架构第八章）
    busy_self_test();
    proto_send(EVT_READY, NULL, 0);
}

// ================= 占位函数（TODO：按架构实现具体逻辑）=================
void busy_motor_start(void) {
    // TODO: 启动发牌电机（PWM 输出，必要时方向/软启动）
}

void busy_motor_stop(void) {
    // TODO: 停止发牌电机（PWM=0）
}

void busy_camera_capture(uint8_t *cardData, uint8_t *cardLen) {
    // TODO: 触发摄像头拍照并识别牌面（花色、点数）
    // 成功：填充 cardData 并设置 *cardLen > 0
    // 失败：保持 *cardLen = 0（状态机会在 2s 后按未知牌处理）
    (void)cardData; (void)cardLen;
}

void busy_self_test(void) {
    // TODO: 光敏 / 电机驱动 / 摄像头初始化自检
}

void busy_status_query(void) {
    // TODO: 回复状态（响应帧待协议定稿）
}

void busy_error_handle(uint8_t errorType) {
    // TODO: 本地错误处理（如 LED 快闪）
    (void)errorType;
}
