#pragma once

/**
 * 底板 ESP32-S3 IO 映射
 * 来源：docs/architecture_v2.md 1.3（转写自《底板软件开发交接指南》）
 * ⚠️ 使用前必须与底板原理图逐项核对（交接指南已声明可能存在错误）。
 */

// ---- TMC2209 步进驱动（底座转盘）----
#define PIN_TMC_STEP   17   // 步进脉冲
#define PIN_TMC_DIR    16   // 方向
#define PIN_TMC_ENN    20   // 使能（高=断电，低=工作）
#define PIN_TMC_TX     8    // 单线 UART 发送（串 1K 电阻接 PDN_UART）
#define PIN_TMC_RX     19   // 单线 UART 接收（不串电阻，直连 PDN_UART）

// ---- A3144 霍尔零点传感器 ----
#define PIN_HALL       7   // 必须 INPUT_PULLUP

// ---- EC11 旋转编码器 ----
#define PIN_ENC_A      37  // A 相（正交）
#define PIN_ENC_B      36  // B 相（正交）
#define PIN_ENC_SW     35  // 按键（按下为低电平）

// ---- SPI 总线（屏幕 + SD 共用 SCK/MOSI，独立片选）----
#define PIN_SPI_SCK    40
#define PIN_SPI_MOSI   39
#define PIN_TFT_DC     41
#define PIN_TFT_RESET  42
#define PIN_TFT_CS     2
#define PIN_SD_CS      38
#define PIN_SD_MISO    1

// ---- 滑环预留外部 IO（拓展备用）----
#define PIN_EXT1       47
#define PIN_EXT2       21
#define PIN_EXT3       14

// ---- 滑环串口（与子板通信，2 根线，UART 协议）----
#define PIN_RING_UART_POS   45   // TX(连子板RX)
#define PIN_RING_UART_NEG   48   // RX
