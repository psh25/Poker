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
bool tmc_uart_ready(void);                    // TMC2209 UART 是否配置成功
bool tmc_set_hold_current(uint8_t cs);        // 运行时调整 IHOLD（0~31）
bool chassis_rotate_to_angle(int16_t angleDeg);  // 底盘转盘转到指定角度（0~359°），true=到位
bool chassis_rotate_turns(int32_t turns);        // 底盘沿同一方向连续转 turns 圈（顶层圈数），true=到位

// ---- 任务内占位函数（TODO：按架构实现具体逻辑）----
void busy_bluetooth(void);
void busy_subboard_event(uint8_t type, const uint8_t *data, uint8_t len);
void busy_deal_step(const char *step);
void busy_encoder(uint8_t kind);
void busy_display(uint8_t cmdType);
void busy_state_enter(uint8_t state);
void busy_state_exit(uint8_t state);
void busy_monitor(void);
