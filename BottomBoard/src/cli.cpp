/**
 * 调试 CLI（从 tasks.cpp 拆出）：电脑串口输入的命令行 → 底板动作 / 子板帧 / 模拟事件。
 * cli_poll() 由子板通信任务周期调用（读一行、执行一行）。
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
#include "deal_config.h"
#include "deal_selection.h"
#include "itc.h"
#include "tasks.h"
#include "cli.h"
#include "tasks_deal.h"
#include "host_actions.h"
#include "state_machine.h"
#include "protocol.h"
#include "display.h"
#include "hardware.h"

// ================= 调试 CLI：电脑串口 → 底板 / 子板 =================
// CLI 调试用发牌计划缓冲（与发牌任务的分开，避免互相覆盖）
static deal_plan_t s_cliPlan;

static void sub_debug_print_help(void) {
    Serial.println("[CLI] help                         - 本帮助");
    Serial.println("[CLI] state                        - 打印当前状态机状态");
    Serial.println("[CLI] dealstart                    - 启动发牌（模拟 IDLE 确认，配合 sim）");
    Serial.println("[CLI] decks | dealinfo             - 打印牌堆预设 / 发牌进度与错误");
    Serial.println("[CLI] game list | game info        - 列出牌局参数 / 当前计划（按当前发牌方式生成）");
    Serial.println("[CLI] game use <1-8|name>          - 选择预置牌局（等同 select）");
    Serial.println("[CLI] game order [seq|rand]        - 查看/设置发牌方式（同 IDLE 长按编码器）");
    Serial.println("[CLI] game custom players=3 hand=17 bottom=3 total=54  - 设 Custom 参数");
    Serial.println("[CLI] game random players=6 hand=2 public=5 total=52  - 同上并切到随机");
    Serial.println("[CLI] stop | reset                  - 停机回 IDLE / 复位（含子板）");
    Serial.println("[CLI] confirm                       - 确认当前方案（两段式第一步）");
    Serial.println("[CLI] subboard                      - 打印子板在线状态与心跳时间");
    Serial.println("[CLI] selftest                     - 打印底板自检结果 + 当前可读状态（不动作、不阻塞）");
    Serial.println("[CLI] sub selftest                 - 让子板重跑自检（含摄像头校准，约 2s 无响应）");
    Serial.println("[CLI] setstate <idle|dealing|active> - 强制切换状态（调试）");
    Serial.println("[CLI] sub <cmd> [hex data...]      - 底板→子板（自动组帧+CRC）");
    Serial.println("[CLI]      cmd: dealstart|stop|statusquery|selftest|reset|camcapture 或 hex");
    Serial.println("[CLI] sim <type> [hex data...]     - 模拟子板→底板事件（喂给协议分发）");
    Serial.println("[CLI]      type: ready|cardout|cardvalue|dealdone|errorcardjam|errormotorstall|errorcamfail|ack|status");
    Serial.println("[CLI] simauto [on|off]             - 方案四自动模拟摄像头(0.5s)/光敏(默认 on)");
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
        // 别名规则（两端统一）：枚举名去前缀(CMD_/EVT_) → 全小写 → 去下划线
        {"dealstart", CMD_DEAL_START}, {"stop", CMD_STOP}, {"statusquery", CMD_STATUS_QUERY},
        {"selftest", CMD_SELF_TEST}, {"reset", CMD_RESET}, {"camcapture", CMD_CAM_CAPTURE},
        {"ready", EVT_READY}, {"cardout", EVT_CARD_OUT}, {"cardvalue", EVT_CARD_VALUE},
        {"dealdone", EVT_DEAL_DONE},
        {"errorcardjam", EVT_ERROR_CARD_JAM}, {"errormotorstall", EVT_ERROR_MOTOR_STALL},
        {"errorcamfail", EVT_ERROR_CAM_FAIL}, {"ack", EVT_ACK}, {"status", EVT_STATUS},
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
        static const char *names[] = {"IDLE", "DEALING", "GAME_ACTIVE"};
        Serial.printf("[CLI] state = %s (%d)\n", names[state_get_current()], state_get_current());
        return;
    }

    // dealstart：启动发牌（要求先 select + confirm；测试用可开 simauto）
    if (strcmp(line, "dealstart") == 0) {
        if (state_get_current() != STATE_IDLE) {
            Serial.println("[CLI] dealstart: 仅 IDLE 状态可启动（先 setstate idle 或编码器重置）");
            return;
        }
        host_action_deal_start();
        Serial.println("[CLI] dealstart（0x01；需先 confirm，否则会被拒绝）");
        return;
    }

    // subboard：子板心跳状态（在线/离线 + 距上次收到子板帧的毫秒数）
    if (strcmp(line, "subboard") == 0) {
        bool on = sub_comm_online();
        uint32_t last = sub_comm_last_rx_ms();
        Serial.printf("[CLI] subboard: %s (last rx %lu ms ago)\n",
                      on ? "ONLINE" : "OFFLINE",
                      (unsigned long)(millis() - last));
        return;
    }

    // selftest：打印底板自检结果位图 + 当前可读状态。
    // 只读不动作：开机自检里的底盘微动、与子板握手都不在这里重跑（会阻塞通信任务）；
    // 想重新验证某项，开机重启即可；想让子板重跑自检用 `sub selftest`。
    if (strcmp(line, "selftest") == 0) {
        selftest_report();
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
        if (state_get_current() == STATE_IDLE && deal_selection_get_confirmed()) {
            Serial.println("[CLI] confirm（0x11）：方案已确认，再发 dealstart/0x01 开始发牌");
        }
        return;
    }

    // decks：打印牌堆角度与发牌顺序预设
    if (strcmp(line, "decks") == 0) {
        Serial.println("[CLI] deck angles (deg):");
        for (int i = 0; i < DECK_COUNT; i++) {
            Serial.printf("  deck %d -> %d\n", i + 1, (int)kDeckAngles[i]);
        }
        uint8_t sel = deal_selection_get_scheme();
        char err[48];
        if (deal_build_plan(&s_cliPlan, sel, deal_selection_get_order_random() ? DEAL_ORDER_RANDOM
                                                                        : DEAL_ORDER_SEQUENTIAL,
                            err, sizeof(err))) {
            deal_plan_print(sel, &s_cliPlan, deal_scheme_params(sel));
        } else {
            Serial.printf("[CLI] 计划生成失败：%s\n", err);
        }
        return;
    }

    // dealinfo：发牌进度与错误（多条一并打印）
    if (strcmp(line, "dealinfo") == 0) {
        deal_status_t st;
        deal_get_status(&st);
        Serial.printf("[CLI] scheme=%u deck=%u dealt=%u/%u progress=%u%% error=%s\n",
                      st.scheme + 1, st.deck + 1, st.dealt,
                      (unsigned)st.total, st.progress,
                      st.errorActive ? "YES" : "no");
        for (uint8_t i = 0; i < st.errorCount; i++) {
            Serial.printf("[CLI]   err[%u] %s\n", i, st.errors[i]);
        }
        return;
    }

    // game ...：参数驱动牌局（预置选择 / 自定义参数 / 查看发牌组）
    if (strcmp(line, "game") == 0 || strcmp(line, "game list") == 0) {
        Serial.printf("[CLI] deal order = %s（IDLE 下长按编码器切换）\n",
                      deal_selection_get_order_random() ? "RANDOM" : "SEQUENTIAL");
        Serial.println("[CLI] game list:");
        for (uint8_t i = 0; i < SCHEME_COUNT; i++) {
            const deal_params_t *p = deal_scheme_params(i);
            if (i == DEAL_INDEX_ROTATE_TEST) {
                Serial.printf("[CLI]  %u) %-10s rotate test（只转不发）\n",
                              (unsigned)i + 1, deal_scheme_name(i));
                continue;
            }
            if (!p || p->players == 0) {
                Serial.printf("[CLI]  %u) %-10s (未设置参数：game custom ...)\n",
                              (unsigned)i + 1, deal_scheme_name(i));
                continue;
            }
            Serial.printf("[CLI]  %u) %-10s %u人x%u 公共%u 底牌%u 总数%u → 需要%u 弃牌%d 牌堆%u\n",
                          (unsigned)i + 1, deal_scheme_name(i),
                          (unsigned)p->players, (unsigned)p->handCards,
                          (unsigned)p->publicCards, (unsigned)p->bottomCards,
                          (unsigned)p->totalCards,
                          (unsigned)deal_params_required(p), (int)deal_params_discard(p),
                          (unsigned)deal_params_piles(p));
        }
        return;
    }
    if (strcmp(line, "game info") == 0) {
        uint8_t sel = deal_selection_get_scheme();
        char err[48];
        if (deal_build_plan(&s_cliPlan, sel, deal_selection_get_order_random() ? DEAL_ORDER_RANDOM
                                                                        : DEAL_ORDER_SEQUENTIAL,
                            err, sizeof(err))) {
            deal_plan_print(sel, &s_cliPlan, deal_scheme_params(sel));
        } else {
            Serial.printf("[CLI] 计划生成失败：%s\n", err);
        }
        return;
    }
    if (strncmp(line, "game use ", 9) == 0) {
        const char *p = line + 9;
        int idx = -1;
        int n = atoi(p);
        if (n >= 1 && n <= SCHEME_COUNT) {
            idx = n - 1;
        } else {
            for (uint8_t i = 0; i < SCHEME_COUNT; i++) {
                if (strcmp(p, deal_scheme_name(i)) == 0) { idx = i; break; }
            }
        }
        if (idx < 0) {
            Serial.printf("[CLI] game use: 1~%d 或方案名（game list 查看）\n", SCHEME_COUNT);
            return;
        }
        host_action_select((uint8_t)idx);
        Serial.printf("[CLI] game -> %d) %s\n", idx + 1, deal_scheme_name((uint8_t)idx));
        return;
    }
    // game order [seq|rand]：查看/设置发牌方式（与方案无关，同 IDLE 长按）
    if (strcmp(line, "game order") == 0) {
        Serial.printf("[CLI] deal order = %s\n",
                      deal_selection_get_order_random() ? "RANDOM" : "SEQUENTIAL");
        return;
    }
    if (strncmp(line, "game order ", 11) == 0) {
        const char *p = line + 11;
        bool rnd;
        if      (strcmp(p, "seq") == 0 || strcmp(p, "sequential") == 0) rnd = false;
        else if (strcmp(p, "rand") == 0 || strcmp(p, "random") == 0)    rnd = true;
        else { Serial.println("[CLI] game order: seq | rand"); return; }
        deal_selection_set_order_random(rnd);
        display_send_menu();
        Serial.printf("[CLI] deal order -> %s\n", rnd ? "RANDOM" : "SEQUENTIAL");
        return;
    }

    // game custom/random <k=v ...>：设置 Custom 方案参数
    //   players=N 人数、hand=M 每人张数、public=P 公共、bottom=B 底牌、total=T 本局总牌数
    //   不写 total 时按“刚好够”（= 人数×每人+公共+底牌），即没有弃牌；
    //   game random 会把发牌方式一并切到随机。
    if (strncmp(line, "game custom", 11) == 0 || strncmp(line, "game random", 11) == 0) {
        bool forceRandom = (line[5] == 'r');
        deal_params_t params = { 0, 0, 0, 0, 0 };
        bool totalGiven = false;

        char buf[80];
        strncpy(buf, line + 11, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        for (char *tok = strtok(buf, " \t"); tok; tok = strtok(NULL, " \t")) {
            char key[16] = {0};
            int val = 0;
            if (sscanf(tok, "%15[^=]=%d", key, &val) != 2) continue;
            if (val < 0 || val > 255) { Serial.println("[CLI] 参数范围 0~255"); return; }
            if      (strcmp(key, "players") == 0) params.players = (uint8_t)val;
            else if (strcmp(key, "hand") == 0)    params.handCards = (uint8_t)val;
            else if (strcmp(key, "public") == 0)  params.publicCards = (uint8_t)val;
            else if (strcmp(key, "bottom") == 0)  params.bottomCards = (uint8_t)val;
            else if (strcmp(key, "total") == 0) { params.totalCards = (uint16_t)val; totalGiven = true; }
            else { Serial.printf("[CLI] 未知参数 %s\n", key); return; }
        }
        if (!totalGiven) params.totalCards = deal_params_required(&params);
        if (forceRandom) deal_selection_set_order_random(true);

        // 先在临时缓冲里试算：成功才写入 Custom 槽位
        char err[48];
        bool ok = deal_selection_get_order_random()
                      ? deal_plan_build_random(&s_cliPlan, "Custom", &params, err, sizeof(err))
                      : deal_plan_build_sequential(&s_cliPlan, "Custom", &params, err, sizeof(err));
        if (!ok) { Serial.printf("[CLI] 设置失败：%s\n", err); return; }

        *deal_scheme_params_custom() = params;
        host_action_select(DEAL_INDEX_CUSTOM);
        Serial.printf("[CLI] Custom: %u人x%u 公共%u 底牌%u 总数%u (order=%s)\n",
                      (unsigned)params.players, (unsigned)params.handCards,
                      (unsigned)params.publicCards, (unsigned)params.bottomCards,
                      (unsigned)params.totalCards,
                      deal_selection_get_order_random() ? "RANDOM" : "SEQUENTIAL");
        deal_plan_print(DEAL_INDEX_CUSTOM, &s_cliPlan, deal_scheme_params_custom());
        return;
    }

    // setstate <name|n>：调试用，强制切换状态（通过事件组交给状态管理任务执行）
    if (strncmp(line, "setstate ", 9) == 0) {
        struct { const char *name; system_state_t st; } map[] = {
            {"idle", STATE_IDLE}, {"dealing", STATE_DEALING},
            {"active", STATE_GAME_ACTIVE},
        };
        const char *p = line + 9;
        system_state_t st = (system_state_t)0xFF;
        for (const auto &m : map) {
            if (strcmp(p, m.name) == 0) { st = m.st; break; }
        }
        if (st == (system_state_t)0xFF) {
            int n = atoi(p);
            if (n >= STATE_IDLE && n <= STATE_GAME_ACTIVE) st = (system_state_t)n;
        }
        if (st == (system_state_t)0xFF) {
            Serial.println("[CLI] setstate: idle|dealing|active（或 0~2）");
            return;
        }
        xEventGroupSetBits(xStateEventGroup, BIT_TEST_IDLE << st);  // 测试位连续
        if (st == STATE_DEALING) {
            // 发牌屏由发牌任务刷新，CLI 强制进入时补一张测试屏
            display_cmd_t cmd = {};
            cmd.type = DISPLAY_CMD_DEALING;
            cmd.payload.dealing.scheme = deal_selection_get_scheme();
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

    // simauto [on|off]：方案四（TEST）自动模拟摄像头识别与光敏确认
    if (strcmp(line, "simauto") == 0 || strncmp(line, "simauto ", 8) == 0) {
        if (line[7] == ' ') {
            const char *p = line + 8;
            if (strcmp(p, "on") == 0)  { deal_sim_set_auto(true); }
            else if (strcmp(p, "off") == 0) { deal_sim_set_auto(false); }
            else {
                Serial.println("[CLI] simauto: on|off（或直接 simauto 查看状态）");
                return;
            }
        }
        Serial.printf("[CLI] simauto = %s（仅方案四 TEST 生效：摄像头 %u ms / 光敏 %u ms 自动注入）\n",
                      deal_sim_get_auto() ? "ON" : "OFF",
                      (unsigned)SIM_CAMERA_DELAY_MS, (unsigned)SIM_PHOTO_DELAY_MS);
        return;
    }

    // 屏幕测试命令
    if (strcmp(line, "idle") == 0) {
        display_send_idle();
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
        cmd.payload.dealing.scheme = deal_selection_get_scheme();
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

// ---- 调试串口输入：按行读取并执行（由子板通信任务周期调用）----
void cli_poll(void) {
    static char    buf[64];
    static uint8_t len = 0;

    while (Serial.available() > 0) {
        char c = (char)Serial.read();
        if (c == 10 || c == 13) {
            if (len > 0) {
                buf[len] = 0;
                sub_debug_cli_process(buf);
                len = 0;
            }
        } else if (len < sizeof(buf) - 1) {
            buf[len++] = c;
        }
    }
}
