#pragma once

/**
 * 底板 ESP32-S3 IO 映射
 * 来源：docs/architecture_v2.md 1.3（转写自《底板软件开发交接指南》）
 * ⚠️ 使用前必须与底板原理图逐项核对（交接指南已声明可能存在错误）。
 */

// ---- TMC2209 步进驱动（底座转盘）----
#define PIN_TMC_STEP   4   // 步进脉冲
#define PIN_TMC_DIR    5   // 方向
#define PIN_TMC_ENN    6   // 使能（高=断电，低=工作）
#define PIN_TMC_TX     17  // 单线 UART 发送（串 1K 电阻接 PDN_UART）
#define PIN_TMC_RX     18  // 单线 UART 接收（不串电阻，直连 PDN_UART）

// ---- A3144 霍尔零点传感器 ----
#define PIN_HALL       1   // 必须 INPUT_PULLUP

// ---- EC11 旋转编码器 ----
#define PIN_ENC_A      8   // A 相（正交）
#define PIN_ENC_B      19  // B 相（正交）
#define PIN_ENC_SW     10  // 按键（按下为低电平）

// ---- 指示灯 / 摄像头补光预留 ----
#define PIN_LED        9

// ---- SPI 总线（屏幕 + SD 共用 SCK/MOSI，独立片选）----
#define PIN_SPI_SCK    12
#define PIN_SPI_MOSI   13
#define PIN_TFT_DC     14
#define PIN_TFT_RESET  21
#define PIN_TFT_CS     47
#define PIN_SD_CS      48
#define PIN_SD_MISO    45

// ---- 滑环预留外部 IO（拓展备用）----
#define PIN_EXT1       39
#define PIN_EXT2       38
#define PIN_EXT3       37

// ---- 滑环串口（与子板通信，2 根线，UART 协议）----
#define PIN_RING_UART_TX  42
#define PIN_RING_UART_RX  41
