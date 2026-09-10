/**
 * 子板 —— 主程序（裸机：超级循环 + 中断）
 * 架构依据：docs/subboard_architecture.md
 *
 * 启动顺序：
 *   0. 双路调试串口（USB CDC + UART0/CH340）→ boot 横幅
 *   1. 硬件初始化（GPIO / UART / 中断）
 *   2. 上电自检 → 上报 EVT_READY
 *   3. （可选）自动连续发牌：AUTO_DEAL_ENABLE=1 时延时后自动一张接一张发牌
 *   4. 进入 IDLE，主循环：解析串口命令 → 执行状态机
 */
#include <Arduino.h>
#include <stdlib.h>
#include <string.h>

#include "app_config.h"
#include "debug.h"
#include "hardware.h"
#include "protocol.h"
#include "state_machine.h"

// ---- 调试 CLI：电脑串口直接向子板注入命令（复用协议解析/ACK/状态机）----
static bool sub_cli_hex_val(char c, uint8_t *v) {
    if (c >= '0' && c <= '9') { *v = (uint8_t)(c - '0'); return true; }
    if (c >= 'a' && c <= 'f') { *v = (uint8_t)(c - 'a' + 10); return true; }
    if (c >= 'A' && c <= 'F') { *v = (uint8_t)(c - 'A' + 10); return true; }
    return false;
}

static char    s_cli_buf[48];
static uint8_t s_cli_len = 0;

static void sub_cli_run(const char *line);   // 前向声明

static void cli_feed_byte(char c) {
    if (c == '\n' || c == '\r') {
        if (s_cli_len > 0) {
            s_cli_buf[s_cli_len] = '\0';
            sub_cli_run(s_cli_buf);
            s_cli_len = 0;
        }
    } else if (s_cli_len < sizeof(s_cli_buf) - 1) {
        s_cli_buf[s_cli_len++] = c;
    }
}

static void sub_cli_run(const char *line) {
    if (strcmp(line, "help") == 0) {
        // 命令别名规则：枚举名去掉前缀(CMD_/EVT_) → 全小写 → 去掉下划线
        dbg_println("[CLI] dealstart|stop|statusquery|selftest|reset|camcapture");
        dbg_println("[CLI] auto [n|off] | mtest | <hex type> [hex data...]");
        dbg_println("[CLI] auto        = 无限自动发牌");
        dbg_println("[CLI] auto <n>    = 自动发 n 张后停");
        dbg_println("[CLI] auto off    = 停止自动发牌（当前这张发完为止）");
        dbg_println("[CLI] mtest       = 电机自检（正转/反转各 300ms）");
        return;
    }

    // 电机自检：正转/反转各 300ms（核对引脚/供电/接线）
    if (strcmp(line, "mtest") == 0) {
        motor_self_test();
        return;
    }

    // 自动连续发牌（子板本地功能，不走协议）
    if (strncmp(line, "auto", 4) == 0) {
        const char *rest = line + 4;
        while (*rest == ' ' || *rest == '\t') rest++;
        if      (strcmp(rest, "off") == 0) sub_auto_deal_stop();
        else if (*rest == '\0')            sub_auto_deal_set(-1);   // 无限
        else                               sub_auto_deal_set(atol(rest));
        return;
    }

    uint8_t type = 0;
    const char *data_s = NULL;

    if      (strcmp(line, "dealstart") == 0)   type = CMD_DEAL_START;
    else if (strcmp(line, "stop") == 0)        type = CMD_STOP;
    else if (strcmp(line, "statusquery") == 0) type = CMD_STATUS_QUERY;
    else if (strcmp(line, "selftest") == 0)    type = CMD_SELF_TEST;
    else if (strcmp(line, "reset") == 0)       type = CMD_RESET;
    else if (strcmp(line, "camcapture") == 0)  type = CMD_CAM_CAPTURE;
    else {
        // 通用格式：<hex type> [hex data...]
        char buf[48];
        strncpy(buf, line, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        char *ts = strtok(buf, " \t");
        data_s = strtok(NULL, "");
        if (!ts) { dbg_println("[CLI] unknown (type 'help')"); return; }
        const char *p = ts;
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
        uint8_t hi, lo;
        if (!(p[0] && p[1] && sub_cli_hex_val(p[0], &hi) && sub_cli_hex_val(p[1], &lo))) {
            dbg_println("[CLI] unknown (type 'help')");
            return;
        }
        type = (uint8_t)((hi << 4) | lo);
    }

    uint8_t data[PROTO_MAX_DATA];
    uint8_t len = 0;
    if (data_s) {
        while (*data_s) {
            while (*data_s == ' ' || *data_s == '\t') data_s++;
            if (!*data_s) break;
            uint8_t hi, lo;
            if (!sub_cli_hex_val(*data_s, &hi) || !sub_cli_hex_val(*(data_s + 1), &lo)) {
                dbg_println("[CLI] data 非法 hex");
                return;
            }
            if (len >= PROTO_MAX_DATA) { dbg_println("[CLI] data 过长"); return; }
            data[len++] = (uint8_t)((hi << 4) | lo);
            data_s += 2;
        }
    }

    proto_frame_t frame;
    frame.type = type;
    frame.len = len;
    if (len) memcpy(frame.data, data, len);
    dbg_printf("[CLI] inject cmd=0x%02X len=%u\n", type, len);
    proto_on_command(&frame);
}

void setup() {
    dbg_init();                   // 0. 双路调试串口（USB CDC + CH340）
    dbg_println();
    dbg_println("[SUB] === boot ===");
    dbg_println("[SUB] dbg dual-output: USB CDC + UART0/CH340 @115200");
    dbg_println();

    sub_hardware_init();          // 1. GPIO / UART / 中断
    sub_self_test();              // 2. 自检 → EVT_READY
    // 2.5 打印当前配置：改完参数烧进去后，先看这一行对不对
    dbg_printf("[SUB] config: fwd=%dms brake=%dms rev=%dms pause=%dms duty=%d/%d auto=%s photo=%s\n",
               MOTOR_FWD_MS, MOTOR_BRAKE_MS, MOTOR_REV_MS, MOTOR_PAUSE_MS,
               MOTOR_DUTY, MOTOR_REV_DUTY,
               AUTO_DEAL_ENABLE ? "ON" : "OFF",
               USE_PHOTO_SENSOR ? "ON" : "OFF");
#if !AUTO_DEAL_ENABLE
    dbg_println("[WARN] AUTO_DEAL_ENABLE=0 -> 自动发牌未启动！想让它自己发牌请改成 1");
#endif
#if USE_PHOTO_SENSOR
    dbg_println("[WARN] USE_PHOTO_SENSOR=1 -> 在等光敏触发；没接光敏会 500ms 超时报卡牌停止");
#endif

    dbg_println("[SUB] boot ok, waiting for commands");
    dbg_println("[CLI] type 'help' for direct command injection");

#if AUTO_DEAL_ENABLE
    sub_auto_deal_arm(AUTO_DEAL_COUNT);   // 3. 自动连续发牌（调试用）
#endif
}

void loop() {
    // 1) 串口接收：中断已写入环形缓冲，这里把字节喂给协议解析器
    uint8_t b;
    while (sub_uart_read_byte(&b)) {
        proto_rx_byte(b);
    }
    sub_camera_service();   // 摄像头回传解析：触发脉冲收尾 / 结果上报

    // 2) 调试：USB CDC + CH340 双路读入命令行（模拟底板发来）
    while (Serial.available() > 0)  cli_feed_byte((char)Serial.read());
    while (Serial0.available() > 0) cli_feed_byte((char)Serial0.read());

    // 3) 执行裸机状态机（超级循环，全部超时基于 millis()）
    sub_state_run();
}
