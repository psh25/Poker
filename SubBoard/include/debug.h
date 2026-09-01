#pragma once

#include <Arduino.h>

/**
 * 调试输出：同时写到 USB CDC（Serial）和 UART0/CH340（Serial0），
 * 兼容“原生 USB”和“CH340 转串口”两种 ESP32-S3 板。
 * PlatformIO 串口监视器 115200 查看：
 *   原生 USB 板 → 选 USB 串口；CH340 板 → 选 CH340 COM 口。
 * 板间通信仍用 Serial1（见 pins_config.h），不受影响。
 */
void dbg_init(void);
void dbg_print(const char *s);
void dbg_println(const char *s);
void dbg_println(void);                    // 空行
void dbg_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
