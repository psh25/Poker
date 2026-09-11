#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/**
 * 参数驱动发牌计划
 *
 * 计划 = 有序的“发牌组”列表，每组 = { 牌堆编号, 张数, 标签 }；
 * 发牌任务统一执行“转到该牌堆 → 连发 count 张 → 下一组”，不区分顺序/随机。
 *
 * 两种生成方式（互斥，全机一个当前值；IDLE 下长按编码器切换）：
 *
 *   顺序发牌（Sequential）
 *     按参数把牌依次分给各牌堆：P1 全部 → P2 全部 → … → 公共 → 底牌。
 *     只发“需要的牌”，少发的部分留在牌源（弃牌张数只统计、不发出）。
 *
 *   随机发牌（Random）
 *     先按“每个牌堆该拿几张”生成一张分配表，再用硬件随机数洗牌，
 *     最后把连续相同的项合并成发牌组。转盘会按洗牌结果**无序**地来回转。
 *     整副牌（totalCards 张）都会被发出，多出来的那张数进“弃牌堆”。
 *
 * 方案（斗地主/掼蛋/…/Custom）只描述参数，计划在开局时按当前方式现场生成。
 */

#define DECK_COUNT_MAX        8    // 实体牌堆位数量上限
#define DEAL_GROUP_MAX        160  // 发牌组数量上限（随机模式最坏情况 = 每张一组）
#define DEAL_TOTAL_CARDS_MAX  160  // 单局总牌数上限（留多副牌余量）
#define DEAL_NAME_LEN         16
#define DEAL_LABEL_LEN        10   // "P1" / "Public" / "Bottom" / "Discard"
#define DEAL_PRESET_COUNT     8    // 菜单里的方案数量（含 Custom；发牌方式不占菜单位）
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

// 发牌方式：顺序 / 随机（互斥，与方案无关）
typedef enum {
    DEAL_ORDER_SEQUENTIAL = 0,
    DEAL_ORDER_RANDOM
} deal_order_t;

typedef struct {
    uint8_t deck;                    // 0-based 实体牌堆编号
    uint8_t count;                   // 本组发牌张数
    char    label[DEAL_LABEL_LEN];   // 显示名：P1 / Public / Bottom / Discard
} deal_group_t;

typedef struct {
    char        name[DEAL_NAME_LEN];
    deal_mode_t mode;
    uint8_t     groupCount;
    deal_group_t groups[DEAL_GROUP_MAX];
    uint16_t    totalCards;          // 本计划实际发出的张数
    uint16_t    rotateTurns;         // DEAL_MODE_ROTATE_TEST 使用
} deal_plan_t;

// 牌局参数（一个方案的全部描述）
typedef struct {
    uint8_t  players;       // 人数（每人一个牌堆位）
    uint8_t  handCards;     // 每人张数
    uint8_t  publicCards;   // 公共牌张数（占 1 个牌堆位，0 = 不用）
    uint8_t  bottomCards;   // 底牌张数（占 1 个牌堆位，0 = 不用）
    uint16_t totalCards;    // 本局总牌数（含多余的弃牌）
} deal_params_t;

void deal_config_init(void);
uint8_t deal_scheme_count(void);
const char *deal_scheme_name(uint8_t index);
const deal_params_t *deal_scheme_params(uint8_t index);
deal_params_t *deal_scheme_params_custom(void);   // Custom 槽位参数（可直接改）

uint16_t deal_params_required(const deal_params_t *p);  // 需要的牌数 = 人数×每人 + 公共 + 底牌
int16_t  deal_params_discard(const deal_params_t *p);   // 弃牌张数（<0 表示参数非法）
uint8_t  deal_params_piles(const deal_params_t *p);     // 需要占用的牌堆位数

// 生成计划（失败返回 false 并写 err）
bool deal_plan_build_sequential(deal_plan_t *out, const char *name,
                                const deal_params_t *p, char *err, size_t errLen);
bool deal_plan_build_random(deal_plan_t *out, const char *name,
                            const deal_params_t *p, char *err, size_t errLen);
bool deal_plan_build_rotate_test(deal_plan_t *out);

// 按“方案 + 发牌方式”生成计划（调用方提供缓冲，避免与发牌任务共用）
bool deal_build_plan(deal_plan_t *out, uint8_t schemeIndex, deal_order_t order,
                     char *err, size_t errLen);

void deal_plan_print(uint8_t index, const deal_plan_t *plan, const deal_params_t *p);
