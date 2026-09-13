#pragma once

#include <stdint.h>
#include <stdbool.h>

/**
 * 子板裸机状态机（docs/subboard_architecture.md 第三章）
 *
 * 单张牌的动作序列（两种模式都一样，**固定时长**，不再由光电门决定何时换相）：
 *   IDLE → MOTOR_ON（正转 MOTOR_FWD_MS）→ BRAKE → REVERSE → PAUSE → 判定 → SEND_BACK
 *
 * 光电门只当"记录器"（见 state_machine.cpp）：
 *   动作期间（正转/刹车/反转）只采样记录"是否出现过有牌"，不做任何判断；
 *   动作结束、电机停稳后（PAUSE 末尾）再看"记录 + 当前电平"判定这一张：
 *     成功发出   = 过程中出现过“有牌”，结束时已恢复“无牌”
 *     卡在出牌口 = 结束时仍是“有牌”
 *     根本没出去 = 整个过程都没出现过“有牌”（漏发 / 卡在牌源里）
 *   失败 → RETRACT（反转撤回）→ RETRACT_WAIT（等门恢复）
 *        → 恢复成功则重试这一张；撤回失败/重试超限则 ERROR（等底板复位）
 *
 * USE_PHOTO_SENSOR=0（没有光电门时的调试路径）：跳过判定，一律按"成功发出"处理。
 * 任意阶段异常 → ERROR（立即上报，等待底板 CMD_RESET / CMD_STOP 恢复）
 */

typedef enum {
    SUB_STATE_IDLE = 0,
    SUB_STATE_MOTOR_ON,         // 正转出牌（期间只记录光电门）
    SUB_STATE_BRAKE,            // 正转→反转之间的短刹车（防换向电流冲击）
    SUB_STATE_REVERSE,          // 出牌后反转回退（摄像头拍牌底）
    SUB_STATE_PAUSE,            // 动作收尾停顿 + 判定前的稳定窗口（判定在这里做）
    SUB_STATE_RETRACT,          // 判失败（卡住/没出去）：反转撤回
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
