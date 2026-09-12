/**
 * 发牌选择状态实现（见 deal_selection.h）
 * 存储 + 互斥量保护：xSelectionMutex 在 create_itc() 里创建。
 */
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "app_config.h"
#include "itc.h"
#include "deal_selection.h"

static deal_selection_t s_sel = { 0, false, false };

// 内核对象创建前（调度器启动前）互斥量为空，此时直接读写即可
static inline void sel_lock(void)   { if (xSelectionMutex) xSemaphoreTake(xSelectionMutex, portMAX_DELAY); }
static inline void sel_unlock(void) { if (xSelectionMutex) xSemaphoreGive(xSelectionMutex); }

void deal_selection_snapshot(deal_selection_t *out) {
    if (!out) return;
    sel_lock();
    *out = s_sel;
    sel_unlock();
}

uint8_t deal_selection_get_scheme(void) {
    sel_lock();
    uint8_t v = s_sel.scheme;
    sel_unlock();
    return v;
}

bool deal_selection_get_confirmed(void) {
    sel_lock();
    bool v = s_sel.confirmed;
    sel_unlock();
    return v;
}

bool deal_selection_get_order_random(void) {
    sel_lock();
    bool v = s_sel.orderRandom;
    sel_unlock();
    return v;
}

void deal_selection_set_scheme(uint8_t scheme) {
    sel_lock();
    s_sel.scheme = scheme;
    s_sel.confirmed = false;   // 改选 = 取消已确认（旋转切换时保持一致）
    sel_unlock();
}

void deal_selection_set_confirmed(bool on) {
    sel_lock();
    s_sel.confirmed = on;
    sel_unlock();
}

void deal_selection_set_order_random(bool rnd) {
    sel_lock();
    s_sel.orderRandom = rnd;
    sel_unlock();
}
