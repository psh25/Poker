/**
 * 底板全部 RTOS 任务（架构 v2 第三章）
 * 时序要点：
 *   - 蓝牙：高(3)，队列阻塞，永久等待
 *   - 子板通信：中(2)，固定核心 0，队列 + 串口轮询（20ms 节拍）
 *   - 发牌控制：中高(2)，固定核心 1，信号量触发；光敏 500ms / 摄像头 2s 超时
 *   - 编码器：中(2)，中断 + 队列，5ms 消抖、2s 长按
 *   - 显示：低(1)，队列阻塞（事件驱动）
 *   - 状态管理：低(1)，事件组阻塞（协调器）
 *   - 监控：低(1)，500ms 周期巡检
 */
#include <Arduino.h>
#include <stdio.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/event_groups.h>

#include "app_config.h"
#include "itc.h"
#include "tasks.h"
#include "state_machine.h"
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

// ================= 蓝牙通信任务 =================
void vBluetoothTask(void *pv) {
    // 高(3) | 任意核心 | 队列阻塞（永久）
    // TODO: BLE 初始化（ESP32-S3 内置），连接小程序；协议待定义
    bt_msg_t rx;
    bt_msg_t tx;
    for (;;) {
        // 接收：小程序指令（选牌、查询状态）
        if (xQueueReceive(xBluetoothRxQueue, &rx, pdMS_TO_TICKS(10)) == pdPASS) {
            // TODO: 选牌指令 → 显示预览；查询指令 → 状态管理
        }
        // 发送：牌堆信息、发牌状态
        if (xQueueReceive(xBluetoothTxQueue, &tx, 0) == pdPASS) {
            // TODO: 经 BLE 发送
        }
        busy_bluetooth();
    }
}

// ================= 子板通信任务 =================
void vSubboardTask(void *pv) {
    // 中(2) | 固定核心 0 | 队列 + 串口中断/轮询
    proto_frame_t tx;
    for (;;) {
        // 1) 下发指令（20ms 超时后继续处理接收，避免收不到上行）
        if (xQueueReceive(xSubboardTxQueue, &tx, pdMS_TO_TICKS(20)) == pdPASS) {
            proto_write_frame(&tx);
        }
        // 2) 接收并解析子板上报（事件由 proto_on_event 分发）
        while (Serial1.available() > 0) {
            proto_rx_byte((uint8_t)Serial1.read());
        }
        // TODO: 掉线检测：超过 COMM_DEAD_TIMEOUT_MS 无数据 → 通知发牌任务暂停
    }
}

// ================= 发牌控制任务 =================
void vDealTask(void *pv) {
    // 中高(2) | 固定核心 1 | 信号量触发（编码器确认）
    for (;;) {
        if (xSemaphoreTake(xDealSemaphore, portMAX_DELAY) != pdPASS) continue;

        busy_deal_step("deal_start");
        uint16_t dealt = 0;
        uint16_t total = 1;   // TODO: 从方案参数读取总张数（牌堆数 × 每堆张数）

        while (dealt < total) {
            // 1) 旋转到目标牌堆（TMC 步进；旋转前拉低 STDBY 唤醒驱动）
            busy_deal_step("rotate_to_deck");

            // 2) 下发子板发牌指令
            if (!proto_send(CMD_DEAL_START, NULL, 0)) {
                // TODO: TX 队列满 / 子板掉线处理
            }

            // 3) 等待 EVT_CARD_OUT（光敏成功，500ms 超时 = 疑似漏发）
            proto_frame_t evt;
            bool ok = false;
            while (!ok) {
                if (xQueueReceive(xSubboardRxQueue, &evt, pdMS_TO_TICKS(PHOTO_TIMEOUT_MS)) == pdPASS) {
                    if (evt.type == EVT_CARD_OUT) {
                        ok = true;
                    } else if (evt.type == ERROR_CARD_JAM) {
                        // TODO: 漏发重试一次，仍失败 → deal_error
                        ok = true;
                    } else if (evt.type == ERROR_MOTOR_STALL) {
                        goto deal_error;
                    }
                } else {
                    goto deal_error;   // 光敏超时
                }
            }

            // 4) 等待 EVT_CARD_VALUE（摄像头识别，2s 超时）
            if (xQueueReceive(xCameraQueue, &evt, pdMS_TO_TICKS(CAMERA_TIMEOUT_MS)) == pdPASS) {
                if (evt.type == EVT_CARD_VALUE) {
                    xSemaphoreTake(xDeckDataMutex, portMAX_DELAY);
                    // TODO: 牌堆写入 deckData[deckIdx].cards[dealt] = evt.data（花色/点数）
                    xSemaphoreGive(xDeckDataMutex);
                }
            } else {
                // 识别超时：按未知牌处理（架构 v2 8.1）
            }
            dealt++;

            // 5) 进度显示
            display_cmd_t cmd = {};
            cmd.type = DISPLAY_CMD_DEALING;
            cmd.payload.dealing.progress = (uint8_t)(dealt * 100U / total);
            strncpy(cmd.payload.dealing.status, "发牌中", sizeof(cmd.payload.dealing.status));
            send_display_command(&cmd);
        }

        // 全部发完 → 状态机 DEALING → GAME_ACTIVE
        xEventGroupSetBits(xStateEventGroup, BIT_DEAL_COMPLETE);
        continue;

    deal_error:
        // 发牌异常（漏发/多发/卡牌）→ DEALING → IDLE，等待重置
        xEventGroupSetBits(xStateEventGroup, BIT_DEAL_ERROR);
    }
}

// ================= 编码器处理任务 =================
void vEncoderTask(void *pv) {
    // 中(2) | 任意核心
    // 旋转：1ms 轮询 A/B 相做四态正交解码，累计满一整格（4 次有效跳变）才计一步，抗抖动/噪声
    // 按键：SW 中断入队；短按确认，长按(>2s)取消返回
    int8_t selection = 0;
    uint32_t press_start = 0;
    bool press_active = false;

    uint8_t prev_state = 0xFF;   // 上次 A/B 组合状态（A<<1|B）
    int8_t quad_accum = 0;       // 同方向累计有效跳变（满 ±4 = 一格）
    uint8_t prev_sw = 0xFF;      // SW 状态：0=按下(低)，1=松开
    uint32_t sw_last_change = 0; // SW 最近一次状态变化时间（10ms 消抖）

    Serial.printf("[ENC] A=%d B=%d SW=%d\n",
                  digitalRead(PIN_ENC_A), digitalRead(PIN_ENC_B),
                  digitalRead(PIN_ENC_SW));

    for (;;) {
        // 1) 轮询按键 SW（10ms 消抖）：按下为低电平
        uint8_t sw = (digitalRead(PIN_ENC_SW) == LOW) ? 0 : 1;
        uint32_t now_ms = millis();
        if (sw != prev_sw && (now_ms - sw_last_change) >= 10) {
            sw_last_change = now_ms;
            prev_sw = sw;
            system_state_t s = state_get_current();

            if (sw == 0) {
                // 按下
                Serial.println("[ENC] SW press");
                if (s == STATE_SELECTING || s == STATE_GAME_END) {
                    press_start = now_ms;
                    press_active = true;
                }
            } else {
                // 松开：短按确认 / 长按取消
                if (!press_active) continue;
                press_active = false;

                uint32_t held = now_ms - press_start;
                if (held >= ENCODER_LONG_PRESS_MS) {
                    if (s == STATE_SELECTING) {
                        Serial.println("[ENC] cancel");
                        send_display_debug("Cancelled");
                        xEventGroupSetBits(xStateEventGroup, BIT_CANCEL);
                    }
                } else {
                    if (s == STATE_SELECTING) {
                        Serial.printf("[ENC] confirm scheme %d\n", selection + 1);
                        char msg[32];
                        snprintf(msg, sizeof(msg), "Selected: %d", selection + 1);
                        send_display_debug(msg);
                        xSemaphoreGive(xDealSemaphore);                       // 确认发牌
                        xEventGroupSetBits(xStateEventGroup, BIT_USER_INPUT); // SELECTING→DEALING
                    } else if (s == STATE_GAME_END) {
                        xEventGroupSetBits(xStateEventGroup, BIT_RESET);      // GAME_END→IDLE
                    } else if (s == STATE_IDLE) {
                        xEventGroupSetBits(xStateEventGroup, BIT_USER_INPUT); // IDLE→SELECTING
                    }
                }
            }
        }

        // 2) 1ms 轮询 A/B 相正交解码
        uint8_t st = (digitalRead(PIN_ENC_A) ? 2 : 0) | (digitalRead(PIN_ENC_B) ? 1 : 0);
        if (st != prev_state) {
            if (prev_state != 0xFF) {
                int8_t d = 0;
                switch ((prev_state << 2) | st) {
                    case 0b0001: case 0b0111: case 0b1110: case 0b1000: d = +1; break;
                    case 0b0010: case 0b0100: case 0b1101: case 0b1011: d = -1; break;
                    default: quad_accum = 0; break;   // 非法跳变（噪声）→ 清零
                }
                quad_accum += d;

                if (quad_accum >= 4) {
                    quad_accum = 0;
                    selection = (int8_t)((selection + 1 + SCHEME_COUNT) % SCHEME_COUNT);
                    Serial.printf("[ENC] rot +1 sel=%d\n", selection + 1);
                    display_set_selected((uint8_t)selection);
                    display_cmd_t cmd = {};
                    cmd.type = DISPLAY_CMD_SELECT;
                    cmd.payload.menu.selectedIndex = (uint8_t)selection;
                    send_display_command(&cmd);
                    if (state_get_current() == STATE_IDLE) {
                        xEventGroupSetBits(xStateEventGroup, BIT_USER_INPUT); // IDLE→SELECTING
                    }
                } else if (quad_accum <= -4) {
                    quad_accum = 0;
                    selection = (int8_t)((selection - 1 + SCHEME_COUNT) % SCHEME_COUNT);
                    Serial.printf("[ENC] rot -1 sel=%d\n", selection + 1);
                    display_set_selected((uint8_t)selection);
                    display_cmd_t cmd = {};
                    cmd.type = DISPLAY_CMD_SELECT;
                    cmd.payload.menu.selectedIndex = (uint8_t)selection;
                    send_display_command(&cmd);
                    if (state_get_current() == STATE_IDLE) {
                        xEventGroupSetBits(xStateEventGroup, BIT_USER_INPUT); // IDLE→SELECTING
                    }
                }
            }
            prev_state = st;
        }

        // 3) 让出 CPU（约 1ms 周期）
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

// ================= 屏幕显示任务 =================
void vDisplayTask(void *pv) {
    // 低(1) | 任意核心 | 队列触发（事件驱动，非轮询）
    display_init();   // 初始化屏幕（ILI9341，320x240）
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
    for (;;) {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(MONITOR_PERIOD_MS));
        busy_monitor();
        // TODO:
        //  - 读取 TMC 温度 / SG_RESULT，超阈值 → ENN 断电 + 屏幕告警（架构 v2 8.2）
        //  - 每 COMM_HEARTBEAT_MS 发 CMD_STATUS_QUERY；超 COMM_DEAD_TIMEOUT_MS 判定掉线
        //  - LED：空闲灭 / 旋转亮 / 故障快闪
        //  - 霍尔 / INDEX 每圈失步校准
    }
}
