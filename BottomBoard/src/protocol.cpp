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
#include "state_machine.h"
#include "tasks_deal.h"

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
// ---- 自检请求闩锁（P4）：区分"复检的 EVT_READY"与"子板自己重启的 EVT_READY" ----
// proto_send(CMD_SELF_TEST) 时置位；窗口内子板回的那一帧 EVT_READY 视为复检上报，取走即清空。
// 窗口只是兜底（防止"子板没回应、闩锁一直挂着"把下一次真正重启的 EVT_READY 也当成复检）。
#define SELFTEST_READY_WINDOW_MS 15000
static volatile uint32_t s_selftest_req_ms = 0;
static volatile bool     s_selftest_req = false;

bool proto_take_selftest_ready(void) {
    bool recent = s_selftest_req &&
                  (uint32_t)(millis() - s_selftest_req_ms) < (uint32_t)SELFTEST_READY_WINDOW_MS;
    s_selftest_req = false;      // 无论是否在窗口内都消费掉：一次请求只对应一帧 EVT_READY
    return recent;
}

bool proto_send(uint8_t type, const uint8_t *data, uint8_t len) {
    if (len > PROTO_MAX_DATA) return false;
    if (type == CMD_SELF_TEST) {          // 记录"刚请求过子板自检"（见上面 P4 说明）
        s_selftest_req_ms = millis();
        s_selftest_req = true;
    }
    proto_frame_t frame;
    frame.type = type;
    frame.len = len;
    if (data && len) memcpy(frame.data, data, len);
    return xQueueSend(xSubboardTxQueue, &frame, 0) == pdPASS;
}

// 子板通信任务调用：把一帧写入串口
void proto_write_frame(const proto_frame_t *frame) {
    uint8_t buf[64];   // 最大帧长 = 1+1+1+32+1+1 = 37B
    uint8_t n = (uint8_t)proto_build_frame(buf, frame);
    Serial1.write(buf, n);
}

// 组帧：帧头 | type | len | data | crc8 | 帧尾
size_t proto_build_frame(uint8_t *buf, const proto_frame_t *frame) {
    uint8_t n = 0;
    buf[n++] = PROTO_HEADER;
    buf[n++] = frame->type;
    buf[n++] = frame->len;
    if (frame->len) memcpy(&buf[n], frame->data, frame->len);
    n += frame->len;
    buf[n++] = proto_crc8(&buf[1], 2 + frame->len);   // 校验 type+len+data
    buf[n++] = PROTO_TAIL;
    return n;
}

// ---- 接收解析状态机 ----
typedef enum { RX_IDLE, RX_TYPE, RX_LEN, RX_DATA, RX_CRC, RX_TAIL } rx_state_t;

void proto_rx_init(proto_rx_t *rx, proto_frame_cb_t on_frame) {
    memset(rx, 0, sizeof(*rx));
    rx->state = RX_IDLE;
    rx->on_frame = on_frame;
}

void proto_rx_feed(proto_rx_t *rx, uint8_t b) {
    switch (rx->state) {
    case RX_IDLE:
        if (b == PROTO_HEADER) rx->state = RX_TYPE;
        break;
    case RX_TYPE:
        rx->frame.type = b;
        rx->state = RX_LEN;
        break;
    case RX_LEN:
        if (b > PROTO_MAX_DATA) { rx->state = RX_IDLE; break; }  // 非法长度，丢弃
        rx->frame.len = b;
        rx->idx = 0;
        rx->state = rx->frame.len ? RX_DATA : RX_CRC;
        break;
    case RX_DATA:
        rx->frame.data[rx->idx++] = b;
        if (rx->idx >= rx->frame.len) rx->state = RX_CRC;
        break;
    case RX_CRC: {
        uint8_t crc = proto_crc8((const uint8_t *)&rx->frame.type, 2 + rx->frame.len);
        rx->state = (crc == b) ? RX_TAIL : RX_IDLE;   // 校验失败直接丢弃
        break;
    }
    case RX_TAIL:
        if (b == PROTO_TAIL && rx->on_frame) rx->on_frame(&rx->frame);
        rx->state = RX_IDLE;
        break;
    default:
        rx->state = RX_IDLE;
        break;
    }
}

// 事件分发：
//   EVT_ACK / EVT_STATUS 仅调试与心跳用，不进业务队列；
//   其余业务事件（牌面 / 出牌 / 单张完成 / 错误）统一进 xSubboardRxQueue，
//   由发牌控制任务按 type 分发（避免错误事件进错队列导致等待方看不到）。
//
// ⚠️ 2026-09-18 修订（P1）：**只有"子板事件帧"（0x81~0x8F）才算子板在线的证据**。
//    以前是"收到任何帧都算在线"，于是只要底板自己的命令被回环（TX/RX 短接）或有人往这条线上
//    灌帧，即使子板断电也会显示 ONLINE，开局前的在线检查就形同虚设；这些回波帧还会进
//    xSubboardRxQueue，把真实的子板事件挤掉。现在底板加了一道与子板对称的 guard（见下）。
static volatile uint32_t s_last_sub_rx_ms = 0;
static volatile bool     s_sub_seen = false;
static volatile uint16_t s_sub_rx_foreign = 0;   // 被 guard 丢弃的"非子板帧"个数

// 帧类型是否属于"子板 → 底板"的事件域（0x81~0x8F）
bool proto_is_sub_event(uint8_t type) {
    return (type >= (uint8_t)PROTO_SUB_EVT_MIN) && (type <= (uint8_t)PROTO_SUB_EVT_MAX);
}

void proto_note_sub_rx(void) {
    s_last_sub_rx_ms = millis();
    s_sub_seen = true;
}

bool sub_comm_online(void) {
    if (!s_sub_seen) return false;
    return (uint32_t)(millis() - s_last_sub_rx_ms) < (uint32_t)COMM_DEAD_TIMEOUT_MS;
}

uint32_t sub_comm_last_rx_ms(void) {
    return s_last_sub_rx_ms;
}

uint16_t sub_comm_foreign_frames(void) {
    return s_sub_rx_foreign;
}

void proto_on_event(const proto_frame_t *frame) {
    // ---- 底板侧 guard：只接受子板事件帧（0x81~0x8F）----
    // 收到命令帧（0x01~0x06）、0x00 或 0x90+ 的主机域帧，说明这条线上有回环（TX/RX 短接）
    // 或外来设备在灌帧：丢弃，**不刷新在线时间、不进事件队列**，避免"假在线"和队列被挤满。
    if (!proto_is_sub_event(frame->type)) {
        s_sub_rx_foreign++;
        if (s_sub_rx_foreign <= 3) {   // 前几次打印，之后只统计（避免回环时刷屏）
            Serial.printf("[BOT] guard: drop type=0x%02X (not a sub event, loopback? total=%u)\n",
                          frame->type, (unsigned)s_sub_rx_foreign);
        }
        return;
    }

    proto_note_sub_rx();   // 只有子板事件帧才算“在线”

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

    // 心跳应答（EVT_STATUS）：打印状态，不进业务队列（避免占满 xSubboardRxQueue）
    if (frame->type == EVT_STATUS) {
        if (frame->len >= 4) {
            uint16_t count = (uint16_t)(frame->data[2] | (frame->data[3] << 8));
            Serial.printf("[BOT] SUB-STATUS: state=%u error=%u count=%u\n",
                          (unsigned)frame->data[0], (unsigned)frame->data[1], count);
        } else {
            Serial.println("[BOT] SUB-STATUS: (short)");
        }
        return;
    }

    // 子板自检结果（EVT_READY，开机自检 + CMD_SELF_TEST 复检都会发）：
    // 只打印位图，不进业务队列——没有业务消费它，留在队列里反而会挤掉后面的真实事件。
    if (frame->type == EVT_READY) {
        if (frame->len >= 2) {
            uint16_t b = (uint16_t)(frame->data[0] | ((uint16_t)frame->data[1] << 8));
            // 只打印"子板自己能判定"的项目（位定义见 SubBoard/include/protocol.h）
            Serial.printf("[BOT] SUB-READY: bits=0x%04X |%s%s%s\n", (unsigned)b,
                          (b & SUB_ST_CAM_CALIB) ? " cam-calib" : "",
                          (b & SUB_ST_HOST_UART) ? " host-uart" : "",
                          (b & SUB_ST_DONE)      ? " done"      : "");
        } else {
            Serial.println("[BOT] SUB-READY: (无结果位图，旧固件)");
        }

        // ---- P4：子板"自己重启"后与本板重新同步 ----
        // EVT_READY 有两个来源：① 子板开机自检完成（= 子板刚复位）；② 底板请求 CMD_SELF_TEST 后的复检上报。
        // 先用自检闩锁把 ② 排除；剩下的 ① 如果发生在**非 IDLE** 状态，说明本局已经不可能正常跑完
        //（底板还在等 EVT_CARD_OUT / EVT_DEAL_DONE，而子板已经回到它自己的 IDLE），
        // 于是立刻按"通信对端重启"收口，而不是干等到 8s 超时、期间还可能多推一张牌。
        if (!proto_take_selftest_ready() && state_get_current() != STATE_IDLE) {
            Serial.println("[BOT] sub board REBOOTED (EVT_READY while not IDLE) -> abort current round");
            deal_abort_remote("sub reset");
        }
        return;
    }

    busy_subboard_event(frame->type, frame->data, frame->len);

    // 所有业务事件（牌面 / 出牌 / 单张完成 / 错误）统一进同一个队列，
    // 由发牌任务按 type 分发——避免“错误进了另一个队列、等待方看不到”的问题。
    xQueueSend(xSubboardRxQueue, frame, 0);
}
