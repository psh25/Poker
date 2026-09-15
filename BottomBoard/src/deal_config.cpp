/**
 * 参数驱动发牌计划实现（顺序发牌 / 随机发牌）。
 * 详见 deal_config.h。
 */
#include <Arduino.h>
#include <esp_system.h>     // esp_random()：ESP32 硬件随机数
#include <string.h>

#include "protocol.h"       // 牌面编码 CARD_*（特殊牌判定用）
#include "deal_config.h"

static deal_params_t s_params[DEAL_PRESET_COUNT];
static bool s_inited = false;

static const char *kPresetNames[DEAL_PRESET_COUNT] = {
    "Doudizhu",    // 0：斗地主（3×17 + 底牌3，总数 54）
    "Guandan",     // 1：掼蛋（4×27，总数 108 = 两副）
    "Shengji",     // 2：升级（4×25 + 底牌8，总数 108）
    "Texas6",      // 3：德州 6 人（6×2 + 公共5，总数 52 → 弃牌 35）
    "Bridge",      // 4：桥牌（4×13，总数 52）
    "SortJoker",   // 5：分拣·分出大小王（王 → 王堆，其余 → 主堆）
    "SortFace",    // 6：分拣·分开正反面（牌背 → 背面堆，其余 → 正面堆）
    "Custom",      // 7：自定义（串口设参数）
    "Test",        // 8：发牌测试（4×1）—— 测试方案，必须放最后
    "RotateTest",  // 9：旋转测试（只转不发）—— 测试方案，必须放最后
};

// ---------------------------------------------------------------- 参数

void deal_config_init(void) {
    if (s_inited) return;
    s_inited = true;
    for (int i = 0; i < DEAL_PRESET_COUNT; i++) memset(&s_params[i], 0, sizeof(s_params[i]));

    s_params[DEAL_INDEX_DOUDIZHU] = (deal_params_t){ 3, 17, 0, 3, 54  };
    s_params[DEAL_INDEX_GUANDAN]  = (deal_params_t){ 4, 27, 0, 0, 108 };
    s_params[DEAL_INDEX_SHENGJI]  = (deal_params_t){ 4, 25, 0, 8, 108 };
    s_params[DEAL_INDEX_TEXAS6]   = (deal_params_t){ 6, 2,  5, 0, 52  };
    s_params[DEAL_INDEX_BRIDGE]   = (deal_params_t){ 4, 13, 0, 0, 52  };
    s_params[DEAL_INDEX_TEST]     = (deal_params_t){ 4, 1,  0, 0, 4   };
    // SortJoker / SortFace 是分拣方案，不走牌局参数（见 deal_plan_build_sort）；
    // RotateTest 不用参数；Custom 由用户在串口填。
}

uint8_t deal_scheme_count(void) {
    return DEAL_PRESET_COUNT;
}

const char *deal_scheme_name(uint8_t index) {
    if (index >= DEAL_PRESET_COUNT) index = 0;
    return kPresetNames[index];
}

const deal_params_t *deal_scheme_params(uint8_t index) {
    deal_config_init();
    if (index >= DEAL_PRESET_COUNT) return NULL;
    return &s_params[index];
}

deal_params_t *deal_scheme_params_custom(void) {
    deal_config_init();
    return &s_params[DEAL_INDEX_CUSTOM];
}

uint16_t deal_params_required(const deal_params_t *p) {
    if (!p) return 0;
    return (uint16_t)p->players * p->handCards + p->publicCards + p->bottomCards;
}

// 普通牌数 = 总牌数 − 特殊牌数。特殊牌不参与分配表（会被单独挑到别的牌堆），
// 所以分配表按“普通牌数”生成，各牌堆收到的张数才精确。
uint16_t deal_params_normal(const deal_params_t *p) {
    if (!p) return 0;
    return (p->totalCards > p->specialCount) ? (uint16_t)(p->totalCards - p->specialCount) : 0;
}

int16_t deal_params_discard(const deal_params_t *p) {
    if (!p) return -1;
    return (int16_t)deal_params_normal(p) - (int16_t)deal_params_required(p);
}

uint8_t deal_params_piles(const deal_params_t *p) {
    if (!p) return 0;
    uint8_t n = p->players;
    if (p->publicCards > 0) n++;
    if (p->bottomCards > 0) n++;
    // 弃牌堆：普通牌的弃牌要它；即使普通牌没有弃牌，被挑出来的特殊牌也需要一个落点
    if (deal_params_discard(p) > 0 || (p->special != SPECIAL_NONE && p->specialCount > 0)) n++;
    return n;
}

// ---------------------------------------------------------------- 特殊牌（分流规则）

// 牌面编码见 protocol.h：0~51 普通牌、52 小王、53 大王、54 背面、55 未知
bool deal_card_is_special(uint8_t card, uint8_t specialKind) {
    switch (specialKind) {
    case SPECIAL_JOKER_ANY:   return (card == CARD_JOKER_SMALL || card == CARD_JOKER_BIG);
    case SPECIAL_JOKER_SMALL: return (card == CARD_JOKER_SMALL);
    case SPECIAL_JOKER_BIG:   return (card == CARD_JOKER_BIG);
    case SPECIAL_JQK:         return (card < 52 && (card % 13) >= CARD_RANK_J);  // J=10 / Q=11 / K=12
    case SPECIAL_BACK:        return (card == CARD_BACK);
    default:                  return false;
    }
}

const char *deal_special_name(uint8_t specialKind) {
    switch (specialKind) {
    case SPECIAL_JOKER_ANY:   return "joker";
    case SPECIAL_JOKER_SMALL: return "joker_small";
    case SPECIAL_JOKER_BIG:   return "joker_big";
    case SPECIAL_JQK:         return "jqk";
    case SPECIAL_BACK:        return "back";
    default:                  return "none";
    }
}

uint8_t deal_special_from_name(const char *name) {
    if (!name) return SPECIAL_NONE;
    if (strcmp(name, "joker") == 0 || strcmp(name, "jokers") == 0) return SPECIAL_JOKER_ANY;
    if (strcmp(name, "joker_small") == 0) return SPECIAL_JOKER_SMALL;
    if (strcmp(name, "joker_big") == 0)   return SPECIAL_JOKER_BIG;
    if (strcmp(name, "jqk") == 0)         return SPECIAL_JQK;
    if (strcmp(name, "back") == 0)        return SPECIAL_BACK;
    return SPECIAL_NONE;                  // 含 "none" 与未知名字
}

// 一副牌下的常见张数（用户没写 `:张数` 时用它；两副牌请显式写 special=joker:4）
uint8_t deal_special_default_count(uint8_t specialKind) {
    switch (specialKind) {
    case SPECIAL_JOKER_ANY:   return 2;
    case SPECIAL_JOKER_SMALL: return 1;
    case SPECIAL_JOKER_BIG:   return 1;
    case SPECIAL_JQK:         return 12;
    default:                  return 0;   // SPECIAL_BACK：张数取决于牌堆状态，必须由调用方给
    }
}

bool deal_scheme_is_sort(uint8_t index) {
    return (index == DEAL_INDEX_SORT_JOKER || index == DEAL_INDEX_SORT_FACE);
}

// 参数校验；失败写 err
static bool params_check(const deal_params_t *p, char *err, size_t errLen) {
#define FAIL(...) do { if (err && errLen) snprintf(err, errLen, __VA_ARGS__); return false; } while (0)
    if (!p)                     FAIL("参数为空");
    if (p->players == 0)        FAIL("人数为 0");
    if (p->handCards == 0)      FAIL("每人张数为 0");
    if (p->totalCards == 0)     FAIL("总牌数为 0");
    if (p->totalCards > DEAL_TOTAL_CARDS_MAX) FAIL("总牌数超过上限 %d", DEAL_TOTAL_CARDS_MAX);
    if (p->specialCount > p->totalCards) FAIL("特殊牌张数超过总牌数");
    if (p->special != SPECIAL_NONE && p->specialCount == 0) FAIL("声明了特殊牌类别但张数为 0");
    if (deal_params_discard(p) < 0) FAIL("需要的牌超过普通牌数（特殊牌不参与分配）");
    uint8_t piles = deal_params_piles(p);
    if (piles > DECK_COUNT_MAX)  FAIL("需要 %u 个牌堆位，超过 %d", (unsigned)piles, DECK_COUNT_MAX);
#undef FAIL
    return true;
}

// ---------------------------------------------------------------- 计划生成

static void plan_start(deal_plan_t *out, const char *name) {
    memset(out, 0, sizeof(*out));
    out->mode = DEAL_MODE_STACKS;
    strncpy(out->name, name ? name : "Custom", sizeof(out->name) - 1);
}

// 顺序发牌：P1 全部 → P2 全部 → … → 公共 → 底牌（弃牌不发，留在牌源）
bool deal_plan_build_sequential(deal_plan_t *out, const char *name,
                                const deal_params_t *p, char *err, size_t errLen) {
    if (!out) return false;
    plan_start(out, name);
    if (!params_check(p, err, errLen)) return false;

    uint8_t  gi = 0;
    uint16_t total = 0;

    for (uint8_t i = 0; i < p->players; i++) {
        out->groups[gi].deck = gi;
        out->groups[gi].count = p->handCards;
        snprintf(out->groups[gi].label, DEAL_LABEL_LEN, "P%u", (unsigned)i + 1);
        total += p->handCards;
        gi++;
    }
    if (p->publicCards > 0) {
        out->groups[gi].deck = gi;
        out->groups[gi].count = p->publicCards;
        strncpy(out->groups[gi].label, "Public", DEAL_LABEL_LEN - 1);
        total += p->publicCards;
        gi++;
    }
    if (p->bottomCards > 0) {
        out->groups[gi].deck = gi;
        out->groups[gi].count = p->bottomCards;
        strncpy(out->groups[gi].label, "Bottom", DEAL_LABEL_LEN - 1);
        total += p->bottomCards;
        gi++;
    }
    out->groupCount = gi;
    out->totalCards = total;

    // 特殊牌分流：牌局模式下固定送“弃牌堆”（即使普通牌没有弃牌，也要占一个堆位）
    out->sourceCards = p->totalCards;
    if (p->special != SPECIAL_NONE && p->specialCount > 0) {
        out->special      = p->special;
        out->specialCount = p->specialCount;
        out->specialDeck  = gi;                       // 紧挨着普通牌堆的下一个空位
        strncpy(out->specialLabel, "Discard", DEAL_LABEL_LEN - 1);
        out->faceBeforeRotate = true;                 // 落点由牌面决定 → 先拿牌面再转动
    }
    return true;
}

// 牌堆编号 → 标签
static void pile_label(const deal_params_t *p, uint8_t pile, uint8_t idxPublic,
                       uint8_t idxBottom, uint8_t idxDiscard, char *dst) {
    if (pile < p->players)          { snprintf(dst, DEAL_LABEL_LEN, "P%u", (unsigned)pile + 1); return; }
    if (p->publicCards > 0 && pile == idxPublic)  { strncpy(dst, "Public",  DEAL_LABEL_LEN - 1); return; }
    if (p->bottomCards > 0 && pile == idxBottom)  { strncpy(dst, "Bottom",  DEAL_LABEL_LEN - 1); return; }
    if (pile == idxDiscard)                        { strncpy(dst, "Discard", DEAL_LABEL_LEN - 1); return; }
    strncpy(dst, "?", DEAL_LABEL_LEN - 1);
}

// 随机发牌：分配表洗牌后合并连续相同项
bool deal_plan_build_random(deal_plan_t *out, const char *name,
                            const deal_params_t *p, char *err, size_t errLen) {
    if (!out) return false;
    plan_start(out, name);
    if (!params_check(p, err, errLen)) return false;

    const uint8_t players = p->players;
    const int16_t discard = deal_params_discard(p);
    const uint8_t idxPublic  = players;
    const uint8_t idxBottom  = (uint8_t)(players + (p->publicCards > 0 ? 1 : 0));
    const uint8_t idxDiscard = (uint8_t)(idxBottom + (p->bottomCards > 0 ? 1 : 0));

    // 1) 分配表：每个牌堆该拿几张（顺序先摆好，随后整体洗牌）
    uint8_t assign[DEAL_TOTAL_CARDS_MAX];
    uint16_t n = 0;
    for (uint8_t i = 0; i < players; i++) {
        for (uint8_t k = 0; k < p->handCards; k++) assign[n++] = i;
    }
    for (uint8_t k = 0; k < p->publicCards; k++) assign[n++] = idxPublic;
    for (uint8_t k = 0; k < p->bottomCards; k++) assign[n++] = idxBottom;
    for (int16_t k = 0; k < discard; k++)        assign[n++] = idxDiscard;

    // 2) Fisher–Yates 洗牌（硬件随机数）
    for (uint16_t i = n; i > 1; i--) {
        uint16_t j = (uint16_t)(esp_random() % i);
        uint8_t t = assign[i - 1];
        assign[i - 1] = assign[j];
        assign[j] = t;
    }

    // 3) 合并连续相同项 → 发牌组（转盘会按洗牌结果无序地来回转）
    uint8_t gi = 0;
    uint16_t i = 0;
    while (i < n) {
        uint8_t pile = assign[i];
        uint16_t run = 1;
        while (i + run < n && assign[i + run] == pile) run++;
        if (gi >= DEAL_GROUP_MAX) {
            if (err && errLen) snprintf(err, errLen, "发牌组过多");
            return false;
        }
        out->groups[gi].deck = pile;
        out->groups[gi].count = (uint8_t)run;      // run ≤ total ≤ 160
        pile_label(p, pile, idxPublic, idxBottom, idxDiscard, out->groups[gi].label);
        gi++;
        i += run;
    }
    out->groupCount = gi;
    out->totalCards = n;

    // 特殊牌分流：牌局模式下固定送“弃牌堆”（params_check 已保证弃牌堆存在）
    out->sourceCards = p->totalCards;
    if (p->special != SPECIAL_NONE && p->specialCount > 0) {
        out->special      = p->special;
        out->specialCount = p->specialCount;
        out->specialDeck  = idxDiscard;
        strncpy(out->specialLabel, "Discard", DEAL_LABEL_LEN - 1);
        out->faceBeforeRotate = true;
    }
    return true;
}

bool deal_plan_build_rotate_test(deal_plan_t *out) {
    if (!out) return false;
    plan_start(out, kPresetNames[DEAL_INDEX_ROTATE_TEST]);
    out->mode = DEAL_MODE_ROTATE_TEST;
    out->rotateTurns = DEAL_ROTATE_TEST_TURNS;
    return true;
}

// 分拣（DEAL_MODE_ROUTE）：把牌源**全部发完**，未命中规则的进主堆、命中的进专用堆。
// 张数不需要预先知道（specialCount = 0 → 靠“发完牌源”sourceCards 结束）。
bool deal_plan_build_sort(deal_plan_t *out, const char *name, uint8_t specialKind,
                          const char *mainLabel, const char *specialLabel,
                          uint8_t mainDeck, uint8_t specialDeck) {
    if (!out || specialKind == SPECIAL_NONE) return false;
    plan_start(out, name ? name : "Sort");
    out->mode = DEAL_MODE_ROUTE;

    // 填充表只有一条“兜底”项：所有未命中的牌都进主堆
    out->groups[0].deck  = mainDeck;
    out->groups[0].count = DEAL_FILL_REST;
    strncpy(out->groups[0].label, mainLabel ? mainLabel : "Main", DEAL_LABEL_LEN - 1);
    out->groupCount = 1;
    out->totalCards = DEAL_SORT_SOURCE_CARDS;

    out->special          = specialKind;
    out->specialCount     = 0;                     // 未知：不靠张数、靠发完牌源结束
    out->specialDeck      = specialDeck;
    strncpy(out->specialLabel, specialLabel ? specialLabel : "Sort", DEAL_LABEL_LEN - 1);
    out->sourceCards      = DEAL_SORT_SOURCE_CARDS;
    out->faceBeforeRotate = true;
    return true;
}

bool deal_build_plan(deal_plan_t *out, uint8_t schemeIndex, deal_order_t order,
                     char *err, size_t errLen) {
    deal_config_init();
    if (err && errLen) err[0] = '\0';
    if (!out || schemeIndex >= DEAL_PRESET_COUNT) {
        if (err && errLen) snprintf(err, errLen, "方案越界");
        return false;
    }
    if (schemeIndex == DEAL_INDEX_ROTATE_TEST) return deal_plan_build_rotate_test(out);
    // 分拣方案：与发牌方式（顺序/随机）无关，用自己的固定规则
    if (schemeIndex == DEAL_INDEX_SORT_JOKER)
        return deal_plan_build_sort(out, kPresetNames[schemeIndex], SPECIAL_JOKER_ANY,
                                    "Main", "Joker", 0, 1);
    if (schemeIndex == DEAL_INDEX_SORT_FACE)
        return deal_plan_build_sort(out, kPresetNames[schemeIndex], SPECIAL_BACK,
                                    "FaceUp", "FaceDown", 0, 1);

    const deal_params_t *p = &s_params[schemeIndex];
    if (order == DEAL_ORDER_RANDOM) {
        return deal_plan_build_random(out, kPresetNames[schemeIndex], p, err, errLen);
    }
    return deal_plan_build_sequential(out, kPresetNames[schemeIndex], p, err, errLen);
}

// ---------------------------------------------------------------- 打印

void deal_plan_print(uint8_t index, const deal_plan_t *plan, const deal_params_t *p) {
    if (!plan) { Serial.println("[GAME] <null>"); return; }

    if (plan->mode == DEAL_MODE_ROTATE_TEST) {
        Serial.printf("[GAME] game[%u] %s: rotate %u turns (no cards)\n",
                      (unsigned)index, plan->name, (unsigned)plan->rotateTurns);
        return;
    }

    if (plan->mode == DEAL_MODE_ROUTE) {
        Serial.printf("[GAME] game[%u] %s: SORT source=%u cards\n",
                      (unsigned)index, plan->name, (unsigned)plan->sourceCards);
        Serial.printf("[GAME]   %s -> deck%u (%s)；其余 -> deck%u (%s)\n",
                      deal_special_name(plan->special),
                      (unsigned)plan->specialDeck + 1, plan->specialLabel,
                      (unsigned)plan->groups[0].deck + 1, plan->groups[0].label);
        return;
    }

    Serial.printf("[GAME] game[%u] %s: groups=%u cards=%u\n",
                  (unsigned)index, plan->name,
                  (unsigned)plan->groupCount, (unsigned)plan->totalCards);
    if (p) {
        Serial.printf("[GAME]   params players=%u hand=%u public=%u bottom=%u total=%u\n",
                      (unsigned)p->players, (unsigned)p->handCards,
                      (unsigned)p->publicCards, (unsigned)p->bottomCards,
                      (unsigned)p->totalCards);
        Serial.printf("[GAME]   need=%u piles=%u discard=%d\n",
                      (unsigned)deal_params_required(p), (unsigned)deal_params_piles(p),
                      (int)deal_params_discard(p));
        if (p->special != SPECIAL_NONE && p->specialCount > 0) {
            Serial.printf("[GAME]   special=%s x%u（不占分配表名额；先拿牌面再转动）\n",
                          deal_special_name(p->special), (unsigned)p->specialCount);
        }
    }
    if (plan->special != SPECIAL_NONE && plan->specialCount > 0) {
        Serial.printf("[GAME]   special -> deck%u (%s)\n",
                      (unsigned)plan->specialDeck + 1, plan->specialLabel);
    }
    for (uint8_t i = 0; i < plan->groupCount; i++) {
        Serial.printf("[GAME]   g%u deck%u x%u %s\n",
                      (unsigned)i, (unsigned)plan->groups[i].deck + 1,
                      (unsigned)plan->groups[i].count, plan->groups[i].label);
    }
}
