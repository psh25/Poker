/**
 * 底板全部 RTOS 任务（架构 v2 第三章）
 * 时序要点：
 *   - 蓝牙：高(3)，BLE(NUS) RX 队列 + 帧解析，收主机命令 / 发状态帧
 *   - 子板通信：中(2)，固定核心 0，队列 + 串口轮询（20ms 节拍）
 *   - 发牌控制：中高(2)，固定核心 1，信号量触发；光敏 500ms / 摄像头 2s 超时
 *   - 编码器：中(2)，轮询 + 消抖，旋转切换/取消、短按确认
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
#include "ble_comms.h"

// ---- 主机（BLE 小程序）帧解析：与板间共用同一帧格式与解析器 ----
static proto_rx_t s_host_rx;
static void host_handle_frame(const proto_frame_t *frame);   // 完整主机命令帧 → 执行动作

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

// ================= 发牌控制：当前发牌运行状态（发牌任务写，CLI/编码器读）=================
static uint8_t  s_deal_scheme = 0;            // 当前方案（0-based）
static uint8_t  s_deal_deck = 0;              // 当前目标牌堆（0-based）
static uint8_t  s_deal_progress = 0;          // 0~100
static uint16_t s_dealt_count = 0;            // 已成功发出张数
static bool     s_deal_error_active = false;  // 是否处于发牌错误（等待编码器重置）
static uint8_t  s_deal_error_count = 0;
static char     s_deal_errors[DEAL_ERROR_MAX][24];

// 已发牌面数据（摄像头识别结果，按发牌顺序保存；占位，供后续蓝牙/小程序使用）
static uint8_t  s_dealt_cards[DEAL_TOTAL_CARDS][PROTO_MAX_DATA];
static uint8_t  s_dealt_lens[DEAL_TOTAL_CARDS];

// ================= 蓝牙通信任务 =================
void vBluetoothTask(void *pv) {
    // 高(3) | 任意核心 | 队列阻塞
    // BLE 初始化在 setup()（见 main.cpp）；本任务把 RX 特征收到的字节
    // 用与板间同一套帧解析器解析成主机命令，交给 host_handle_frame。
    proto_rx_init(&s_host_rx, host_handle_frame);
    bt_msg_t rx;
    for (;;) {
        if (xQueueReceive(xBluetoothRxQueue, &rx, pdMS_TO_TICKS(100)) == pdPASS) {
            for (uint8_t i = 0; i < rx.len; i++) {
                proto_rx_feed(&s_host_rx, rx.data[i]);
            }
        }
    }
}

// ================= 主机命令：BLE 帧与串口 CLI 共用同一动作（同一功能三种场景）=================
// 发一帧给主机（BLE 已连接才真正 notify，并打印调试）
static void host_send(const proto_frame_t *frame) {
    uint8_t buf[64];
    size_t n = proto_build_frame(buf, frame);
    if (ble_send(buf, n)) {
        Serial.printf("[HOST] TX type=0x%02X len=%u\n", frame->type, frame->len);
    }
}

// 回执（镜像子板 EVT_ACK 语义：data = 原 type + 原 data）
static void host_ack(const proto_frame_t *frame) {
    proto_frame_t ack = {};
    ack.type = HOST_EVT_ACK;
    ack.data[0] = frame->type;
    ack.len = 1;
    uint8_t copy = (frame->len < PROTO_MAX_DATA - 1) ? frame->len : (PROTO_MAX_DATA - 1);
    if (copy) memcpy(&ack.data[1], frame->data, copy);
    ack.len += copy;
    host_send(&ack);
}

// 状态通知（0x91：state / scheme / confirmed）
void host_notify_state(uint8_t state) {
    proto_frame_t evt = {};
    evt.type = HOST_EVT_STATE;
    evt.len = 3;
    evt.data[0] = state;
    evt.data[1] = display_get_selected();
    evt.data[2] = display_get_confirmed() ? 1 : 0;
    host_send(&evt);
}

static void host_action_select(uint8_t idx) {
    if (state_get_current() != STATE_IDLE) { Serial.println("[HOST] select: 仅 IDLE 有效"); return; }
    if (idx >= SCHEME_COUNT) { Serial.println("[HOST] select: 越界"); return; }
    display_set_selected(idx);
    display_set_confirmed(false);
    display_cmd_t cmd = {};
    cmd.type = DISPLAY_CMD_SELECT;
    cmd.payload.menu.selectedIndex = idx;
    cmd.payload.menu.confirmed = 0;
    send_display_command(&cmd);
}

static void host_action_confirm(void) {
    if (state_get_current() != STATE_IDLE) { Serial.println("[HOST] confirm: 仅 IDLE 有效"); return; }
    if (display_get_confirmed()) { Serial.println("[HOST] confirm: 已确认，再次发牌请发 0x01"); return; }
    display_set_confirmed(true);
    display_cmd_t cmd = {};
    cmd.type = DISPLAY_CMD_SELECT;
    cmd.payload.menu.selectedIndex = display_get_selected();
    cmd.payload.menu.confirmed = 1;
    send_display_command(&cmd);
}

static void host_action_deal_start(void) {
    if (state_get_current() != STATE_IDLE) { Serial.println("[HOST] deal: 仅 IDLE 可启动"); return; }
    xSemaphoreGive(xDealSemaphore);
    xEventGroupSetBits(xStateEventGroup, BIT_DEAL_CONFIRM);
}

static void host_action_stop(void) {
    proto_send(CMD_STOP, NULL, 0);                        // 停机子板
    xEventGroupSetBits(xStateEventGroup, BIT_RESET);      // 底板回 IDLE
}

static void host_action_reset(void) {
    proto_send(CMD_RESET, NULL, 0);                       // 复位子板状态机
    xEventGroupSetBits(xStateEventGroup, BIT_RESET);      // 底板回 IDLE
}

static void host_action_status(void) {
    host_notify_state((uint8_t)state_get_current());
}

// 完整主机命令帧分发（BLE / 未来串口二进制帧共用）
static void host_handle_frame(const proto_frame_t *frame) {
    Serial.printf("[HOST] RX type=0x%02X len=%u\n", frame->type, frame->len);
    switch (frame->type) {
    case CMD_DEAL_START:   host_action_deal_start(); break;
    case CMD_STOP:         host_action_stop();       break;
    case CMD_STATUS_QUERY: host_action_status();     break;
    case CMD_SELF_TEST:    proto_send(CMD_SELF_TEST, NULL, 0); break;  // 转发子板自检
    case CMD_RESET:        host_action_reset();      break;
    case HOST_CMD_SELECT_SCHEME:
        if (frame->len >= 1) host_action_select(frame->data[0]);
        break;
    case HOST_CMD_CONFIRM_SCHEME: host_action_confirm(); break;
    default:
        Serial.printf("[HOST] unknown type=0x%02X\n", frame->type);
        break;
    }
    host_ack(frame);   // 统一回执（与子板 ACK 同语义）
}

// ================= 调试 CLI：电脑串口 → 底板 / 子板 =================
static void sub_debug_print_help(void) {
    Serial.println("[CLI] help                         - 本帮助");
    Serial.println("[CLI] state                        - 打印当前状态机状态");
    Serial.println("[CLI] deal                         - 启动发牌（模拟 IDLE 确认，配合 sim）");
    Serial.println("[CLI] decks | dealinfo             - 打印牌堆预设 / 发牌进度与错误");
    Serial.println("[CLI] stop | reset                  - 停机回 IDLE / 复位（含子板）");
    Serial.println("[CLI] confirm                       - 确认当前方案（两段式第一步）");
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

    // deal：模拟 IDLE 下 CONFIRM，启动发牌（配合 sim cardvalue / sim cardout 联调）
    if (strcmp(line, "deal") == 0) {
        if (state_get_current() != STATE_IDLE) {
            Serial.println("[CLI] deal: 仅 IDLE 状态可启动（先 setstate idle 或编码器重置）");
            return;
        }
        host_action_deal_start();
        Serial.println("[CLI] deal started（0x01；流程：sim cardvalue <hex>... → sim cardout ...）");
        return;
    }

    // stop / reset / confirm：与 BLE 主机命令同动作（同一功能）
    if (strcmp(line, "stop") == 0) {
        host_action_stop();
        Serial.println("[CLI] stop（0x02）：已向子板发 CMD_STOP，底板回 IDLE");
        return;
    }
    if (strcmp(line, "reset") == 0) {
        host_action_reset();
        Serial.println("[CLI] reset（0x05）：已向子板发 CMD_RESET，底板回 IDLE");
        return;
    }
    if (strcmp(line, "confirm") == 0) {
        host_action_confirm();
        if (state_get_current() == STATE_IDLE && display_get_confirmed()) {
            Serial.println("[CLI] confirm（0x11）：方案已确认，再发 deal/0x01 开始发牌");
        }
        return;
    }

    // decks：打印牌堆角度与发牌顺序预设
    if (strcmp(line, "decks") == 0) {
        Serial.println("[CLI] deck angles (deg):");
        for (int i = 0; i < DECK_COUNT; i++) {
            Serial.printf("  deck %d -> %d\n", i + 1, (int)kDeckAngles[i]);
        }
        Serial.print("[CLI] deal sequence (TEST scheme):");
        for (int i = 0; i < DEAL_TOTAL_CARDS; i++) {
            Serial.printf(" %d", kDealDeckSequence[i] + 1);
        }
        Serial.println();
        Serial.println("[CLI]   note: 仅方案 4（TEST）有发牌模式，方案 1~3 未定义");
        return;
    }

    // dealinfo：发牌进度与错误（多条一并打印）
    if (strcmp(line, "dealinfo") == 0) {
        Serial.printf("[CLI] scheme=%u deck=%u dealt=%u/%u progress=%u%% error=%s\n",
                      s_deal_scheme + 1, s_deal_deck + 1, s_dealt_count,
                      (unsigned)DEAL_TOTAL_CARDS, s_deal_progress,
                      s_deal_error_active ? "YES" : "no");
        for (uint8_t i = 0; i < s_deal_error_count; i++) {
            Serial.printf("[CLI]   err[%u] %s\n", i, s_deal_errors[i]);
        }
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
        if (st == STATE_DEALING) {
            // 发牌屏由发牌任务刷新，CLI 强制进入时补一张测试屏
            display_cmd_t cmd = {};
            cmd.type = DISPLAY_CMD_DEALING;
            cmd.payload.dealing.scheme = display_get_selected();
            cmd.payload.dealing.deck = 1;
            cmd.payload.dealing.progress = 0;
            strncpy(cmd.payload.dealing.status, "setstate test", sizeof(cmd.payload.dealing.status) - 1);
            send_display_command(&cmd);
        }
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
        cmd.payload.menu.confirmed = display_get_confirmed() ? 1 : 0;
        send_display_command(&cmd);
        Serial.println("[CLI] idle screen sent");
        return;
    }
    if (strncmp(line, "select ", 7) == 0) {
        int n = atoi(line + 7);
        if (n < 1 || n > SCHEME_COUNT) { Serial.printf("[CLI] select: 范围 1~%d\n", SCHEME_COUNT); return; }
        host_action_select((uint8_t)(n - 1));
        Serial.printf("[CLI] select scheme %d（0x10）\n", n);
        return;
    }
    if (strncmp(line, "dealing ", 8) == 0) {
        int pct = atoi(line + 8);
        display_cmd_t cmd = {};
        cmd.type = DISPLAY_CMD_DEALING;
        cmd.payload.dealing.scheme = display_get_selected();
        cmd.payload.dealing.deck = 1;
        cmd.payload.dealing.progress = (uint8_t)pct;
        strncpy(cmd.payload.dealing.status, "screen test", sizeof(cmd.payload.dealing.status) - 1);
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
    static proto_rx_t s_sub_rx;
    proto_rx_init(&s_sub_rx, proto_on_event);

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
            proto_rx_feed(&s_sub_rx, (uint8_t)Serial1.read());
        }
        // TODO: 掉线检测：超过 COMM_DEAD_TIMEOUT_MS 无数据 → 通知发牌任务暂停
    }
}

// ================= 发牌控制任务 =================
static bool deal_error_active(void) { return s_deal_error_active; }

static void deal_add_error(const char *msg) {
    if (s_deal_error_count < DEAL_ERROR_MAX) {
        strncpy(s_deal_errors[s_deal_error_count], msg, sizeof(s_deal_errors[0]) - 1);
        s_deal_errors[s_deal_error_count][sizeof(s_deal_errors[0]) - 1] = '\0';
        s_deal_error_count++;
    }
}

// 发牌界面刷新：携带当前牌堆、进度、阶段状态与全部错误
static void deal_update_screen(const char *status) {
    display_cmd_t cmd = {};
    cmd.type = DISPLAY_CMD_DEALING;
    cmd.payload.dealing.scheme = s_deal_scheme;
    cmd.payload.dealing.deck = s_deal_deck + 1;
    cmd.payload.dealing.progress = s_deal_progress;
    strncpy(cmd.payload.dealing.status, status, sizeof(cmd.payload.dealing.status) - 1);
    for (uint8_t i = 0; i < s_deal_error_count && i < DEAL_ERROR_MAX; i++) {
        strncpy(cmd.payload.dealing.errors[i], s_deal_errors[i], sizeof(cmd.payload.dealing.errors[0]) - 1);
    }
    cmd.payload.dealing.errorCount = s_deal_error_count;
    send_display_command(&cmd);
}

// 发牌失败：向子板发停机 → 屏幕显示错误 → 停在 DEALING 等编码器重置
static void deal_fail(void) {
    s_deal_error_active = true;
    proto_send(CMD_STOP, NULL, 0);            // 底板向子板发送停机信息
    deal_update_screen("DEAL ERROR");
    xEventGroupSetBits(xStateEventGroup, BIT_DEAL_ERROR);
}

// 等待指定类型事件：忽略其他事件；子板 EVT_ERROR_* 记录错误后继续等；超时返回 false
static bool deal_wait_evt(QueueHandle_t q, uint8_t want, uint32_t timeoutMs, proto_frame_t *out) {
    uint32_t deadline = millis() + timeoutMs;
    while ((int32_t)(millis() - deadline) < 0) {
        proto_frame_t evt;
        if (xQueueReceive(q, &evt, pdMS_TO_TICKS(50)) != pdPASS) continue;
        if (evt.type == want) {
            if (out) *out = evt;
            return true;
        }
        // 子板错误事件：记录后立即返回 false（触发停机），不再等超时
        if (evt.type == EVT_ERROR_CARD_JAM)         { deal_add_error("sub: card jam"); return false; }
        else if (evt.type == EVT_ERROR_MOTOR_STALL) { deal_add_error("sub: motor stall"); return false; }
        else if (evt.type == EVT_ERROR_CAM_FAIL)    { deal_add_error("sub: cam fail"); return false; }
        // 其他事件（EVT_DEAL_DONE / EVT_CARD_VALUE 等）忽略，继续等目标事件
    }
    return false;
}

// 步进电机：当前未就绪，仅打印“切换到哪个牌堆”并等待到位（模拟）
static void debug_rotate_to_deck(uint8_t deck) {
    Serial.printf("[DEAL] 切换到牌堆 %u（角度 %d°），等待 %u ms 到位\n",
                  deck + 1, (int)kDeckAngles[deck], (unsigned)ROTATE_WAIT_MS);
    // TODO: TMC 步进旋转 + INDEX/霍尔到位确认；当前只打印 + 延时
    vTaskDelay(pdMS_TO_TICKS(ROTATE_WAIT_MS));
}

void vDealTask(void *pv) {
    // 中高(2) | 固定核心 1 | 信号量触发（IDLE 确认 / CLI deal）
    for (;;) {
        if (xSemaphoreTake(xDealSemaphore, portMAX_DELAY) != pdPASS) continue;

        // 新一轮发牌：清空错误/计数，锁存方案
        s_deal_error_count = 0;
        s_deal_error_active = false;
        s_dealt_count = 0;
        s_deal_progress = 0;
        s_deal_deck = 0;
        s_deal_scheme = display_get_selected();
        busy_deal_step("deal_start");

        // 仅“方案四（TEST）”已定义发牌模式；其余方案报“方案未定义”并停机等待编码器重置
        if (s_deal_scheme != SCHEME_TEST_INDEX) {
            deal_add_error("scheme undefined");
            deal_fail();
            continue;
        }

        bool prev_out = true;   // 第一张无需确认上一张

        for (uint16_t i = 0; i < DEAL_TOTAL_CARDS && !s_deal_error_active; i++) {
            uint8_t deck = kDealDeckSequence[i];
            s_deal_deck = deck;
            char status[24];
            proto_frame_t face;

            // 1) 摄像头识别当前（即将发出的）牌面 → EVT_CARD_VALUE
            snprintf(status, sizeof(status), "wait card %u face", (unsigned)i + 1);
            deal_update_screen(status);
            bool haveFace = deal_wait_evt(xCameraQueue, EVT_CARD_VALUE, CAMERA_TIMEOUT_MS, &face);
            if (!haveFace) {
                deal_add_error("card face timeout");   // 未识别到牌面 → 立即停机
                deal_fail();
                break;
            }

            // 2) 确认上一张牌成功发出（光敏 EVT_CARD_OUT），或这是第一张
            if (!prev_out) {
                deal_update_screen("wait prev card out");
                if (!deal_wait_evt(xSubboardRxQueue, EVT_CARD_OUT, PHOTO_TIMEOUT_MS, NULL)) {
                    deal_add_error("prev card not out");
                    deal_fail();
                    break;
                }
                prev_out = true;
            }

            // 3) 步进电机转到目标牌堆（打印调试信息 + 等待到位）
            snprintf(status, sizeof(status), "rotate deck %u", (unsigned)deck + 1);
            deal_update_screen(status);
            debug_rotate_to_deck(deck);

            // 4) 下发子板发牌指令
            snprintf(status, sizeof(status), "deal card %u", (unsigned)i + 1);
            deal_update_screen(status);
            if (!proto_send(CMD_DEAL_START, NULL, 0)) {
                deal_add_error("tx queue full");
                deal_fail();
                break;
            }

            // 5) 等待本张牌发出（光敏 EVT_CARD_OUT）
            if (!deal_wait_evt(xSubboardRxQueue, EVT_CARD_OUT, PHOTO_TIMEOUT_MS, NULL)) {
                deal_add_error("card not out");
                deal_fail();
                break;
            }
            prev_out = true;

            // 保存牌面数据（摄像头识别结果）
            xSemaphoreTake(xDeckDataMutex, portMAX_DELAY);
            if (haveFace) {
                uint8_t n = (face.len < PROTO_MAX_DATA) ? face.len : PROTO_MAX_DATA;
                memcpy(s_dealt_cards[i], face.data, n);
                s_dealt_lens[i] = n;
            } else {
                s_dealt_lens[i] = 0;
            }
            xSemaphoreGive(xDeckDataMutex);

            s_dealt_count++;
            s_deal_progress = (uint8_t)(s_dealt_count * 100U / DEAL_TOTAL_CARDS);
            snprintf(status, sizeof(status), "card %u ok", (unsigned)s_dealt_count);
            deal_update_screen(status);
        }

        if (s_deal_error_active) continue;   // 错误已显示，等待编码器重置后重新开始

        // 全部发完 → DEALING → GAME_ACTIVE
        xEventGroupSetBits(xStateEventGroup, BIT_DEAL_COMPLETE);
    }
}

// ================= 编码器处理任务 =================
void vEncoderTask(void *pv) {
    // 中(2) | 任意核心
    // 旋转：1ms 轮询 A/B 相做四态正交解码，累计满一整格（4 次有效跳变）才计一步，抗抖动/噪声
    // 按键：SW 轮询消抖；短按两段式（确认方案 → CONFIRM 发牌）；DEALING 出错/ GAME_END 短按重置；
    // 旋转只在 IDLE 生效（切换/取消方案），其他状态旋转无反应
    int8_t selection = 0;
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
                if (s == STATE_IDLE || s == STATE_GAME_END ||
                    (s == STATE_DEALING && deal_error_active())) {
                    press_active = true;
                }
            } else {
                // 松开：短按处理（第一次=确认方案，第二次=CONFIRM 发牌；DEALING 出错/ GAME_END=重置）
                if (!press_active) continue;
                press_active = false;

                if (s == STATE_IDLE) {
                    if (!display_get_confirmed()) {
                        // 第一次按下：确认当前高亮方案（顶部显示，不进入发牌）
                        Serial.printf("[ENC] scheme %d selected\n", selection + 1);
                        display_set_confirmed(true);
                        display_cmd_t cmd = {};
                        cmd.type = DISPLAY_CMD_SELECT;
                        cmd.payload.menu.selectedIndex = (uint8_t)selection;
                        cmd.payload.menu.confirmed = 1;
                        send_display_command(&cmd);
                    } else {
                        // 第二次按下（CONFIRM 最终确认）：进入 DEALING
                        Serial.printf("[ENC] confirm deal scheme %d\n", selection + 1);
                        char msg[32];
                        snprintf(msg, sizeof(msg), "Confirm: %d", selection + 1);
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
                } else if (s == STATE_GAME_END) {
                    xEventGroupSetBits(xStateEventGroup, BIT_RESET);           // GAME_END→IDLE
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
                    if (state_get_current() == STATE_IDLE) {   // 交互仅 IDLE 生效
                        selection = (int8_t)((selection + 1 + SCHEME_COUNT) % SCHEME_COUNT);
                        Serial.printf("[ENC] rot +1 sel=%d\n", selection + 1);
                        display_set_selected((uint8_t)selection);
                        display_set_confirmed(false);   // 旋转改变选择 → 回到未确认
                        display_cmd_t cmd = {};
                        cmd.type = DISPLAY_CMD_SELECT;
                        cmd.payload.menu.selectedIndex = (uint8_t)selection;
                        cmd.payload.menu.confirmed = 0;
                        send_display_command(&cmd);
                    }
                } else if (quad_accum <= -4) {
                    quad_accum = 0;
                    if (state_get_current() == STATE_IDLE) {   // 交互仅 IDLE 生效
                        selection = (int8_t)((selection - 1 + SCHEME_COUNT) % SCHEME_COUNT);
                        Serial.printf("[ENC] rot -1 sel=%d\n", selection + 1);
                        display_set_selected((uint8_t)selection);
                        display_set_confirmed(false);   // 旋转改变选择 → 回到未确认
                        display_cmd_t cmd = {};
                        cmd.type = DISPLAY_CMD_SELECT;
                        cmd.payload.menu.selectedIndex = (uint8_t)selection;
                        cmd.payload.menu.confirmed = 0;
                        send_display_command(&cmd);
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
