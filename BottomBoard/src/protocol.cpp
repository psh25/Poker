/**
 * 底板 ↔ 子板 UART 协议（架构 v2 第七章，草案）
 * 帧：0xA5 | type | len | data | crc8(type+len+data) | 0xAA
 */
#include <Arduino.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "app_config.h"
#include "itc.h"
#include "protocol.h"
#include "hardware.h"

// ---- CRC-8（多项式 0x07，初值 0x00）----
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

// 发送：先入 TX 队列，由子板通信任务统一写串口
bool proto_send(uint8_t type, const uint8_t *data, uint8_t len) {
    if (len > PROTO_MAX_DATA) return false;
    proto_frame_t frame;
    frame.type = type;
    frame.len = len;
    if (data && len) memcpy(frame.data, data, len);
    return xQueueSend(xSubboardTxQueue, &frame, 0) == pdPASS;
}

// 子板通信任务调用：把一帧写入串口
void proto_write_frame(const proto_frame_t *frame) {
    uint8_t buf[64];   // 最大帧长 = 1+1+1+32+1+1 = 37B
    uint8_t n = 0;
    buf[n++] = PROTO_HEADER;
    buf[n++] = frame->type;
    buf[n++] = frame->len;
    if (frame->len) memcpy(&buf[n], frame->data, frame->len);
    n += frame->len;
    buf[n++] = proto_crc8(&buf[1], 2 + frame->len);   // 校验 type+len+data
    buf[n++] = PROTO_TAIL;
    Serial1.write(buf, n);
}

// ---- 接收解析状态机 ----
typedef enum { RX_IDLE, RX_TYPE, RX_LEN, RX_DATA, RX_CRC, RX_TAIL } rx_state_t;
static rx_state_t s_rx = RX_IDLE;
static proto_frame_t s_frame;
static uint8_t s_idx = 0;

void proto_rx_byte(uint8_t b) {
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
        if (b == PROTO_TAIL) proto_on_event(&s_frame);
        s_rx = RX_IDLE;
        break;
    default:
        s_rx = RX_IDLE;
        break;
    }
}

// 事件分发（架构 v2 4.1 触发源调整）：
//   EVT_CARD_OUT → xCardDetectedSem；EVT_READY → xDeckReadySem
//   所有事件 → xSubboardRxQueue（发牌控制任务消费）
//   EVT_CARD_VALUE 额外 → xCameraQueue（状态管理/显示）
void proto_on_event(const proto_frame_t *frame) {
    // 调试①：子板确认回执（EVT_ACK），回显收到的是哪条指令
    if (frame->type == EVT_ACK) {
        if (frame->len >= 1) {
            Serial.printf("[BOT] SUB-ACK: cmd=0x%02X", frame->data[0]);
            for (uint8_t i = 1; i < frame->len; i++) {
                Serial.printf(" %02X", frame->data[i]);
            }
            Serial.println();
        } else {
            Serial.println("[BOT] SUB-ACK: (empty)");
        }
        return;   // ACK 仅用于调试，不进业务队列
    }

    busy_subboard_event(frame->type, frame->data, frame->len);

    if (frame->type == EVT_CARD_OUT) {
        xSemaphoreGive(xCardDetectedSem);
    } else if (frame->type == EVT_READY) {
        xSemaphoreGive(xDeckReadySem);
    }

    xQueueSend(xSubboardRxQueue, frame, 0);
    if (frame->type == EVT_CARD_VALUE) {
        xQueueSend(xCameraQueue, frame, 0);
    }
}
