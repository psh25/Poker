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
#include "tasks.h"

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

    // 状态变化 → 屏幕（架构 v2 附录 A 生产者映射）
    display_cmd_t cmd = {};
    cmd.type = (display_cmd_type_t)0xFF;   // 默认不发送
    switch (next) {
    case STATE_IDLE:
        display_set_confirmed(false);   // 进入 IDLE 一律视为“未选择”
        cmd.type = DISPLAY_CMD_IDLE;
        cmd.payload.menu.selectedIndex = display_get_selected();   // 保留当前选中项
        cmd.payload.menu.confirmed = 0;
        break;
    case STATE_DEALING:
        // 发牌界面由发牌任务负责刷新，避免“开始”屏覆盖发牌错误信息
        break;
    case STATE_GAME_ACTIVE: cmd.type = DISPLAY_CMD_GAME_ACTIVE; break;
    default: break;
    }
    if (cmd.type != (display_cmd_type_t)0xFF) send_display_command(&cmd);

    // 状态变化 → 主机（BLE 0x91；未连接时仅打印逻辑跳过）
    host_notify_state((uint8_t)next);
}

void vStateManagerTask(void *pv) {
    // 低(1) | 任意核心 | 事件组触发（永久阻塞）
    //
    // 注意：一次取出并清掉所有已置位的请求后，要在**同一轮**里把它们按优先级
    // 全部处理完。原来的写法是命中第一个位就 continue，会把同一批里的其它请求
    // 一起丢掉（例如"确认发牌"与"发牌完成"几乎同时置位时）。
    const EventBits_t kMask = BIT_DEAL_COMPLETE | BIT_DEAL_ERROR |
                              BIT_RESET | BIT_DEAL_CONFIRM |
                              BIT_TEST_IDLE | BIT_TEST_DEALING | BIT_TEST_ACTIVE;
    system_state_t cur = STATE_IDLE;
    for (;;) {
        EventBits_t bits = xEventGroupWaitBits(xStateEventGroup, kMask,
                                               pdTRUE,     // 取出并清位
                                               pdFALSE,    // 任一满足即可
                                               portMAX_DELAY);

        // 1) CLI 调试：强制切换到指定状态（优先级最高，只处理一个）
        if (bits & BIT_TEST_IDLE)    { state_transition_to(STATE_IDLE);        cur = STATE_IDLE;        continue; }
        if (bits & BIT_TEST_DEALING) { state_transition_to(STATE_DEALING);     cur = STATE_DEALING;     continue; }
        if (bits & BIT_TEST_ACTIVE)  { state_transition_to(STATE_GAME_ACTIVE); cur = STATE_GAME_ACTIVE; continue; }

        // 2) 复位优先级高于常规转移：STOP / RESET / GAME_ACTIVE 长按结束
        if (bits & BIT_RESET) {
            if (cur != STATE_IDLE) state_transition_to(STATE_IDLE);
            cur = STATE_IDLE;
            continue;
        }

        // 3) 常规转移：同一批里的条件按顺序依次生效，不丢事件
        if (cur == STATE_IDLE && (bits & BIT_DEAL_CONFIRM)) {
            state_transition_to(STATE_DEALING);
            cur = STATE_DEALING;
        }
        if (cur == STATE_DEALING && (bits & BIT_DEAL_COMPLETE)) {
            state_transition_to(STATE_GAME_ACTIVE);
            cur = STATE_GAME_ACTIVE;
        }
        // BIT_DEAL_ERROR：停在 DEALING，屏幕已显示错误，等编码器按下重置
    }
}
