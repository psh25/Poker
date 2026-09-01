/**
 * 子板 ↔ 底板 UART 协议（docs/subboard_architecture.md 第五章）
 * 帧：0xA5 | type | len | data | crc8(type+len+data) | 0xAA
 */
#include <Arduino.h>
#include <string.h>

#include "app_config.h"
#include "protocol.h"
#include "hardware.h"
#include "debug.h"
#include "state_machine.h"

// ---- CRC-8（多项式 0x07，初值 0x00，与底板一致）----
uint8_t proto_crc8(const uint8_t *buf, size_t len) {
    uint8_t crc = 0;
    for (size_t i = 0; i < len; i++) {
        crc ^= buf[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

// 上报事件：直接写串口（裸机，无队列）
bool proto_send(uint8_t type, const uint8_t *data, uint8_t len) {
    if (len > PROTO_MAX_DATA) return false;

    uint8_t buf[64];   // 最大帧长 = 1+1+1+32+1+1 = 37B
    uint8_t n = 0;
    buf[n++] = PROTO_HEADER;
    buf[n++] = type;
    buf[n++] = len;
    if (len && data) memcpy(&buf[n], data, len);
    n += len;
    buf[n++] = proto_crc8(&buf[1], 2 + len);   // 校验 type+len+data
    buf[n++] = PROTO_TAIL;
    Serial1.write(buf, n);
    return true;
}

// ---- 接收解析状态机 ----
typedef enum { RX_IDLE, RX_TYPE, RX_LEN, RX_DATA, RX_CRC, RX_TAIL } rx_state_t;
static rx_state_t s_rx = RX_IDLE;
static proto_frame_t s_frame;
static uint8_t s_idx = 0;
static bool s_uart_frame = false;  // frame came from Serial1 (bottom board), not CLI injection

void proto_rx_byte(uint8_t b) {
    sub_led_flash_rx_byte();  // any byte on Serial1 -> red LED flash
    switch (s_rx) {
    case RX_IDLE:
        if (b == PROTO_HEADER) s_rx = RX_TYPE;
        break;
    case RX_TYPE:
        s_frame.type = b;
        s_rx = RX_LEN;
        break;
    case RX_LEN:
        if (b > PROTO_MAX_DATA) { s_rx = RX_IDLE; break; }  // 非法长度，丢弃
        s_frame.len = b;
        s_idx = 0;
        s_rx = s_frame.len ? RX_DATA : RX_CRC;
        break;
    case RX_DATA:
        s_frame.data[s_idx++] = b;
        if (s_idx >= s_frame.len) s_rx = RX_CRC;
        break;
    case RX_CRC: {
        uint8_t crc = proto_crc8((const uint8_t *)&s_frame.type, 2 + s_frame.len);
        s_rx = (crc == b) ? RX_TAIL : RX_IDLE;   // 校验失败直接丢弃
        break;
    }
    case RX_TAIL:
        if (b == PROTO_TAIL) {
            s_uart_frame = true;
            sub_led_flash_frame_ok();  // complete frame + CRC OK -> green LED flash
            proto_on_command(&s_frame);
        }
        s_rx = RX_IDLE;
        break;
    default:
        s_rx = RX_IDLE;
        break;
    }
}

// 完整命令分发 → 子板状态机
void proto_on_command(const proto_frame_t *frame) {
    bool fromUart = s_uart_frame;
    s_uart_frame = false;
    // 调试①：收到并解析完整命令 → 串口打印 + 回发 ACK（原 type + 原 data 回显）
    dbg_printf("[SUB] RX: cmd=0x%02X len=%u data:", frame->type, frame->len);
    for (uint8_t i = 0; i < frame->len; i++) {
        dbg_printf(" %02X", frame->data[i]);
    }
    dbg_println();

    uint8_t ack[PROTO_MAX_DATA];
    ack[0] = frame->type;
    uint8_t ackLen = 1;
    uint8_t copyLen = (frame->len < PROTO_MAX_DATA - 1) ? frame->len : (PROTO_MAX_DATA - 1);
    if (copyLen) memcpy(&ack[1], frame->data, copyLen);
    ackLen += copyLen;

    if (proto_send(EVT_ACK, ack, ackLen)) {
        dbg_printf("[SUB] TX: ACK cmd=0x%02X len=%u\n", frame->type, ackLen);
        if (fromUart) sub_led_flash_ack_tx();  // ACK written -> blue LED flash
    } else {
        dbg_println("[SUB] TX: ACK send FAILED");
    }

    sub_state_handle_command(frame->type, frame->data, frame->len);
}

