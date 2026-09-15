/**
 * 发牌机底板 —— 主程序框架
 * 架构依据：docs/architecture_v2.md
 *
 * 启动顺序（架构 v2 第九章）：
 *   1. 硬件初始化（GPIO / UART / SPI）
 *   2. 创建内核对象（队列 / 信号量 / 互斥量 / 事件组）
 *   3. 屏幕初始化（含上电自检色块）—— 自检结果要画在屏幕上，所以排在第 5 步前面
 *   4. 底盘步进初始化（AccelStepper，参考程序同款控制，无 UART 电流配置）
 *   5. 霍尔归零（当前"以开机位置为零点"，见 hardware.cpp）
 *   6. 外设自检（self_test：引脚电平 / BLE / 与子板握手 + 底盘微动 + 屏幕汇总）
 *   7. 创建任务（按优先级从低到高）
 *   8. 发送初始菜单，进入 IDLE
 */
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "app_config.h"
#include "itc.h"
#include "tasks.h"
#include "tasks_deal.h"
#include "hardware.h"
#include "display.h"
#include "ble_comms.h"

void setup() {
    Serial.begin(115200);              // CH340 调试串口（UART0）
    delay(200);
    // 版本自证：一眼看出板上跑的是哪次构建（排查"改完/烧完没生效"用）
    //   build  = 编译时刻（__DATE__/__TIME__）
    //   schemes= 方案表数量：10 = 含 SortJoker/SortFace 的新表；8 = 重排前的旧表
    Serial.printf("[BOOT] 发牌机底板 v2 框架  build %s %s  schemes=%d (last=%s)\n",
                  __DATE__, __TIME__, DEAL_PRESET_COUNT,
                  deal_scheme_name(DEAL_PRESET_COUNT - 1));

    init_hardware();                   // 1
    create_itc();                      // 2
    ble_init();                        // 2.1 BLE(NUS)：收小程序命令帧 / 发状态帧
    init_interrupts();                 // 2.5 中断挂接（依赖内核对象）
    display_init();                    // 3：屏幕（含上电自检色块）—— 自检要在屏幕上出结果
    tmc2209_init();                    // 4：底盘步进初始化（函数名沿用早期版本）
    hall_homing();                     // 5
    // 子板模式：无子板模式（CLI `subsim on` / USE_SUBBOARD=0）下自检的"子板链路"会显示 FAIL，
    // 这是预期的（本来就没接子板）。先把模式打出来，免得误判。
    Serial.printf("[BOOT] sub board mode: %s\n",
                  deal_sim_get_auto()
                      ? "SIM 无子板模式（底板自己模拟子板事件；subsim off 关闭）"
                      : "REAL 正常模式（要求子板在线）");
    self_test();                       // 6：外设自检（屏幕汇总 + 底盘微动 + 与子板握手）
    create_all_tasks();                // 7

    display_cmd_t cmd = {};
    cmd.type = DISPLAY_CMD_IDLE;       // 8：初始 IDLE 屏（未选择 + CONFIRM 按钮）
    cmd.payload.menu.selectedIndex = 0;
    send_display_command(&cmd);

    Serial.println("[BOOT] 启动完成，进入 IDLE");
}

void loop() {
    // 所有业务都在 FreeRTOS 任务中执行，此处仅作空闲
    vTaskDelay(pdMS_TO_TICKS(1000));
}
