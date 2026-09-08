#pragma once

/**
 * 子板 IO 映射
 * ⚠️ 当前按 test1 调试台（ESP32-S3 + TB6612FNG + 直流减速电机）配置，
 *    量产子板硬件定型后必须按实际原理图修改。
 */

// ---- 发牌电机：TB6612FNG（与 test1 调试台一致，引脚已实测可用）----
//   PWMA = PWM（LEDC 20kHz）；AIN1/AIN2 = 方向；STBY = 高使能
//   方向（实测本机相反，已对调）：AIN1=0,AIN2=1 → 正转出牌；AIN1=1,AIN2=0 → 反转回退
//   ⚠️ GPIO5 / GPIO6 在调试台上不可用（见 test1/HARDWARE_NOTES.md）
#define PIN_MOTOR_PWM        11  // TB6612 PWMA
#define PIN_MOTOR_AIN1       48  // TB6612 AIN1
#define PIN_MOTOR_AIN2       47  // TB6612 AIN2
#define PIN_MOTOR_STBY       45  // TB6612 STBY（高=使能）

// ---- 光敏传感器 ----
#define PIN_PHOTO            3   // TODO: 光敏 GPIO 中断（检测牌通过）；测试台未接光敏（定时出牌模式）
//   ⚠️ 量产子板接光敏时按实际原理图改到独立引脚，勿与电机 STBY 共用。

// ---- 摄像头 / 视觉模组（接口待定：SPI / I2C / UART）----
#define PIN_CAM_TX           10  // 子板的连到摄像头的TX
#define PIN_CAM_RX           46  // 同上，子板的RX
#define PIN_CAM_TRIG         20  // 未使用

// ---- 与底板通信：2 线 UART ----
// 测试台接线（test1 rig ↔ 底板）：子板 RX=41 ← 底板 TX=42；子板 TX=42 → 底板 RX=41
#define PIN_UART_POS         38     // RX(连底板TX)
#define PIN_UART_NEG         39     // TX(连底板RX)
