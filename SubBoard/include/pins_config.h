#pragma once

/**
 * 子板 IO 映射
 * ⚠️ 当前按 test1 调试台（ESP32-S3 + TB6612FNG + 直流减速电机）配置，
 *    量产子板硬件定型后必须按实际原理图修改。
 */

// ---- 发牌电机：TB6612FNG（与 test1 调试台一致，引脚已实测可用）----
//   PWMA = PWM（LEDC 20kHz）；AIN1/AIN2 = 方向；STBY = 高使能
//   方向（实测本机相反，已对调）：AIN1=0,AIN2=1 → 正转出牌；AIN1=1,AIN2=0 → 反转回退
//   ⚠️ 旧 PCB 已损坏，现在按飞线自由分配（原来"GPIO5/GPIO6 不可用"是旧调试台的限制，已作废）。
//   ⚠️ 但下面这些脚**不要用**：19/20（原生 USB D-/D+）、26~37（Flash/PSRAM，N16R8）、
//      43/44（UART0 调试口）、0/3/45/46（strapping，能避就避）。
#define PIN_MOTOR_PWM        21  // TB6612 PWMA
#define PIN_MOTOR_AIN1       48  // TB6612 AIN1
// ⚠️ AIN2 = GPIO20 会占掉原生 USB 的 D+：从初始化起就把它当输出驱动，原生 USB 日志会不正常。
//    建议把 AIN2 也换到普通 IO（5/6/7/8/15/16/17/18 任一），把 19/20 留给 USB；
//    换好接线后只要改这一行即可（代码里没有别处引用）。
#define PIN_MOTOR_AIN2       20  // TB6612 AIN2
#define PIN_MOTOR_STBY       45  // TB6612 STBY（高=使能）

// ---- 光电门（原来的光敏传感器）----
// 电平：有牌 = 低电平(GND)，无牌 = 高。主循环轮询 + 去抖（不用中断），见 hardware.cpp
#define PIN_PHOTO            3

// ---- 摄像头 / 视觉模组（已定：OpenMV H7 Plus，UART 文本行回传）----
#define PIN_CAM_TX           10  // 子板 → 摄像头 TX
#define PIN_CAM_RX           46  // 摄像头 → 子板 RX（只接收）
// 【v4.0 改脚】TRIG 从 GPIO20 换到 GPIO4：20 已经给了 PIN_MOTOR_AIN2，两个功能同脚会互相打架
//   （电机换向会当成摄像头触发、摄像头触发又会翻转电机方向）；而且 20 是原生 USB 的 D+，
//   当输出会破坏 USB 日志（本工程开了 ARDUINO_USB_CDC_ON_BOOT）。
//   其它可用备选：5 / 6 / 7 / 8 / 15 / 16 / 17 / 18（避开上面列的那些禁用脚）。
#define PIN_CAM_TRIG         4   // 摄像头截图触发（**拉低一个脉冲**，OpenMV P6 下降沿触发）

// ---- 与底板通信：2 线 UART ----
// 接线（POS 连 POS、NEG 连 NEG + 共地）：
//   底板 POS=IO15(TX) → 子板 POS=IO38(RX)；子板 NEG=IO39(TX) → 底板 NEG=IO12(RX)
#define PIN_UART_POS         38     // RX（连底板 POS/TX）
#define PIN_UART_NEG         39     // TX（连底板 NEG/RX）
