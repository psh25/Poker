/**
 * 全局状态机（架构 v2 第五章）
 * 单写者：只有状态管理任务执行状态切换；其他任务通过事件组位提出请求。
 */
#include <Arduino.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>

#include "itc.h"
#include "state_machine.h"
#include "display.h"
#include "hardware.h"

static system_state_t g_current = STATE_IDLE;

system_state_t state_get_current(void) {
    system_state_t s;
    xSemaphoreTake(xStateMutex, portMAX_DELAY);
    s = g_current;
    xSemaphoreGive(xStateMutex);
    return s;
}

void state_transition_to(system_state_t next) {
    xSemaphoreTake(xStateMutex, portMAX_DELAY);
    if (next == g_current) {
        xSemaphoreGive(xStateMutex);
        return;
    }
    busy_state_exit((uint8_t)g_current);
    g_current = next;
    busy_state_enter((uint8_t)next);
    xSemaphoreGive(xStateMutex);

    xEventGroupSetBits(xStateEventGroup, BIT_STATE_CHANGED);

    // 状态变化 → 屏幕（架构 v2 附录 A 生产者映射）
    display_cmd_t cmd = {};
    switch (next) {
    case STATE_IDLE:
        display_set_confirmed(false);   // 进入 IDLE 一律视为“未选择”
        cmd.type = DISPLAY_CMD_IDLE;
        cmd.payload.menu.selectedIndex = display_get_selected();   // 保留当前选中项
        cmd.payload.menu.confirmed = 0;
        break;
    case STATE_DEALING:   cmd.type = DISPLAY_CMD_DEALING;
                          cmd.payload.dealing.progress = 0;
                          strncpy(cmd.payload.dealing.status, "发牌中", sizeof(cmd.payload.dealing.status));
                          break;
    case STATE_GAME_ACTIVE: cmd.type = DISPLAY_CMD_GAME_ACTIVE; break;
    case STATE_GAME_END:  cmd.type = DISPLAY_CMD_GAME_END;    break;
    default: break;
    }
    send_display_command(&cmd);
}

void vStateManagerTask(void *pv) {
    // 低(1) | 任意核心 | 事件组触发（永久阻塞）
    system_state_t cur = STATE_IDLE;
    for (;;) {
        EventBits_t bits = xEventGroupWaitBits(
            xStateEventGroup,
            BIT_DEAL_COMPLETE | BIT_DEAL_ERROR | BIT_CONFIRM_RECEIVED |
            BIT_GAME_END | BIT_RESET | BIT_DEAL_CONFIRM |
            BIT_TEST_IDLE | BIT_TEST_DEALING | BIT_TEST_ACTIVE | BIT_TEST_END,
            pdTRUE,        // 清除位
            pdFALSE,       // 任一满足即可
            portMAX_DELAY);
        (void)bits;

        // CLI 调试：强制切换到指定状态（测试各状态显示/功能）
        if (bits & BIT_TEST_IDLE)        { state_transition_to(STATE_IDLE);        cur = STATE_IDLE;        continue; }
        if (bits & BIT_TEST_DEALING)     { state_transition_to(STATE_DEALING);     cur = STATE_DEALING;     continue; }
        if (bits & BIT_TEST_ACTIVE)      { state_transition_to(STATE_GAME_ACTIVE); cur = STATE_GAME_ACTIVE; continue; }
        if (bits & BIT_TEST_END)         { state_transition_to(STATE_GAME_END);    cur = STATE_GAME_END;    continue; }

        // 转移逻辑对应架构 v2 5.3 转移条件表
        switch (cur) {
        case STATE_IDLE:
            if (bits & BIT_DEAL_CONFIRM) {
                state_transition_to(STATE_DEALING); cur = STATE_DEALING;
                // 若发牌任务已极快完成/出错，避免丢失完成位
                if (bits & BIT_DEAL_ERROR)       { state_transition_to(STATE_IDLE); cur = STATE_IDLE; }
                else if (bits & BIT_DEAL_COMPLETE) { state_transition_to(STATE_GAME_ACTIVE); cur = STATE_GAME_ACTIVE; }
            }
            break;
        case STATE_DEALING:
            if (bits & BIT_DEAL_ERROR)       { state_transition_to(STATE_IDLE); cur = STATE_IDLE; }
            else if (bits & BIT_DEAL_COMPLETE) { state_transition_to(STATE_GAME_ACTIVE); cur = STATE_GAME_ACTIVE; }
            break;
        case STATE_GAME_ACTIVE:
            if (bits & BIT_GAME_END)         { state_transition_to(STATE_GAME_END); cur = STATE_GAME_END; }
            break;
        case STATE_GAME_END:
            if (bits & BIT_RESET)            { state_transition_to(STATE_IDLE); cur = STATE_IDLE; }
            break;
        default:
            break;
        }
    }
}
