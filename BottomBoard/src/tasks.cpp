/**
 * 底板任务（架构 v2 第三章）——本文件只放"任务本体"：
 *   创建任务 / 子板通信 / 编码器 / 屏幕显示 / 系统监控
 * 其它已拆分到：
 *   - host_actions.cpp : 主机命令动作 + 蓝牙接收任务
 *   - cli.cpp          : 调试串口命令行
 *   - tasks_deal.cpp   : 发牌控制任务
 */
#include <Arduino.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/event_groups.h>

#include "app_config.h"
#include "itc.h"
#include "tasks.h"
#include "deal_selection.h"
#include "cli.h"
#include "state_machine.h"
#include "tasks_deal.h"
#include "protocol.h"
#include "display.h"
#include "hardware.h"

void create_all_tasks(void) {
    // 按优先级从低到高创建（避免高优先级任务先于低优先级运行）
    xTaskCreatePinnedToCore(vDisplayTask,      "display",   STACK_DISPLAY,   NULL,
                            PRIO_DISPLAY, NULL, tskNO_AFFINITY);
    xTaskCreatePinnedToCore(vStateManagerTask, "state",     STACK_STATE,     NULL,
                            PRIO_STATE,   NULL, tskNO_AFFINITY);
    xTaskCreatePinnedToCore(vMonitorTask,      "monitor",   STACK_MONITOR,   NULL,
                            PRIO_MONITOR, NULL, tskNO_AFFINITY);
    xTaskCreatePinnedToCore(vSubboardTask,     "subboard",  STACK_SUBBOARD,  NULL,
                            PRIO_SUBBOARD, NULL, CORE_SUBBOARD);
    xTaskCreatePinnedToCore(vEncoderTask,      "encoder",   STACK_ENCODER,   NULL,
                            PRIO_ENCODER, NULL, tskNO_AFFINITY);
    xTaskCreatePinnedToCore(vDealTask,         "deal",      STACK_DEAL,      NULL,
                            PRIO_DEAL,    NULL, CORE_DEAL);
    xTaskCreatePinnedToCore(vBluetoothTask,    "bluetooth", STACK_BLUETOOTH, NULL,
                            PRIO_BLUETOOTH, NULL, tskNO_AFFINITY);
}

// ================= 子板通信任务 =================
void vSubboardTask(void *pv) {
    // 中(2) | 固定核心 0 | 子板串口中断 + 队列
    proto_frame_t tx;

    Serial.println("[CLI] type 'help' for subboard command list");
    static proto_rx_t s_sub_rx;
    proto_rx_init(&s_sub_rx, proto_on_event);

    for (;;) {
        // 1) 下发指令：队列一到就发；没有指令时最多等 SUB_COMM_POLL_MS，顺便当本任务的节拍
        if (xQueueReceive(xSubboardTxQueue, &tx, pdMS_TO_TICKS(SUB_COMM_POLL_MS)) == pdPASS) {
            proto_write_frame(&tx);
        }
        // 2) 接收：子板串口中断已把字节写进环形缓冲（见 hardware.cpp），这里取出来喂协议解析器
        //    事件由 proto_on_event 分发；掉线判定见 protocol.cpp 的 sub_comm_online()
        uint8_t b;
        while (sub_uart_read_byte(&b)) {
            proto_rx_feed(&s_sub_rx, b);
        }
        // 3) 调试 CLI：读取并执行一行命令（实现在 cli.cpp）；人输入频率低，挂在本任务节拍上够用
        cli_poll();
    }
}

// ================= 编码器处理任务 =================
void vEncoderTask(void *pv) {
    // 中(2) | 任意核心
    // 旋转：A/B 相已在 GPIO 中断里做四态正交解码（见 hardware.cpp），本任务只消费累计格数——
    //       一次可能拿到多格（转得快时），按净位移更新选中项，不会丢步。
    // 按键：SW 是慢事件，仍在本任务轮询 + ENCODER_DEBOUNCE_MS 消抖；短按两段式
    //       （确认方案 → CONFIRM 发牌）；DEALING 出错短按重置；
    //       GAME_ACTIVE 长按确认结束并直接回 IDLE（短按与屏幕主体预留给后续功能）；
    // 旋转只在 IDLE 生效（切换/取消方案），其他状态旋转无反应
    bool press_active = false;
    uint32_t sw_press_ms = 0;    // 本次按下起始时间（长按判定）
    bool sw_long_done = false;   // 本次按下是否已触发过长按动作

    uint8_t prev_sw = 0xFF;      // SW 状态：0=按下(低)，1=松开
    uint32_t sw_last_change = 0; // SW 最近一次状态变化时间（消抖）

    Serial.printf("[ENC] A=%d B=%d SW=%d（A/B 走中断，SW 轮询）\n",
                  digitalRead(PIN_ENC_A), digitalRead(PIN_ENC_B),
                  digitalRead(PIN_ENC_SW));
    encoder_reset_steps();       // 丢弃开机/自检期间的转动

    for (;;) {
        uint32_t now_ms = millis();

        // 1) 旋转：消费中断累计的格数（净位移直接作用到选中项；非 IDLE 时只消费不响应）
        int32_t steps = encoder_take_steps();
        if (steps != 0 && state_get_current() == STATE_IDLE) {   // 交互仅 IDLE 生效
            int32_t n = (int32_t)deal_selection_get_scheme() + steps;
            n %= SCHEME_COUNT;
            if (n < 0) n += SCHEME_COUNT;
            Serial.printf("[ENC] rot %+ld sel=%ld\n", (long)steps, (long)n + 1);
            deal_selection_set_scheme((uint8_t)n);   // 旋转改变选择 → 自动回到未确认
            display_send_menu();
        }

        // 2) 轮询按键 SW（ENCODER_DEBOUNCE_MS 消抖）：按下为低电平
        uint8_t sw = (digitalRead(PIN_ENC_SW) == LOW) ? 0 : 1;
        if (sw != prev_sw && (uint32_t)(now_ms - sw_last_change) >= (uint32_t)ENCODER_DEBOUNCE_MS) {
            sw_last_change = now_ms;
            prev_sw = sw;
            system_state_t s = state_get_current();

            if (sw == 0) {
                // 按下
                Serial.println("[ENC] SW press");
                sw_press_ms = now_ms;
                sw_long_done = false;
                if (s == STATE_IDLE || s == STATE_GAME_ACTIVE ||
                    (s == STATE_DEALING && deal_error_active())) {
                    press_active = true;
                }
            } else {
                // 松开：长按已处理或按住超过长按阈值 → 不触发短按动作
                if (!press_active) continue;
                press_active = false;
                if (sw_long_done ||
                    (uint32_t)(now_ms - sw_press_ms) >= (uint32_t)ENCODER_LONG_PRESS_MS) {
                    continue;
                }

                if (s == STATE_IDLE) {
                    uint8_t sel = deal_selection_get_scheme();
                    if (!deal_selection_get_confirmed()) {
                        // 第一次按下：确认当前高亮方案（顶部显示，不进入发牌）
                        Serial.printf("[ENC] scheme %d selected\n", sel + 1);
                        deal_selection_set_confirmed(true);
                        display_cmd_t cmd = {};
                        cmd.type = DISPLAY_CMD_SELECT;
                        cmd.payload.menu.selectedIndex = sel;
                        cmd.payload.menu.confirmed = 1;
                        send_display_command(&cmd);
                    } else {
                        // 第二次按下（CONFIRM 最终确认）：进入 DEALING
                        Serial.printf("[ENC] confirm deal scheme %d\n", sel + 1);
                        char msg[32];
                        snprintf(msg, sizeof(msg), "Confirm: %d", sel + 1);
                        send_display_debug(msg);
                        xSemaphoreGive(xDealSemaphore);                       // 确认发牌
                        xEventGroupSetBits(xStateEventGroup, BIT_DEAL_CONFIRM); // IDLE→DEALING
                    }
                } else if (s == STATE_DEALING) {
                    // 发牌出错时按下重置到 IDLE；正常发牌中按下无反应
                    if (deal_error_active()) {
                        Serial.println("[ENC] reset from DEALING error");
                        xEventGroupSetBits(xStateEventGroup, BIT_RESET);
                    }
                }
            }
        }

        // 3) 长按动作：
        //   IDLE        → 切换发牌方式（顺序 ↔ 随机），屏幕顶部徽标即时更新
        //   GAME_ACTIVE → 确认结束并直接回 IDLE
        if (prev_sw == 0 && !sw_long_done &&
            (uint32_t)(now_ms - sw_press_ms) >= (uint32_t)ENCODER_LONG_PRESS_MS) {
            system_state_t ls = state_get_current();
            if (ls == STATE_GAME_ACTIVE) {
                sw_long_done = true;
                Serial.println("[ENC] long press: game end & reset to IDLE");
                xEventGroupSetBits(xStateEventGroup, BIT_RESET);
            } else if (ls == STATE_IDLE) {
                sw_long_done = true;
                bool rnd = !deal_selection_get_order_random();
                deal_selection_set_order_random(rnd);
                Serial.printf("[ENC] long press: deal order = %s\n",
                              rnd ? "RANDOM" : "SEQUENTIAL");
                display_send_menu();
            }
        }

        // 4) 让出 CPU（约 ENCODER_POLL_MS 周期；A/B 已在中断里解码，不需要 1ms 节拍）
        vTaskDelay(pdMS_TO_TICKS(ENCODER_POLL_MS));
    }
}

// ================= 屏幕显示任务 =================
void vDisplayTask(void *pv) {
    // 低(1) | 任意核心 | 队列触发（事件驱动，非轮询）
    // 注意：display_init() 已在 setup() 里调用（自检汇总屏要在显示任务启动前画出来），
    //       这里只处理显示命令。
    display_cmd_t cmd;
    for (;;) {
        if (xQueueReceive(xDisplayQueue, &cmd, portMAX_DELAY) == pdPASS) {
            display_handle_command(&cmd);
        }
    }
}

// ================= 系统监控任务 =================
void vMonitorTask(void *pv) {
    // 低(1) | 任意核心 | 定时器周期触发（500ms）
    TickType_t last = xTaskGetTickCount();
    uint32_t lastHeartbeat = 0;
    bool prevOnline = false;
    for (;;) {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(MONITOR_PERIOD_MS));
        busy_monitor();

        // ---- 子板心跳：周期发 CMD_STATUS_QUERY，子板回 EVT_STATUS ----
        // 判定依据：COMM_DEAD_TIMEOUT_MS 内是否收到过任何来自子板的帧
        uint32_t now = millis();
        if ((uint32_t)(now - lastHeartbeat) >= (uint32_t)COMM_HEARTBEAT_MS) {
            lastHeartbeat = now;
            proto_send(CMD_STATUS_QUERY, NULL, 0);
        }
        bool online = sub_comm_online();
        if (online != prevOnline) {
            prevOnline = online;
            Serial.printf("[MON] sub board %s\n", online ? "ONLINE" : "OFFLINE");
        }

        // TODO:
        //  - LED：空闲灭 / 旋转亮 / 故障快闪
        //  - 霍尔 / INDEX 每圈失步校准
        //  - 掉线时是否需要暂停发牌/屏幕告警，待与业务确认
    }
}
