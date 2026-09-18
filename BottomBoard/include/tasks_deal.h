#pragma once

#include <stdint.h>
#include <stdbool.h>

#include <stddef.h>
#include "deal_config.h"
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

// ---- 外部事件触发的本局异常收口（P3 子板掉线 / P4 子板中途复位）----
// 由其他任务调用（监控任务判定掉线 / 子板通信任务收到子板重启的 EVT_READY）。
// **仅当处于 DEALING 时生效**；动作 = 记错误 + 屏幕显示 + 尽力给子板发 CMD_STOP +
// 中止底盘转动与各项等待 + 置 BIT_DEAL_ERROR，之后停在 DEALING 等编码器按下重置
//（与其它发牌错误同一套出口）。
// ⚠️ 异常路径：调用方与发牌任务可能并发；本函数只写错误标志/错误串并发布快照，不再继续发牌。
void deal_abort_remote(const char *errMsg);

// ---- 无子板模式（子板仿真）----
// 打开后底板不再依赖子板：跳过"子板不在线"检查、不下发动作类命令，
// 每张牌的牌面/出牌成功/单张完成都由底板自己按 SIM_* 延时补发模拟事件。
// 默认值由 app_config.h 的 USE_SUBBOARD 决定；运行时用底板 CLI `subsim on|off` 切换。
// ---- 小程序上传的发牌计划（逐张目标牌堆序列）----
// 提交后按 scheme 关联；发牌任务会优先使用与当前方案匹配的上传计划。
bool deal_host_plan_begin(uint8_t scheme, uint16_t total, uint8_t pileCount);
bool deal_host_plan_chunk(uint8_t offset, const uint8_t *decks, uint8_t count);
bool deal_host_plan_commit(void);
void deal_host_plan_clear(void);
bool deal_host_plan_copy(deal_plan_t *out, uint8_t scheme);
void deal_sim_set_auto(bool on);   // 设置无子板模式（函数名沿用早期版本）
bool deal_sim_get_auto(void);      // 当前是否处于无子板模式
