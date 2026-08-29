/**
 * 子板裸机状态机（docs/subboard_architecture.md 第三章）
 * 流水线：启动电机 → 等光敏 → 拍照识别 → 回传 → 待命
 * 关键时序：光敏 500ms 超时、摄像头 2s 超时
 */
#include <Arduino.h>
#include <string.h>

#include "app_config.h"
#include "protocol.h"
#include "state_machine.h"
#include "hardware.h"

static sub_state_t g_state = SUB_STATE_IDLE;
static uint32_t g_state_start_ms = 0;
static uint8_t g_error = 0;

// 单卡数据缓冲（架构第六章：只保留当前一张牌，复用）
static uint8_t s_card_data[CARD_DATA_MAX];
static uint8_t s_card_len = 0;

sub_state_t sub_state_get(void) {
    return g_state;
}

// 底板命令分发
void sub_state_handle_command(uint8_t cmd, const uint8_t *data, uint8_t len) {
    (void)data; (void)len;
    switch (cmd) {
    case CMD_DEAL_START:
        if (g_state == SUB_STATE_IDLE) {
            busy_motor_start();
            g_state = SUB_STATE_MOTOR_ON;
            g_state_start_ms = millis();
        }
        break;
    case CMD_STOP:
    case CMD_RESET:
        busy_motor_stop();
        s_card_len = 0;
        g_error = 0;
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
    g_state = SUB_STATE_ERROR;
}

void sub_state_run(void) {
    uint32_t now = millis();

    switch (g_state) {
    case SUB_STATE_IDLE:
        // 等待底板命令（CMD_DEAL_START 等）
        break;

    case SUB_STATE_MOTOR_ON:
        // 电机启动完成（占位判定）→ 进入等待光敏，并开始 500ms 超时计时
        if (now - g_state_start_ms >= MOTOR_STARTUP_MS) {
            g_state = SUB_STATE_WAIT_CARD;
            g_state_start_ms = now;
        }
        // TODO: 电流检测 → 堵转立即 sub_mark_error(EVT_ERROR_MOTOR_STALL)
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
        proto_send(EVT_CARD_VALUE, s_card_data, s_card_len);
        s_card_len = 0;
        proto_send(EVT_DEAL_DONE, NULL, 0);
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
