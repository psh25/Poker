/**
 * 子板 —— 主程序（裸机：超级循环 + 中断）
 * 架构依据：docs/subboard_architecture.md
 *
 * 启动顺序：
 *   1. 硬件初始化（GPIO / UART / 中断）
 *   2. 上电自检 → 上报 EVT_READY
 *   3. 进入 IDLE，主循环：解析串口命令 → 执行状态机
 */
#include <Arduino.h>

#include "app_config.h"
#include "hardware.h"
#include "protocol.h"
#include "state_machine.h"

void setup() {
    sub_hardware_init();          // 1. GPIO / UART / 中断
    sub_self_test();              // 2. 自检 → EVT_READY

    Serial.println("[SUB] boot ok, waiting for commands");
}

void loop() {
    // 1) 串口接收：中断已写入环形缓冲，这里把字节喂给协议解析器
    uint8_t b;
    while (sub_uart_read_byte(&b)) {
        proto_rx_byte(b);
    }

    // 2) 执行裸机状态机（超级循环，全部超时基于 millis()）
    sub_state_run();
}
