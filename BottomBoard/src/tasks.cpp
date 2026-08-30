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

// ================= 调试 CLI：电脑串口 → 底板 / 子板 =================
static void sub_debug_print_help(void) {
    Serial.println("[CLI] help                         - 本帮助");
    Serial.println("[CLI] state                        - 打印当前状态机状态");
    Serial.println("[CLI] setstate <idle|dealing|active|end> - 强制切换状态（调试）");
    Serial.println("[CLI] sub <cmd> [hex data...]      - 底板→子板（自动组帧+CRC）");
    Serial.println("[CLI]      cmd: deal|stop|status|selftest|reset 或 hex，如 sub 0x03");
    Serial.println("[CLI] sim <type> [hex data...]     - 模拟子板→底板事件（喂给协议分发）");
    Serial.println("[CLI]      type: ready|cardout|cardvalue|dealdone|jam|motorstall|camfail|ack 或 hex");
    Serial.println("[CLI] idle | select <n> | dealing <pct> - 底板屏幕测试");
}

static bool sub_debug_hex_val(char c, uint8_t *v) {
    if (c >= '0' && c <= '9') { *v = (uint8_t)(c - '0'); return true; }
    if (c >= 'a' && c <= 'f') { *v = (uint8_t)(c - 'a' + 10); return true; }
    if (c >= 'A' && c <= 'F') { *v = (uint8_t)(c - 'A' + 10); return true; }
    return false;
}

// 文本别名 / hex 解析为 type
static bool sub_debug_lookup_type(const char *s, uint8_t *type) {
    struct { const char *name; uint8_t type; } map[] = {
        {"deal", CMD_DEAL_START}, {"stop", CMD_STOP}, {"status", CMD_STATUS_QUERY},
        {"selftest", CMD_SELF_TEST}, {"reset", CMD_RESET},
    {"ready", EVT_READY}, {"cardout", EVT_CARD_OUT}, {"cardvalue", EVT_CARD_VALUE},
    {"dealdone", EVT_DEAL_DONE}, {"jam", EVT_ERROR_CARD_JAM}, {"motorstall", EVT_ERROR_MOTOR_STALL},
    {"camfail", EVT_ERROR_CAM_FAIL}, {"ack", EVT_ACK},
    };
    for (const auto &m : map) {
        if (strcmp(s, m.name) == 0) { *type = m.type; return true; }
    }
    const char *p = s;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
    uint8_t hi, lo;
    if (p[0] && p[1] && sub_debug_hex_val(p[0], &hi) && sub_debug_hex_val(p[1], &lo)) {
        *type = (uint8_t)((hi << 4) | lo);
        return true;
    }
    return false;
}

// 空格分隔的 hex 数据 → 字节数组；返回长度，0xFF 表示非法
static uint8_t sub_debug_parse_hex_data(const char *s, uint8_t *out, uint8_t maxLen) {
    uint8_t n = 0;
    while (*s) {
        while (*s == ' ' || *s == '\t') s++;
        if (!*s) break;
        uint8_t hi, lo;
        if (!sub_debug_hex_val(*s, &hi) || !sub_debug_hex_val(*(s + 1), &lo)) return 0xFF;
        if (n >= maxLen) return 0xFF;
        out[n++] = (uint8_t)((hi << 4) | lo);
        s += 2;
    }
    return n;
}

static void sub_debug_cli_process(const char *line) {
    if (strcmp(line, "help") == 0) { sub_debug_print_help(); return; }

    if (strcmp(line, "state") == 0) {
        static const char *names[] = {"IDLE", "DEALING", "GAME_ACTIVE", "GAME_END"};
        Serial.printf("[CLI] state = %s (%d)\n", names[state_get_current()], state_get_current());
        return;
    }

    // setstate <name|n>：调试用，强制切换状态（通过事件组交给状态管理任务执行）
    if (strncmp(line, "setstate ", 9) == 0) {
        struct { const char *name; system_state_t st; } map[] = {
            {"idle", STATE_IDLE}, {"dealing", STATE_DEALING},
            {"active", STATE_GAME_ACTIVE},
            {"end", STATE_GAME_END},
        };
        const char *p = line + 9;
        system_state_t st = (system_state_t)0xFF;
        for (const auto &m : map) {
            if (strcmp(p, m.name) == 0) { st = m.st; break; }
        }
        if (st == (system_state_t)0xFF) {
            int n = atoi(p);
            if (n >= STATE_IDLE && n <= STATE_GAME_END) st = (system_state_t)n;
        }
        if (st == (system_state_t)0xFF) {
            Serial.println("[CLI] setstate: idle|dealing|active|end（或 0~3）");
            return;
        }
        xEventGroupSetBits(xStateEventGroup, BIT_TEST_IDLE << st);  // 测试位连续
        Serial.printf("[CLI] setstate -> %d\n", (int)st);
        return;
    }

    // sub <cmd> [hex data...]：底板 → 子板，自动组帧 + CRC
    if (strncmp(line, "sub ", 4) == 0) {
        char buf[64];
        strncpy(buf, line + 4, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        char *type_s = strtok(buf, " \t");
        char *data_s = strtok(NULL, "");
        uint8_t type = 0;
        if (!type_s || !sub_debug_lookup_type(type_s, &type)) {
            Serial.println("[CLI] sub: 未知命令（type 'help'）");
            return;
        }
        uint8_t data[PROTO_MAX_DATA];
        uint8_t len = 0;
        if (data_s) {
            len = sub_debug_parse_hex_data(data_s, data, PROTO_MAX_DATA);
            if (len == 0xFF) { Serial.println("[CLI] sub: data 非法 hex"); return; }
        }
        if (proto_send(type, data, len)) {
            Serial.printf("[CLI] -> SUB type=0x%02X len=%u sent\n", type, len);
        } else {
            Serial.println("[CLI] sub: 发送队列满/失败");
        }
        return;
    }

    // sim <type> [hex data...]：模拟子板 → 底板，直接喂给协议分发
    if (strncmp(line, "sim ", 4) == 0) {
        char buf[64];
        strncpy(buf, line + 4, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        char *type_s = strtok(buf, " \t");
        char *data_s = strtok(NULL, "");
        uint8_t type = 0;
        if (!type_s || !sub_debug_lookup_type(type_s, &type)) {
            Serial.println("[CLI] sim: 未知类型（type 'help'）");
            return;
        }
        uint8_t data[PROTO_MAX_DATA];
        uint8_t len = 0;
        if (data_s) {
            len = sub_debug_parse_hex_data(data_s, data, PROTO_MAX_DATA);
            if (len == 0xFF) { Serial.println("[CLI] sim: data 非法 hex"); return; }
        }
        proto_frame_t frame;
        frame.type = type;
        frame.len = len;
        if (len) memcpy(frame.data, data, len);
        Serial.printf("[CLI] ~ sim EVT 0x%02X len=%u -> proto_on_event\n", type, len);
        proto_on_event(&frame);
        return;
    }

    // 屏幕测试命令
    if (strcmp(line, "idle") == 0) {
        display_cmd_t cmd = {};
        cmd.type = DISPLAY_CMD_IDLE;
        cmd.payload.menu.selectedIndex = display_get_selected();
        send_display_command(&cmd);
        Serial.println("[CLI] idle screen sent");
        return;
    }
    if (strncmp(line, "select ", 7) == 0) {
        int n = atoi(line + 7);
        if (n < 1 || n > SCHEME_COUNT) { Serial.printf("[CLI] select: 范围 1~%d\n", SCHEME_COUNT); return; }
        display_set_selected((uint8_t)(n - 1));
        display_cmd_t cmd = {};
        cmd.type = DISPLAY_CMD_SELECT;
        cmd.payload.menu.selectedIndex = (uint8_t)(n - 1);
        send_display_command(&cmd);
        Serial.printf("[CLI] select scheme %d\n", n);
        return;
    }
    if (strncmp(line, "dealing ", 8) == 0) {
        int pct = atoi(line + 8);
        display_cmd_t cmd = {};
        cmd.type = DISPLAY_CMD_DEALING;
        cmd.payload.dealing.progress = (uint8_t)pct;
        send_display_command(&cmd);
        Serial.printf("[CLI] dealing %d%%\n", pct);
        return;
    }

    Serial.printf("[CLI] unknown: '%s'\n", line);
    sub_debug_print_help();
}

// ================= 子板通信任务 =================
void vSubboardTask(void *pv) {
    // 中(2) | 固定核心 0 | 队列 + 串口中断/轮询
    proto_frame_t tx;
    static char s_cli_buf[64];
    static uint8_t s_cli_len = 0;

    Serial.println("[CLI] type 'help' for subboard command list");

    for (;;) {
        // 0) 调试②：解析底板 USB 串口命令行，转发给子板
        while (Serial.available() > 0) {
            char c = (char)Serial.read();
            if (c == '\n' || c == '\r') {
                if (s_cli_len > 0) {
                    s_cli_buf[s_cli_len] = '\0';
                    sub_debug_cli_process(s_cli_buf);
                    s_cli_len = 0;
                }
            } else if (s_cli_len < sizeof(s_cli_buf) - 1) {
                s_cli_buf[s_cli_len++] = c;
            }
        }

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
                    } else if (evt.type == EVT_ERROR_CARD_JAM) {
                        // TODO: 漏发重试一次，仍失败 → deal_error
                        ok = true;
                    } else if (evt.type == EVT_ERROR_MOTOR_STALL) {
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
    // 按键：SW 轮询消抖；短按确认（IDLE 直接发牌 / GAME_END 重置），长按无动作
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
                if (s == STATE_IDLE || s == STATE_GAME_END) {
                    press_start = now_ms;
                    press_active = true;
                }
            } else {
                // 松开：短按确认 / 长按取消
                if (!press_active) continue;
                press_active = false;

                uint32_t held = now_ms - press_start;
                // 长按(≥2s) 无动作（原 SELECTING 取消已随状态删除）
                if (held < ENCODER_LONG_PRESS_MS) {
                    if (s == STATE_IDLE) {
                        // IDLE 短按 = 确认发牌（用当前已选方案）
                        Serial.printf("[ENC] confirm scheme %d\n", selection + 1);
                        char msg[32];
                        snprintf(msg, sizeof(msg), "Confirm: %d", selection + 1);
                        send_display_debug(msg);
                        xSemaphoreGive(xDealSemaphore);                       // 确认发牌
                        xEventGroupSetBits(xStateEventGroup, BIT_DEAL_CONFIRM); // IDLE→DEALING
                    } else if (s == STATE_GAME_END) {
                        xEventGroupSetBits(xStateEventGroup, BIT_RESET);      // GAME_END→IDLE
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
                } else if (quad_accum <= -4) {
                    quad_accum = 0;
                    selection = (int8_t)((selection - 1 + SCHEME_COUNT) % SCHEME_COUNT);
                    Serial.printf("[ENC] rot -1 sel=%d\n", selection + 1);
                    display_set_selected((uint8_t)selection);
                    display_cmd_t cmd = {};
                    cmd.type = DISPLAY_CMD_SELECT;
                    cmd.payload.menu.selectedIndex = (uint8_t)selection;
                    send_display_command(&cmd);
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
