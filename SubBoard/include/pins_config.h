#pragma once

/**
 * 子板 IO 映射（占位）
 * ⚠️ 子板硬件选型尚未最终确定（见 docs/subboard_architecture.md 落地清单），
 *    以下引脚均为占位符，硬件定型后必须按实际原理图修改。
 */

// ---- 发牌电机（6V 直流，占位：PWM + 方向，可选电流检测）----
#define PIN_MOTOR_PWM        4   // TODO: 电机 PWM 输出
#define PIN_MOTOR_DIR        5   // TODO: 电机方向
#define PIN_MOTOR_CURRENT    6   // TODO: 电流检测（ADC，可选，用于堵转检测）

// ---- 光敏传感器 ----
#define PIN_PHOTO            7   // TODO: 光敏 GPIO 中断（检测牌通过）

// ---- 摄像头 / 视觉模组（接口待定：SPI / I2C / UART）----
#define PIN_CAM_SCK          10  // TODO
#define PIN_CAM_MOSI         11  // TODO
#define PIN_CAM_MISO         12  // TODO
#define PIN_CAM_CS           13  // TODO
#define PIN_CAM_RDY          14  // TODO: 拍照完成 / 识别就绪

// ---- 指示灯 ----
#define PIN_LED_R            15   // RGB LED common-cathode: R channel
#define PIN_LED_G            16   // RGB LED common-cathode: G channel
#define PIN_LED_B            8    // RGB LED common-cathode: B channel
#define PIN_LED              PIN_LED_R   // compat alias: old single LED pin

// ---- 与底板通信：2 线 UART（对端为底板 IO42/IO41）----
#define PIN_UART_TX          17  // TODO
#define PIN_UART_RX          18  // TODO
