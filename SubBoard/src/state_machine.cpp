/**
 * 子板裸机状态机（docs/subboard_architecture.md 第三章）
 * 流水线（定时模式）：启动电机正转 → 反转回退 → 停顿 → 回传完成 → 自动下一张 / 待命
 * 流水线（光敏模式）：启动电机 → 等光敏 → 拍照识别 → 回传 → 待命
 * 关键时序：光敏 500ms 超时、摄像头 2s 超时
 */
#include <Arduino.h>
#include <string.h>

#include "app_config.h"
#include "protocol.h"
#include "state_machine.h"
#include "hardware.h"
#include "debug.h"

static sub_state_t g_state = SUB_STATE_IDLE;
static uint32_t g_state_start_ms = 0;
static uint8_t g_error = 0;
static uint32_t s_deal_count = 0;   // 出牌计数（调试日志用）

// 单卡数据缓冲（架构第六章：只保留当前一张牌，复用）
static uint8_t s_card_data[CARD_DATA_MAX];
static uint8_t s_card_len = 0;

// ---- 自动连续发牌（调试用）----
static bool  s_auto_active  = false;
static long  s_auto_left    = 0;     // >0 剩余张数；-1 无限
static bool  s_auto_pending = false; // 上电延时等待中
static uint32_t s_auto_start_ms = 0;

sub_state_t sub_state_get(void) {
    return g_state;
}

// 启动一张牌：从 IDLE（手动/自动首次）或 SEND_BACK（自动下一张）进入 MOTOR_ON
static void deal_start_now(void) {
    if (g_state != SUB_STATE_IDLE && g_state != SUB_STATE_SEND_BACK) return;
    busy_motor_start();               // 正转出牌
    g_state = SUB_STATE_MOTOR_ON;
    g_state_start_ms = millis();
}

// ---- 自动连续发牌 API ----
void sub_auto_deal_arm(long count) {
    s_auto_pending = true;
    s_auto_active  = false;
    s_auto_left    = (count <= 0) ? -1 : count;
    s_auto_start_ms = millis() + AUTO_DEAL_START_DELAY_MS;
    dbg_printf("[SUB] auto deal armed: starts in %u ms (type 'auto off' to cancel, left=%ld)\n",
                  (unsigned)AUTO_DEAL_START_DELAY_MS, s_auto_left);
}

void sub_auto_deal_set(long count) {
    s_auto_pending = false;
    s_auto_active  = true;
    s_auto_left    = (count <= 0) ? -1 : count;
    if (g_state == SUB_STATE_IDLE) {
        dbg_printf("[SUB] auto deal started, left=%ld\n", s_auto_left);
        deal_start_now();
    } else {
        dbg_printf("[SUB] auto deal queued, left=%ld (current card finishes first)\n", s_auto_left);
    }
}

void sub_auto_deal_stop(void) {
    s_auto_pending = false;
    s_auto_active  = false;
    s_auto_left    = 0;
    dbg_println("[SUB] auto deal stopped");
}

// 底板命令分发
void sub_state_handle_command(uint8_t cmd, const uint8_t *data, uint8_t len) {
    (void)data; (void)len;
    switch (cmd) {
    case CMD_DEAL_START:
        deal_start_now();             // 手动单张（自动模式运行中会被状态保护忽略）
        break;
    case CMD_STOP:
    case CMD_RESET:
        busy_motor_stop();
        s_card_len = 0;
        g_error = 0;
        sub_auto_deal_stop();         // 停止/复位同时取消自动发牌
        g_state = SUB_STATE_IDLE;
        break;
    case CMD_SELF_TEST:
        busy_self_test();
        break;
    case CMD_STATUS_QUERY:
        busy_status_query();   // TODO: 回复状态事件（响应帧待协议定稿）
        break;
    default:
        break;
    }
}

// 物理层异常：立即上报，进入 ERROR 等待底板恢复（架构第七章）
void sub_mark_error(uint8_t errorType) {
    g_error = errorType;
    busy_motor_stop();
    busy_error_handle(errorType);
    proto_send(errorType, NULL, 0);
    sub_auto_deal_stop();             // 异常时取消自动发牌，等底板处理
    g_state = SUB_STATE_ERROR;
}

void sub_state_run(void) {
    uint32_t now = millis();

    switch (g_state) {
    case SUB_STATE_IDLE:
        // 上电自动发牌：延时到点后开始
        if (s_auto_pending && (int32_t)(now - s_auto_start_ms) >= 0) {
            s_auto_pending = false;
            s_auto_active  = true;
            dbg_printf("[SUB] auto deal started, left=%ld\n", s_auto_left);
            deal_start_now();
        }
        break;

    case SUB_STATE_MOTOR_ON:
        // 主动维持正转驱动（GPIO 状态异常自愈）
        busy_motor_start();

        if (!USE_PHOTO_SENSOR) {
            // 定时出牌：正转 MOTOR_FWD_MS 后认为一张已出
            if (now - g_state_start_ms >= MOTOR_FWD_MS) {
                busy_motor_stop();
                proto_send(EVT_CARD_OUT, NULL, 0);   // 实时上报：已出一张
                if (MOTOR_REV_MS > 0) {
                    busy_motor_start_reverse();       // 出牌后反转回退（摄像头拍牌底）
                    g_state = SUB_STATE_REVERSE;
                } else {
                    g_state = SUB_STATE_PAUSE;
                }
                g_state_start_ms = now;
            }
        } else {
            // 光敏模式：电机启动完成 → 等待光敏检测到牌
            if (now - g_state_start_ms >= MOTOR_STARTUP_MS) {
                g_state = SUB_STATE_WAIT_CARD;
                g_state_start_ms = now;
            }
            // TODO: 电流检测 → 堵转立即 sub_mark_error(EVT_ERROR_MOTOR_STALL)
        }
        break;

    case SUB_STATE_REVERSE:
        // 主动维持反转驱动（GPIO 状态异常自愈）
        busy_motor_start_reverse();
        if (now - g_state_start_ms >= MOTOR_REV_MS) {
            busy_motor_stop();
            g_state = SUB_STATE_PAUSE;
            g_state_start_ms = now;
        }
        break;

    case SUB_STATE_PAUSE:
        // 每张牌之间的停顿：让牌完全出去、牌堆复位
        busy_motor_stop();
        if (now - g_state_start_ms >= MOTOR_PAUSE_MS) {
            g_state = SUB_STATE_SEND_BACK;
        }
        break;

    case SUB_STATE_WAIT_CARD:
        if (sub_photo_take()) {
            busy_motor_stop();
            proto_send(EVT_CARD_OUT, NULL, 0);     // 光敏触发 → 立即上报（实时传输原则）
            s_card_len = 0;
            busy_camera_capture(s_card_data, &s_card_len);
            g_state = SUB_STATE_CAM_CAPTURE;
            g_state_start_ms = now;                // 开始 2s 识别超时
        } else if (now - g_state_start_ms >= PHOTO_TIMEOUT_MS) {
            sub_mark_error(EVT_ERROR_CARD_JAM);    // 500ms 未检测到牌 → 卡牌/漏发
        }
        break;

    case SUB_STATE_CAM_CAPTURE:
        if (s_card_len > 0) {
            // 识别完成 → 回传
            g_state = SUB_STATE_SEND_BACK;
        } else if (now - g_state_start_ms >= CAMERA_TIMEOUT_MS) {
            // 2s 超时 → 该张按未知牌处理并上报（架构 8.1）
            proto_send(EVT_ERROR_CAM_FAIL, NULL, 0);
            g_state = SUB_STATE_SEND_BACK;
        }
        break;

    case SUB_STATE_SEND_BACK:
        // 单张结果实时回传：牌面值 + 流程完成（不缓存整副牌）
        // 定时模式无摄像头：card 为空 → 底板按“未知牌”继续下一张
        proto_send(EVT_CARD_VALUE, s_card_data, s_card_len);
        s_card_len = 0;
        proto_send(EVT_DEAL_DONE, NULL, 0);
        s_deal_count++;
        dbg_printf("[SUB] card #%lu out (fwd=%dms rev=%dms)\n",
                   (unsigned long)s_deal_count, MOTOR_FWD_MS, MOTOR_REV_MS);

        if (s_auto_active) {
            // 自动发牌：直接开始下一张
            if (s_auto_left > 0) {
                s_auto_left--;
                if (s_auto_left == 0) {
                    s_auto_active = false;
                    dbg_println("[SUB] auto deal finished");
                    g_state = SUB_STATE_IDLE;
                    break;
                }
            }
            deal_start_now();                       // s_auto_left = -1(无限) 或 >0
            if (s_auto_left > 0) dbg_printf("[SUB] auto next card, left=%ld\n", s_auto_left);
            break;                                  // 已进入 MOTOR_ON
        }
        g_state = SUB_STATE_IDLE;
        break;

    case SUB_STATE_ERROR:
        // 等待底板 CMD_RESET / CMD_STOP 恢复（在 handle_command 中处理）
        break;

    default:
        g_state = SUB_STATE_IDLE;
        break;
    }
}



