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
#include <string.h>

#include "app_config.h"
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

static void sub_cli_run(const char *line) {
    if (strcmp(line, "help") == 0) {
        Serial.println("[CLI] deal|stop|status|selftest|reset | <hex type> [hex data...]");
        return;
    }

    uint8_t type = 0;
    const char *data_s = NULL;

    if      (strcmp(line, "deal") == 0)     type = CMD_DEAL_START;
    else if (strcmp(line, "stop") == 0)     type = CMD_STOP;
    else if (strcmp(line, "status") == 0)   type = CMD_STATUS_QUERY;
    else if (strcmp(line, "selftest") == 0) type = CMD_SELF_TEST;
    else if (strcmp(line, "reset") == 0)    type = CMD_RESET;
    else {
        // 通用格式：<hex type> [hex data...]
        char buf[48];
        strncpy(buf, line, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        char *ts = strtok(buf, " \t");
        data_s = strtok(NULL, "");
        if (!ts) { Serial.println("[CLI] unknown (type 'help')"); return; }
        const char *p = ts;
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
        uint8_t hi, lo;
        if (!(p[0] && p[1] && sub_cli_hex_val(p[0], &hi) && sub_cli_hex_val(p[1], &lo))) {
            Serial.println("[CLI] unknown (type 'help')");
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
                Serial.println("[CLI] data 非法 hex");
                return;
            }
            if (len >= PROTO_MAX_DATA) { Serial.println("[CLI] data 过长"); return; }
            data[len++] = (uint8_t)((hi << 4) | lo);
            data_s += 2;
        }
    }

    proto_frame_t frame;
    frame.type = type;
    frame.len = len;
    if (len) memcpy(frame.data, data, len);
    Serial.printf("[CLI] inject cmd=0x%02X len=%u\n", type, len);
    proto_on_command(&frame);
}

void setup() {
    sub_hardware_init();          // 1. GPIO / UART / 中断
    sub_self_test();              // 2. 自检 → EVT_READY

    Serial.println("[SUB] boot ok, waiting for commands");
    Serial.println("[CLI] type 'help' for direct command injection");
}

void loop() {
    // 1) 串口接收：中断已写入环形缓冲，这里把字节喂给协议解析器
    uint8_t b;
    while (sub_uart_read_byte(&b)) {
        proto_rx_byte(b);
    }

    // 2) 调试：电脑串口直接注入命令（模拟底板发来）
    static char s_cli[48];
    static uint8_t s_cli_len = 0;
    while (Serial.available() > 0) {
        char c = (char)Serial.read();
        if (c == '\n' || c == '\r') {
            if (s_cli_len > 0) {
                s_cli[s_cli_len] = '\0';
                sub_cli_run(s_cli);
                s_cli_len = 0;
            }
        } else if (s_cli_len < sizeof(s_cli) - 1) {
            s_cli[s_cli_len++] = c;
        }
    }

    // 3) 执行裸机状态机（超级循环，全部超时基于 millis()）
    sub_state_run();
}
