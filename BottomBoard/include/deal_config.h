#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/**
 * 参数驱动发牌计划（自定义牌局）
 *
 * 核心思想：发牌任务不再按“方案编号”写逻辑，只执行一份“发牌计划”：
 *   计划 = 有序的发牌组列表，每组 = { 源牌堆编号, 张数, 标签 }
 *   执行方式：转盘转到该牌堆 → 从该牌堆连续发 count 张 → 下一组
 *
 * 常见牌局（斗地主/掼蛋/升级/德州/桥牌等）只是不同的参数预设：
 *   参数 = { 人数, 每人张数, 公共牌张数, 底牌张数 }
 * 自定义牌局通过串口 `game custom ...` 输入同一组参数生成计划；
 * 以后新增玩法只需在预置表里加一条参数（或直接由小程序下发参数）。
 */

#define DECK_COUNT_MAX        8    // 实体牌堆位数量上限
#define DEAL_GROUP_MAX        8    // 单局发牌组数量上限（不超过牌堆数）
#define DEAL_TOTAL_CARDS_MAX  160  // 单局发牌总张数上限（留出多副牌余量）
#define DEAL_NAME_LEN         16
#define DEAL_LABEL_LEN        8    // "P1" / "Bottom" / "Public" 等
#define DEAL_PRESET_COUNT     8    // 菜单里的方案数量（含 Custom）
#define SCHEME_COUNT          DEAL_PRESET_COUNT

// 预置方案索引（沿用旧习惯：索引2=旋转测试、索引3=TEST发牌）
enum {
    DEAL_INDEX_DOUDIZHU   = 0,
    DEAL_INDEX_GUANDAN    = 1,
    DEAL_INDEX_ROTATE_TEST = 2,
    DEAL_INDEX_TEST       = 3,
    DEAL_INDEX_SHENGJI    = 4,
    DEAL_INDEX_TEXAS6     = 5,
    DEAL_INDEX_BRIDGE     = 6,
    DEAL_INDEX_CUSTOM     = 7,
};

#define SCHEME_ROTATE_TEST_INDEX  DEAL_INDEX_ROTATE_TEST
#define SCHEME_TEST_INDEX         DEAL_INDEX_TEST
#define DEAL_ROTATE_TEST_TURNS    10   // 旋转测试方案：顶层连续转多少圈

typedef enum {
    DEAL_MODE_STACKS = 0,      // 按发牌组逐堆发牌
    DEAL_MODE_ROTATE_TEST      // 只旋转、不发牌（调试）
} deal_mode_t;

typedef struct {
    uint8_t deck;                    // 0-based 实体牌堆编号
    uint8_t count;                   // 本组发牌张数
    char    label[DEAL_LABEL_LEN];   // 显示名：P1 / Bottom / Public ...
} deal_group_t;

typedef struct {
    char        name[DEAL_NAME_LEN];
    deal_mode_t mode;
    uint8_t     groupCount;
    deal_group_t groups[DEAL_GROUP_MAX];
    uint16_t    totalCards;          // 各组张数之和
    uint16_t    rotateTurns;         // DEAL_MODE_ROTATE_TEST 使用
} deal_plan_t;

typedef struct {
    uint8_t players;       // 人数（每人一组牌堆）
    uint8_t handCards;     // 每人张数
    uint8_t publicCards;   // 公共牌张数（占用 1 组）
    uint8_t bottomCards;   // 底牌张数（占用 1 组）
} deal_params_t;

void deal_config_init(void);
uint8_t deal_scheme_count(void);
const char *deal_scheme_name(uint8_t index);
const deal_plan_t *deal_scheme_plan(uint8_t index);
deal_plan_t *deal_scheme_custom(void);          // 自定义槽位（DEAL_INDEX_CUSTOM）

// 由参数生成计划；失败时写 err 并返回 false
bool deal_plan_build(deal_plan_t *out, const char *name, const deal_params_t *p,
                     char *err, size_t errLen);

uint16_t deal_plan_total_cards(const deal_plan_t *plan);
void deal_plan_print(uint8_t index, const deal_plan_t *plan);
