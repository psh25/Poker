#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "display.h"    // DEAL_ERROR_MAX

/**
 * 发牌控制任务的对外接口（任务本体在 tasks_deal.cpp）
 */

// 发牌运行状态快照（发牌任务写、CLI 读；避免跨文件直接访问内部变量）
typedef struct {
    uint8_t  scheme;        // 0-based 当前方案
    uint8_t  deck;          // 0-based 当前目标牌堆
    uint8_t  progress;      // 0~100
    uint16_t dealt;         // 已成功发出张数
    uint16_t total;         // 本局计划总张数
    bool     errorActive;   // 是否处于发牌错误（等编码器重置）
    uint8_t  errorCount;
    char     errors[DEAL_ERROR_MAX][24];
} deal_status_t;

void deal_get_status(deal_status_t *out);

// 是否处于发牌错误（编码器任务用来判断"出错时短按重置"）
bool deal_error_active(void);

// ---- 无子板模式（子板仿真）----
// 打开后底板不再依赖子板：跳过"子板不在线"检查、不下发动作类命令，
// 每张牌的牌面/出牌成功/单张完成都由底板自己按 SIM_* 延时补发模拟事件。
// 默认值由 app_config.h 的 USE_SUBBOARD 决定；运行时用底板 CLI `subsim on|off` 切换。
void deal_sim_set_auto(bool on);   // 设置无子板模式（函数名沿用早期版本）
bool deal_sim_get_auto(void);      // 当前是否处于无子板模式
