#pragma once

#include <stdint.h>

/**
 * 主机命令动作（BLE 小程序 / 串口 CLI / 板间协议的共同入口）；实现在 host_actions.cpp。
 * 这些动作处理"底板级"逻辑，需要子板的再转发板间帧。
 */
void host_action_select(uint8_t idx);     // 选择方案（仅 IDLE）
void host_action_confirm(void);           // 两段式第一步：确认方案
void host_action_deal_start(void);        // 启动发牌（要求已确认）
void host_action_stop(void);              // 停机 + 中止发牌 + 回 IDLE
void host_action_reset(void);             // 复位子板 + 中止发牌 + 回 IDLE
void host_action_status(void);            // 立即上报状态

// 底板 → 小程序事件（供发牌任务实时上报）
void host_notify_card(uint16_t idx, uint8_t pile, uint8_t cardCode, uint8_t src);
void host_notify_deal_done(uint16_t total);
