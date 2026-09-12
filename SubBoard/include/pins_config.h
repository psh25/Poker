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

// ---- 光电门（原来的光敏传感器）----
// 电平：有牌 = 低电平(GND)，无牌 = 高。主循环轮询 + 去抖（不用中断），见 hardware.cpp
#define PIN_PHOTO            3

// ---- 摄像头 / 视觉模组（已定：OpenMV H7 Plus，UART 文本行回传）----
#define PIN_CAM_TX           10  // 子板 → 摄像头 TX
#define PIN_CAM_RX           46  // 摄像头 → 子板 RX（只接收）
// ⚠️ CAM_TRIG = GPIO20：ESP32-S3 的 GPIO19/20 是原生 USB 的 D-/D+。
//    本工程开了 ARDUINO_USB_CDC_ON_BOOT，因此**开机时不要碰这个脚**，
//    否则会把原生 USB 口拉坏。代码里改成"只在真正要截图时才配置成输出"，
//    平时保持 USB 态；截图期间原生 USB 日志不可用，请从 CH340 口看日志。
//    PCB 已固定该引脚，暂不改动；若以后要换脚需同步改原理图。
#define PIN_CAM_TRIG         20  // 摄像头截图触发（**拉低一个脉冲**，OpenMV P6 下降沿触发）

// ---- 与底板通信：2 线 UART ----
// 接线（POS 连 POS、NEG 连 NEG + 共地）：
//   底板 POS=IO15(TX) → 子板 POS=IO38(RX)；子板 NEG=IO39(TX) → 底板 NEG=IO12(RX)
#define PIN_UART_POS         38     // RX（连底板 POS/TX）
#define PIN_UART_NEG         39     // TX（连底板 NEG/RX）
