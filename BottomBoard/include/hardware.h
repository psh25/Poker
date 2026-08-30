#pragma once

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    int direction;       // +1 顺时针, -1 逆时针
    bool pressed;        // 是否按下
    uint32_t timestamp;  // ms 时间戳（用于消抖）
} encoder_event_t;

void init_hardware(void);   // GPIO / UART / SPI 初始化
void init_interrupts(void); // 中断挂接（必须在 create_itc 之后调用）
void tmc2209_init(void);    // TMC 单线 UART：I_scale_analog=0、StallGuard
void hall_homing(void);     // 霍尔两段式自动归零
void self_test(void);       // 外设自检
void led_set(bool on);

// ---- 任务内占位函数（TODO：按架构实现具体逻辑）----
void busy_bluetooth(void);
void busy_subboard_event(uint8_t type, const uint8_t *data, uint8_t len);
void busy_deal_step(const char *step);
void busy_encoder(uint8_t kind);
void busy_display(uint8_t cmdType);
void busy_state_enter(uint8_t state);
void busy_state_exit(uint8_t state);
void busy_monitor(void);
