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

// 模拟事件开关（只对 TEST 方案生效，默认关闭）
void deal_sim_set_auto(bool on);
bool deal_sim_get_auto(void);
