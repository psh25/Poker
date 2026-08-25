#pragma once

#include <stdint.h>
#include <stdbool.h>

/**
 * 子板裸机状态机（docs/subboard_architecture.md 第三章）
 * IDLE → MOTOR_ON → WAIT_CARD → CAM_CAPTURE → SEND_BACK → IDLE
 * 任意阶段异常 → ERROR（立即上报，等待底板 CMD_RESET / CMD_STOP 恢复）
 */

typedef enum {
    SUB_STATE_IDLE = 0,
    SUB_STATE_MOTOR_ON,
    SUB_STATE_WAIT_CARD,
    SUB_STATE_CAM_CAPTURE,
    SUB_STATE_SEND_BACK,
    SUB_STATE_ERROR
} sub_state_t;

sub_state_t sub_state_get(void);

void sub_state_run(void);                       // 主循环调用：执行状态机
void sub_state_handle_command(uint8_t cmd, const uint8_t *data, uint8_t len);
void sub_mark_error(uint8_t errorType);         // 物理层异常入口（立即上报）
