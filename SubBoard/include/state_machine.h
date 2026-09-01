#pragma once

#include <stdint.h>
#include <stdbool.h>

/**
 * 子板裸机状态机（docs/subboard_architecture.md 第三章）
 * 定时出牌（USE_PHOTO_SENSOR=0）：IDLE → MOTOR_ON → REVERSE → PAUSE → SEND_BACK → (自动下一张|IDLE)
 * 光敏出牌（USE_PHOTO_SENSOR=1）：IDLE → MOTOR_ON → WAIT_CARD → CAM_CAPTURE → SEND_BACK → IDLE
 * 任意阶段异常 → ERROR（立即上报，等待底板 CMD_RESET / CMD_STOP 恢复）
 */

typedef enum {
    SUB_STATE_IDLE = 0,
    SUB_STATE_MOTOR_ON,
    SUB_STATE_REVERSE,          // 出牌后反转回退（摄像头拍牌底）
    SUB_STATE_PAUSE,            // 定时模式：每张牌之间的停顿
    SUB_STATE_WAIT_CARD,
    SUB_STATE_CAM_CAPTURE,
    SUB_STATE_SEND_BACK,
    SUB_STATE_ERROR
} sub_state_t;

sub_state_t sub_state_get(void);

void sub_state_run(void);                       // 主循环调用：执行状态机
void sub_state_handle_command(uint8_t cmd, const uint8_t *data, uint8_t len);
void sub_mark_error(uint8_t errorType);         // 物理层异常入口（立即上报）

// ---- 自动连续发牌（调试用：不需要底板，自己一张接一张发）----
void sub_auto_deal_arm(long count);   // 上电延时后自动开始（AUTO_DEAL_START_DELAY_MS）
void sub_auto_deal_set(long count);   // 立即开始：>0 = 发 N 张后停；<=0 = 无限
void sub_auto_deal_stop(void);        // 停止自动发牌（当前这张发完为止）
