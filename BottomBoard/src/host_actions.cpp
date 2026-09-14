/**
 * 主机（BLE 小程序 / 串口 CLI）命令动作 + 蓝牙接收任务（从 tasks.cpp 拆出）。
 * 同一套动作被"串口 CLI、BLE、板间协议"三种场景复用。
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
#include "tasks_deal.h"
#include "host_actions.h"
#include "state_machine.h"
#include "protocol.h"
#include "display.h"
#include "hardware.h"
#include "ble_comms.h"

// ---- 主机（BLE 小程序）帧解析：与板间共用同一帧格式与解析器 ----
static proto_rx_t s_host_rx;
static void host_handle_frame(const proto_frame_t *frame);   // 完整主机命令帧 → 执行动作

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
    evt.data[1] = deal_selection_get_scheme();
    evt.data[2] = deal_selection_get_confirmed() ? 1 : 0;
    host_send(&evt);
}

// 小程序牌面编码 → 底板 BLE 事件编码。
// 底板内部 cardCode: A,2,3..K / 小王 / 大王；小程序 rank: 3..15,16,17。
static void host_card_code_to_suit_rank(uint8_t code, uint8_t *suit, uint8_t *rank) {
    if (code <= 51) {
        *suit = (uint8_t)(code / 13);
        const uint8_t rankIdx = (uint8_t)(code % 13);
        if (rankIdx == CARD_RANK_A)      *rank = 14;  // A
        else if (rankIdx == CARD_RANK_2) *rank = 15;  // 2
        else                              *rank = (uint8_t)(rankIdx + 1); // 3..K
    } else if (code == CARD_JOKER_SMALL) {
        *suit = 4; *rank = 16;
    } else if (code == CARD_JOKER_BIG) {
        *suit = 4; *rank = 17;
    } else {
        *suit = 0xFF; *rank = 0xFF;  // 背面 / 未知
    }
}

void host_notify_card(uint16_t idx, uint8_t pile, uint8_t cardCode, uint8_t src) {
    uint8_t suit = 0xFF;
    uint8_t rank = 0xFF;
    host_card_code_to_suit_rank(cardCode, &suit, &rank);

    proto_frame_t evt = {};
    evt.type = HOST_EVT_CARD;
    evt.len = 6;
    evt.data[0] = (uint8_t)(idx & 0xFF);
    evt.data[1] = (uint8_t)((idx >> 8) & 0xFF);
    evt.data[2] = pile;
    evt.data[3] = suit;
    evt.data[4] = rank;
    evt.data[5] = (src == CARD_SRC_DEBUG) ? 1 : 0;
    host_send(&evt);
}

void host_notify_deal_done(uint16_t total) {
    proto_frame_t evt = {};
    evt.type = HOST_EVT_DEAL_DONE;
    evt.len = 2;
    evt.data[0] = (uint8_t)(total & 0xFF);
    evt.data[1] = (uint8_t)((total >> 8) & 0xFF);
    host_send(&evt);
}

static void host_action_plan_begin(const uint8_t *data, uint8_t len) {
    if (state_get_current() != STATE_IDLE || len < 4) return;
    const uint8_t scheme = data[0];
    const uint16_t total = (uint16_t)data[1] | ((uint16_t)data[2] << 8);
    const uint8_t pileCount = data[3];
    if (!deal_host_plan_begin(scheme, total, pileCount)) {
        Serial.println("[HOST] plan begin rejected");
    }
}

static void host_action_plan_chunk(const uint8_t *data, uint8_t len) {
    if (state_get_current() != STATE_IDLE || len < 2) return;
    if (!deal_host_plan_chunk(data[0], &data[1], (uint8_t)(len - 1))) {
        Serial.println("[HOST] plan chunk rejected");
    }
}

static void host_action_plan_commit(void) {
    if (state_get_current() != STATE_IDLE) return;
    if (!deal_host_plan_commit()) Serial.println("[HOST] plan commit rejected");
}

static void host_action_play_text(const uint8_t *data, uint8_t len) {
    display_cmd_t cmd = {};
    cmd.type = DISPLAY_CMD_GAME_ACTIVE;
    uint8_t n = (len < sizeof(cmd.payload.game.cardInfo) - 1)
                    ? len : (uint8_t)(sizeof(cmd.payload.game.cardInfo) - 1);
    if (n) memcpy(cmd.payload.game.cardInfo, data, n);
    cmd.payload.game.cardInfo[n] = '\0';
    send_display_command(&cmd);
}
void host_action_select(uint8_t idx) {
    if (state_get_current() != STATE_IDLE) { Serial.println("[HOST] select: 仅 IDLE 有效"); return; }
    if (idx >= SCHEME_COUNT) { Serial.println("[HOST] select: 越界"); return; }
    deal_selection_set_scheme(idx);
    display_send_menu();      // 改选会清掉 confirmed，这里统一刷新菜单屏
}

void host_action_confirm(void) {
    if (state_get_current() != STATE_IDLE) { Serial.println("[HOST] confirm: 仅 IDLE 有效"); return; }
    if (deal_selection_get_confirmed()) { Serial.println("[HOST] confirm: 已确认，再次发牌请发 0x01"); return; }
    deal_selection_set_confirmed(true);
    display_send_menu();
}

void host_action_deal_start(void) {
    if (state_get_current() != STATE_IDLE) { Serial.println("[HOST] deal: 仅 IDLE 可启动"); return; }
    if (!deal_selection_get_confirmed()) {
        Serial.println("[HOST] deal: 方案未确认（先 select + confirm）");
        return;
    }
    xSemaphoreGive(xDealSemaphore);
    xEventGroupSetBits(xStateEventGroup, BIT_DEAL_CONFIRM);
}

void host_action_stop(void) {
    motion_abort_request();                               // 立即中止正在执行的发牌/转动
    deal_host_plan_clear();
    // 停机子板；无子板模式（subsim on / USE_SUBBOARD=0）不下发动作类命令
    if (!deal_sim_get_auto()) proto_send(CMD_STOP, NULL, 0);
    xEventGroupSetBits(xStateEventGroup, BIT_RESET);      // 底板回 IDLE
}

void host_action_reset(void) {
    motion_abort_request();                               // 立即中止正在执行的发牌/转动
    deal_host_plan_clear();
    if (!deal_sim_get_auto()) proto_send(CMD_RESET, NULL, 0);   // 复位子板状态机（无子板模式跳过）
    xEventGroupSetBits(xStateEventGroup, BIT_RESET);      // 底板回 IDLE
}

void host_action_status(void) {
    host_notify_state((uint8_t)state_get_current());
}

// 完整主机命令帧分发（BLE / 未来串口二进制帧共用）
static void host_handle_frame(const proto_frame_t *frame) {
    Serial.printf("[HOST] RX type=0x%02X len=%u\n", frame->type, frame->len);
    switch (frame->type) {
    case CMD_DEAL_START:   host_action_deal_start(); break;
    case CMD_STOP:         host_action_stop();       break;
    case CMD_STATUS_QUERY: host_action_status();     break;
    case CMD_SELF_TEST:    // 转发子板自检；无子板模式下没有对象可自检
        if (deal_sim_get_auto()) Serial.println("[HOST] selftest: 无子板模式，跳过（subsim off 恢复）");
        else                     proto_send(CMD_SELF_TEST, NULL, 0);
        break;
    case CMD_RESET:        host_action_reset();      break;
    case HOST_CMD_SELECT_SCHEME:
        if (frame->len >= 1) host_action_select(frame->data[0]);
        break;
    case HOST_CMD_CONFIRM_SCHEME: host_action_confirm(); break;
    case HOST_CMD_PLAY_TEXT:      host_action_play_text(frame->data, frame->len); break;
    case HOST_CMD_PLAN_BEGIN:     host_action_plan_begin(frame->data, frame->len); break;
    case HOST_CMD_PLAN_CHUNK:     host_action_plan_chunk(frame->data, frame->len); break;
    case HOST_CMD_PLAN_COMMIT:    host_action_plan_commit(); break;
    default:
        Serial.printf("[HOST] unknown type=0x%02X\n", frame->type);
        break;
    }
    host_ack(frame);   // 统一回执（与子板 ACK 同语义）
}
