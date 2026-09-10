/**
 * 参数驱动发牌计划实现（预置方案 + 自定义参数生成）。
 * 详见 deal_config.h。
 */
#include <Arduino.h>
#include <string.h>

#include "deal_config.h"

static deal_plan_t s_plans[DEAL_PRESET_COUNT];
static bool s_inited = false;

static const char *kPresetNames[DEAL_PRESET_COUNT] = {
    "Doudizhu",   // 0：斗地主（3×17 + 底牌3）
    "Guandan",    // 1：掼蛋（4×27）
    "RotateTest", // 2：旋转测试
    "Test",       // 3：发牌测试（4×1）
    "Shengji",    // 4：升级（4×25 + 底牌8）
    "Texas6",     // 5：德州 6 人（6×2 + 公共5）
    "Bridge",     // 6：桥牌（4×13）
    "Custom",     // 7：自定义
};

static void plan_clear(deal_plan_t *p) {
    memset(p, 0, sizeof(*p));
    p->mode = DEAL_MODE_STACKS;
}

uint16_t deal_plan_total_cards(const deal_plan_t *plan) {
    if (!plan) return 0;
    if (plan->mode == DEAL_MODE_ROTATE_TEST) return 0;
    return plan->totalCards;
}

bool deal_plan_build(deal_plan_t *out, const char *name, const deal_params_t *p,
                     char *err, size_t errLen) {
    if (!out || !p) return false;
    if (err && errLen) err[0] = '\0';
    plan_clear(out);
    strncpy(out->name, name ? name : "Custom", sizeof(out->name) - 1);
    out->mode = DEAL_MODE_STACKS;

    if (p->players > 0 && p->handCards == 0) {
        if (err && errLen) snprintf(err, errLen, "hand=0 但 players>0");
        return false;
    }

    uint8_t gi = 0;
    uint16_t total = 0;

    if (p->handCards > 0) {
        for (uint8_t i = 0; i < p->players; i++) {
            if (gi >= DEAL_GROUP_MAX) {
                if (err && errLen) snprintf(err, errLen, "发牌组>%d，超出牌堆位数", DEAL_GROUP_MAX);
                return false;
            }
            out->groups[gi].deck = gi;
            out->groups[gi].count = p->handCards;
            snprintf(out->groups[gi].label, sizeof(out->groups[gi].label), "P%u", (unsigned)i + 1);
            total += p->handCards;
            gi++;
        }
    }
    if (p->publicCards > 0) {
        if (gi >= DEAL_GROUP_MAX) {
            if (err && errLen) snprintf(err, errLen, "发牌组>%d，超出牌堆位数", DEAL_GROUP_MAX);
            return false;
        }
        out->groups[gi].deck = gi;
        out->groups[gi].count = p->publicCards;
        strncpy(out->groups[gi].label, "Public", sizeof(out->groups[gi].label) - 1);
        total += p->publicCards;
        gi++;
    }
    if (p->bottomCards > 0) {
        if (gi >= DEAL_GROUP_MAX) {
            if (err && errLen) snprintf(err, errLen, "发牌组>%d，超出牌堆位数", DEAL_GROUP_MAX);
            return false;
        }
        out->groups[gi].deck = gi;
        out->groups[gi].count = p->bottomCards;
        strncpy(out->groups[gi].label, "Bottom", sizeof(out->groups[gi].label) - 1);
        total += p->bottomCards;
        gi++;
    }

    if (gi == 0) {
        if (err && errLen) snprintf(err, errLen, "没有任何发牌组（全为 0）");
        return false;
    }
    if (total > DEAL_TOTAL_CARDS_MAX) {
        if (err && errLen) snprintf(err, errLen, "总张数 %u > 上限 %d", (unsigned)total, DEAL_TOTAL_CARDS_MAX);
        return false;
    }

    out->groupCount = gi;
    out->totalCards = total;
    return true;
}

static void build_rotate_test(deal_plan_t *p) {
    plan_clear(p);
    strncpy(p->name, kPresetNames[DEAL_INDEX_ROTATE_TEST], sizeof(p->name) - 1);
    p->mode = DEAL_MODE_ROTATE_TEST;
    p->rotateTurns = DEAL_ROTATE_TEST_TURNS;
    p->groupCount = 0;
    p->totalCards = 0;
}

void deal_config_init(void) {
    if (s_inited) return;
    s_inited = true;
    plan_clear(&s_plans[DEAL_INDEX_CUSTOM]);
    strncpy(s_plans[DEAL_INDEX_CUSTOM].name, "Custom", sizeof(s_plans[0].name) - 1);

    struct { uint8_t idx; deal_params_t p; } presets[] = {
        { DEAL_INDEX_DOUDIZHU, { 3, 17, 0, 3 } },
        { DEAL_INDEX_GUANDAN,  { 4, 27, 0, 0 } },
        { DEAL_INDEX_TEST,     { 4, 1,  0, 0 } },
        { DEAL_INDEX_SHENGJI,  { 4, 25, 0, 8 } },
        { DEAL_INDEX_TEXAS6,   { 6, 2,  5, 0 } },
        { DEAL_INDEX_BRIDGE,   { 4, 13, 0, 0 } },
    };
    for (size_t i = 0; i < sizeof(presets) / sizeof(presets[0]); i++) {
        char err[48];
        if (!deal_plan_build(&s_plans[presets[i].idx], kPresetNames[presets[i].idx],
                             &presets[i].p, err, sizeof(err))) {
            Serial.printf("[GAME] preset %s build failed: %s\n",
                          kPresetNames[presets[i].idx], err);
        }
    }
    build_rotate_test(&s_plans[DEAL_INDEX_ROTATE_TEST]);
}

uint8_t deal_scheme_count(void) {
    return DEAL_PRESET_COUNT;
}

const char *deal_scheme_name(uint8_t index) {
    deal_config_init();
    if (index >= DEAL_PRESET_COUNT) index = 0;
    if (s_plans[index].name[0]) return s_plans[index].name;
    return kPresetNames[index];
}

const deal_plan_t *deal_scheme_plan(uint8_t index) {
    deal_config_init();
    if (index >= DEAL_PRESET_COUNT) return NULL;
    return &s_plans[index];
}

deal_plan_t *deal_scheme_custom(void) {
    deal_config_init();
    return &s_plans[DEAL_INDEX_CUSTOM];
}

void deal_plan_print(uint8_t index, const deal_plan_t *plan) {
    if (!plan) { Serial.println("[GAME] <null>"); return; }
    if (plan->mode == DEAL_MODE_ROTATE_TEST) {
        Serial.printf("[GAME] game[%u] %s: rotate %u turns (no cards)\n",
                      (unsigned)index, plan->name, (unsigned)plan->rotateTurns);
        return;
    }
    Serial.printf("[GAME] game[%u] %s: groups=%u cards=%u\n",
                  (unsigned)index, plan->name,
                  (unsigned)plan->groupCount, (unsigned)plan->totalCards);
    for (uint8_t i = 0; i < plan->groupCount; i++) {
        Serial.printf("[GAME]   g%u deck%u x%u %s\n",
                      (unsigned)i, (unsigned)plan->groups[i].deck + 1,
                      (unsigned)plan->groups[i].count, plan->groups[i].label);
    }
}
