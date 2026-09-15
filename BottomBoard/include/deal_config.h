#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/**
 * 参数驱动发牌计划
 *
 * 计划 = 有序的“发牌组”列表（每组 = { 牌堆编号, 张数, 标签 }）+ 可选的“特殊牌分流”规则；
 * 发牌任务统一执行“决定落点 → 转到位 → 发一张 → 等事件”，不区分顺序/随机。
 *
 * 三种生成方式：
 *
 *   顺序发牌（Sequential，`DEAL_ORDER_SEQUENTIAL`）
 *     按参数把牌依次分给各牌堆：P1 全部 → P2 全部 → … → 公共 → 底牌。
 *     只发“需要的牌”，少发的部分留在牌源（弃牌张数只统计、不发出）。
 *
 *   随机发牌（Random，`DEAL_ORDER_RANDOM`）
 *     先按“每个牌堆该拿几张”生成一张分配表，再用硬件随机数洗牌，
 *     最后把连续相同的项合并成发牌组。转盘会按洗牌结果**无序**地来回转。
 *     整副牌（`totalCards` 张）都会被发出，多出来的那张数进“弃牌堆”。
 *
 *   分拣（Sort，`DEAL_MODE_ROUTE`；预置 SortJoker / SortFace）
 *     这是**前置工作而不是牌局**：不看牌局参数，把牌源**全部发完**，按牌面把牌分到不同牌堆。
 *     例：分出大小王（王 → 王堆、其余 → 主堆）、分开正反面（牌背 → 背面堆、其余 → 正面堆）。
 *     张数**不需要预先知道**（`specialCount = 0`），靠“发完牌源”结束。
 *
 * 特殊牌分流（上面两种用法共用同一套机制）：
 *   - `special` / `specialCount` 声明“哪一类牌算特殊牌、源里有几张”；
 *   - 分配表按**普通牌数**（`totalCards − specialCount`）生成，特殊牌**不占名额**，
 *     所以各牌堆收到的张数仍然精确等于分配表；
 *   - 执行时先识别牌面：命中 → 送 `specialDeck`；未命中 → 按分配表/填充表走；
 *   - 落点由牌面决定，因此这类计划会置 `faceBeforeRotate`（先拿牌面、再转动）。
 *
 * 方案（斗地主/掼蛋/…/Custom）只描述参数，计划在开局时按当前方式现场生成。
 */

#define DECK_COUNT_MAX        8    // 实体牌堆位数量上限
#define DEAL_GROUP_MAX        160  // 发牌组数量上限（随机模式最坏情况 = 每张一组）
#define DEAL_TOTAL_CARDS_MAX  160  // 单局总牌数上限（留多副牌余量）
#define DEAL_NAME_LEN         16
#define DEAL_LABEL_LEN        10   // "P1" / "Public" / "Bottom" / "Discard"
#define DEAL_PRESET_COUNT     10   // 菜单里的方案数量（含 Custom 与两个分拣方案；发牌方式不占菜单位）
#define SCHEME_COUNT          DEAL_PRESET_COUNT

// 预置方案索引。排列约定：**真实牌局 → 分拣 → Custom → 测试方案**，
// 测试方案必须留在最后（见下面的 static_assert），以后增删方案都照这个规则排。
enum {
    DEAL_INDEX_DOUDIZHU   = 0,
    DEAL_INDEX_GUANDAN    = 1,
    DEAL_INDEX_SHENGJI    = 2,
    DEAL_INDEX_TEXAS6     = 3,
    DEAL_INDEX_BRIDGE     = 4,
    DEAL_INDEX_SORT_JOKER = 5,   // 分拣：分出大小王
    DEAL_INDEX_SORT_FACE  = 6,   // 分拣：分开正反面
    DEAL_INDEX_CUSTOM     = 7,
    DEAL_INDEX_TEST       = 8,   // 测试方案 —— 必须保持在最后
    DEAL_INDEX_ROTATE_TEST = 9,  // 测试方案 —— 必须保持在最后
};

// 保护约定：测试方案永远排在方案表最后
static_assert(DEAL_INDEX_TEST == DEAL_PRESET_COUNT - 2 &&
              DEAL_INDEX_ROTATE_TEST == DEAL_PRESET_COUNT - 1,
              "测试方案（Test / RotateTest）必须排在方案表最后");

#define SCHEME_ROTATE_TEST_INDEX  DEAL_INDEX_ROTATE_TEST
#define SCHEME_TEST_INDEX         DEAL_INDEX_TEST
#define DEAL_ROTATE_TEST_TURNS    10   // 旋转测试方案：顶层连续转多少圈
#define DEAL_SORT_SOURCE_CARDS    54   // 分拣方案的牌源张数（一副牌；两副请改这里或走 game custom）

typedef enum {
    DEAL_MODE_STACKS = 0,      // 按发牌组逐堆发牌
    DEAL_MODE_ROTATE_TEST,     // 只旋转、不发牌（调试）
    DEAL_MODE_ROUTE            // 分拣：发完牌源，按牌面分流（见顶部说明）
} deal_mode_t;

// 发牌方式：顺序 / 随机（互斥，与方案无关）
typedef enum {
    DEAL_ORDER_SEQUENTIAL = 0,
    DEAL_ORDER_RANDOM
} deal_order_t;

// 特殊牌类别：用于“把这（已知张数的）一类牌挑出来”
// （Tier A：牌局里剔除 → 送弃牌堆；Tier B：分拣 → 送专用堆）
typedef enum {
    SPECIAL_NONE = 0,      // 不挑牌（默认）
    SPECIAL_JOKER_ANY,     // 任意大小王
    SPECIAL_JOKER_SMALL,   // 小王
    SPECIAL_JOKER_BIG,     // 大王
    SPECIAL_JQK,           // J / Q / K（一副牌 12 张）
    SPECIAL_BACK,          // 牌背（正面朝下）
} deal_special_kind_t;

#define DEAL_FILL_REST  0xFF   // 发牌组 count 取该值 = “剩下的全部给这个牌堆”（分拣模式的兜底堆）

typedef struct {
    uint8_t deck;                    // 0-based 实体牌堆编号
    uint8_t count;                   // 本组发牌张数；DEAL_FILL_REST = 收剩下的全部
    char    label[DEAL_LABEL_LEN];   // 显示名：P1 / Public / Bottom / Discard
} deal_group_t;

typedef struct {
    char        name[DEAL_NAME_LEN];
    deal_mode_t mode;
    uint8_t     groupCount;
    deal_group_t groups[DEAL_GROUP_MAX];
    uint16_t    totalCards;          // 按发牌组要发出的张数（不含特殊牌）
    uint16_t    rotateTurns;         // DEAL_MODE_ROTATE_TEST 使用

    // ---- 特殊牌分流（两块默认 0 = 不分流，行为与旧版完全一致）----
    uint8_t     special;             // deal_special_kind_t
    uint8_t     specialCount;        // 源里这类牌的**已知张数**（0 = 未知，靠发完牌源结束）
    uint8_t     specialDeck;         // 命中的牌送到哪个牌堆
    char        specialLabel[DEAL_LABEL_LEN];
    uint16_t    sourceCards;         // 牌源里装的牌数（推牌次数上限）
    bool        faceBeforeRotate;    // true = 先拿牌面再转动（有分流规则时必须）
} deal_plan_t;

// 牌局参数（一个方案的全部描述）
typedef struct {
    uint8_t  players;       // 人数（每人一个牌堆位）
    uint8_t  handCards;     // 每人张数
    uint8_t  publicCards;   // 公共牌张数（占 1 个牌堆位，0 = 不用）
    uint8_t  bottomCards;   // 底牌张数（占 1 个牌堆位，0 = 不用）
    uint16_t totalCards;    // 牌源里装的牌数（含特殊牌与多余的弃牌）
    uint8_t  special;       // deal_special_kind_t：要挑出来的特殊牌类别（0 = 不挑）
    uint8_t  specialCount;  // 这类牌在源里的已知张数（special != NONE 时必须给对，见 params_check）
} deal_params_t;

void deal_config_init(void);
uint8_t deal_scheme_count(void);
const char *deal_scheme_name(uint8_t index);
const deal_params_t *deal_scheme_params(uint8_t index);
deal_params_t *deal_scheme_params_custom(void);   // Custom 槽位参数（可直接改）

uint16_t deal_params_required(const deal_params_t *p);  // 需要的牌数 = 人数×每人 + 公共 + 底牌
uint16_t deal_params_normal(const deal_params_t *p);    // 普通牌数 = 总牌数 − 特殊牌数（分配表按它生成）
int16_t  deal_params_discard(const deal_params_t *p);   // 弃牌张数 = 普通牌数 − 需要的牌（<0 表示参数非法）
uint8_t  deal_params_piles(const deal_params_t *p);     // 需要占用的牌堆位数

// ---- 特殊牌（分流规则）----
bool        deal_card_is_special(uint8_t card, uint8_t specialKind);  // 这张牌是否属于该类
const char *deal_special_name(uint8_t specialKind);                   // 显示名（none/joker/jqk/back…）
uint8_t     deal_special_from_name(const char *name);                 // 解析类别名，未知 → SPECIAL_NONE
uint8_t     deal_special_default_count(uint8_t specialKind);          // 一副牌下的常见张数（joker=2 / jqk=12）
bool        deal_scheme_is_sort(uint8_t index);                       // 该方案是否为“分拣”（不看牌局参数）

// 生成计划（失败返回 false 并写 err）
bool deal_plan_build_sequential(deal_plan_t *out, const char *name,
                                const deal_params_t *p, char *err, size_t errLen);
bool deal_plan_build_random(deal_plan_t *out, const char *name,
                            const deal_params_t *p, char *err, size_t errLen);
bool deal_plan_build_rotate_test(deal_plan_t *out);
bool deal_plan_build_sort(deal_plan_t *out, const char *name, uint8_t specialKind,
                          const char *mainLabel, const char *specialLabel,
                          uint8_t mainDeck, uint8_t specialDeck);

// 按“方案 + 发牌方式”生成计划（调用方提供缓冲，避免与发牌任务共用）
bool deal_build_plan(deal_plan_t *out, uint8_t schemeIndex, deal_order_t order,
                     char *err, size_t errLen);

void deal_plan_print(uint8_t index, const deal_plan_t *plan, const deal_params_t *p);
