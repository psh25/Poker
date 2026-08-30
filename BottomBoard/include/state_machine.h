#pragma once

#include <stdint.h>

/**
 * 全局状态机（架构 v2 第五章，2026-08 修订：删除 SELECTING，选方案并入 IDLE）：
 * IDLE → DEALING → GAME_ACTIVE → GAME_END → IDLE
 * 状态切换统一由状态管理任务执行（单写者），其他任务只置事件组位。
 */

typedef enum {
    STATE_IDLE = 0,
    STATE_DEALING,
    STATE_GAME_ACTIVE,
    STATE_GAME_END
} system_state_t;

system_state_t state_get_current(void);
void state_transition_to(system_state_t next);
