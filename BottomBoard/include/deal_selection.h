#pragma once

#include <stdint.h>
#include <stdbool.h>

/**
 * 发牌选择状态（菜单选中项 / 两段式确认 / 发牌方式）
 *
 * 这三个值**决定发牌行为**，属于业务状态而不是绘制数据，因此从显示层搬到这里：
 *   - 显示层只负责画（把快照通过显示命令传进去，不再自己保存）；
 *   - 编码器任务、CLI、BLE 主机命令、状态机通过 set 接口修改；
 *   - 所有读写都在互斥量保护下，避免多任务读到"半新半旧"的组合。
 */
typedef struct {
    uint8_t scheme;       // 当前选中的方案（0-based）
    bool    confirmed;    // 是否已完成两段式第一步（顶部显示"已选"）
    bool    orderRandom;  // 发牌方式：false=顺序，true=随机
} deal_selection_t;

void deal_selection_snapshot(deal_selection_t *out);   // 一次性取三个值（一致快照）

uint8_t deal_selection_get_scheme(void);
bool    deal_selection_get_confirmed(void);
bool    deal_selection_get_order_random(void);

void deal_selection_set_scheme(uint8_t scheme);        // 改选会同时清掉 confirmed
void deal_selection_set_confirmed(bool on);
void deal_selection_set_order_random(bool rnd);
