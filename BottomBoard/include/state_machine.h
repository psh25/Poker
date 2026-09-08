#pragma once

#include <stdint.h>

/**
 * 全局状态机（架构 v2 第五章，2026-08 修订：删除 SELECTING，选方案并入 IDLE）：
 * 2026-09 修订：删除 GAME_END（冗余），GAME_ACTIVE 长按编码器 / 主机 reset 直接回 IDLE。
 * IDLE → DEALING → GAME_ACTIVE →（长按编码器确认结束）→ IDLE
 * IDLE 交互：旋转切换/取消方案 → 短按确认方案（顶部显示） → 再短按 CONFIRM 发牌
 * 状态切换统一由状态管理任务执行（单写者），其他任务只置事件组位。
 */

typedef enum {
    STATE_IDLE = 0,
    STATE_DEALING,
    STATE_GAME_ACTIVE
} system_state_t;

system_state_t state_get_current(void);
void state_transition_to(system_state_t next);
