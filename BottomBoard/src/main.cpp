/**
 * 发牌机底板 —— 主程序框架
 * 架构依据：docs/architecture_v2.md
 *
 * 启动顺序（架构 v2 第九章）：
 *   1. 硬件初始化（GPIO / UART / SPI）
 *   2. 创建内核对象（队列 / 信号量 / 互斥量 / 事件组）
 *   3. TMC2209 单线 UART 初始化（I_scale_analog=0、StallGuard）
 *   4. 霍尔两段式自动归零
 *   5. 外设自检（霍尔 / 编码器 / SD / TMC / 滑环串口）
 *   6. 创建任务（按优先级从低到高）
 *   7. 发送初始菜单，进入 IDLE
 */
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "app_config.h"
#include "itc.h"
#include "tasks.h"
#include "hardware.h"
#include "display.h"

void setup() {
    Serial.begin(115200);              // CH340 调试串口（UART0）
    delay(200);
    Serial.println("[BOOT] 发牌机底板 v2 框架");

    init_hardware();                   // 1
    create_itc();                      // 2
    init_interrupts();                 // 2.5 中断挂接（依赖内核对象）
    tmc2209_init();                    // 3
    hall_homing();                     // 4
    self_test();                       // 5
    create_all_tasks();                // 6

    display_cmd_t cmd = {};
    cmd.type = DISPLAY_CMD_IDLE;       // 7：初始 IDLE 屏（未选择 + CONFIRM 按钮）
    cmd.payload.menu.selectedIndex = 0;
    send_display_command(&cmd);

    Serial.println("[BOOT] 启动完成，进入 IDLE");
}

void loop() {
    // 所有业务都在 FreeRTOS 任务中执行，此处仅作空闲
    vTaskDelay(pdMS_TO_TICKS(1000));
}
