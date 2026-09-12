#pragma once

#include <stdint.h>
#include <stdbool.h>

/**
 * 子板裸机状态机（docs/subboard_architecture.md 第三章）
 * 定时出牌（USE_PHOTO_SENSOR=0）：IDLE → MOTOR_ON → BRAKE → REVERSE → PAUSE → SEND_BACK
 * 光电门出牌（USE_PHOTO_SENSOR=1）：
 *   IDLE → MOTOR_ON → WAIT_CARD（等有牌）→ WAIT_GONE（等无牌，确认牌完整通过）
 *        → BRAKE → REVERSE → PAUSE → SEND_BACK
 *   卡牌保护：WAIT_* → RETRACT（反转撤回）→ RETRACT_WAIT（等门清空）
 *            → 恢复成功则重试这一张；撤回失败/重试超限则 ERROR（等底板复位）
 * 任意阶段异常 → ERROR（立即上报，等待底板 CMD_RESET / CMD_STOP 恢复）
 */

typedef enum {
    SUB_STATE_IDLE = 0,
    SUB_STATE_MOTOR_ON,
    SUB_STATE_BRAKE,            // 正转→反转之间的短刹车（防换向电流冲击）
    SUB_STATE_REVERSE,          // 出牌后反转回退（摄像头拍牌底）
    SUB_STATE_PAUSE,            // 定时模式：每张牌之间的停顿
    SUB_STATE_WAIT_CARD,        // 光电门模式：等“有牌”
    SUB_STATE_WAIT_GONE,        // 光电门模式：等“无牌”并确认牌完整通过
    SUB_STATE_RETRACT,          // 卡牌：反转撤回
    SUB_STATE_RETRACT_WAIT,     // 撤回后等门恢复“无牌”
    SUB_STATE_SEND_BACK,
    SUB_STATE_ERROR
} sub_state_t;

sub_state_t sub_state_get(void);

void sub_state_run(void);                       // 主循环调用：执行状态机
void sub_state_handle_command(uint8_t cmd, const uint8_t *data, uint8_t len);
void sub_mark_error(uint8_t errorType);         // 物理层异常入口（立即上报）
void sub_status_report(void);                   // 状态回执（心跳应答 EVT_STATUS）

// ---- 自动连续发牌（调试用：不需要底板，自己一张接一张发）----
void sub_auto_deal_arm(long count);   // 上电延时后自动开始（AUTO_DEAL_START_DELAY_MS）
void sub_auto_deal_set(long count);   // 立即开始：>0 = 发 N 张后停；<=0 = 无限
void sub_auto_deal_stop(void);        // 停止自动发牌（当前这张发完为止）
