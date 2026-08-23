/**
 * 屏幕显示（架构 v2 附录 A）：事件驱动、非阻塞发送、只展示不决策。
 */
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "itc.h"
#include "display.h"
#include "hardware.h"

bool send_display_command(const display_cmd_t *cmd) {
    // 非阻塞发送；队列满丢弃（UI 只关心最终状态，快速旋转时中间态可丢）
    return xQueueSend(xDisplayQueue, cmd, 0) == pdPASS;
}
