#pragma once

/*
 * 底板 IO 映射 —— 新主控：经典 ESP32 (ESP32 Dev Module / esp32dev)
 * 2026-09-09 由 ESP32-S3 迁移。经典 ESP32 限制：
 *   - GPIO6~11 接内部 Flash，不可用
 *   - GPIO34~39 仅输入且无内部上拉（编码器/霍尔不要用）
 *   - GPIO1/3 为 UART0（串口调试），不占用
 * ⚠️ 以下为“建议分配”，接线时请与底板原理图核对；SD/EXT 暂不接线。
 */

// ---- TMC2209 步进驱动（底座转盘；电流由驱动板 VREF 设定，当前固件不用单线 UART）----
#define PIN_TMC_STEP   27   // 步进脉冲
#define PIN_TMC_DIR    14   // 方向
#define PIN_TMC_ENN    26   // 使能（高=断电，低=工作）

// ---- EC11 旋转编码器 ----
#define PIN_ENC_A      25   // A 相（CLK）
#define PIN_ENC_B      33   // B 相（DT）
#define PIN_ENC_SW     32   // 按键（按下为低电平）

// ---- SPI 总线（屏幕 + SD 共用 SCK/MOSI，独立片选）----
#define PIN_SPI_SCK    23
#define PIN_SPI_MOSI   22
#define PIN_TFT_DC     21
#define PIN_TFT_RESET  19
#define PIN_TFT_CS     18
// SD：暂不使用 —— 保留定义、不接线、不初始化
#define PIN_SD_CS      4
#define PIN_SD_MISO    5

// ---- 滑环串口（与子板通信，2 根线，UART 协议）----
// ⚠️ NEG(RX)=12 为 strapping 脚（复位需为低）；若接线后无法启动，请把 RX 换到 4/5 等脚
#define PIN_RING_UART_POS   17   // TX2 -> 子板 RX
#define PIN_RING_UART_NEG   16   // RX2 <- 子板 TX

// 原 ESP32-S3 的 EXT1~3 预留脚在新板暂不使用，已移除；需要时再按可用 IO 分配。
