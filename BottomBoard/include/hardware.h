#pragma once

#include <stdint.h>
#include <stdbool.h>

void init_hardware(void);   // GPIO / UART / SPI 初始化
void init_interrupts(void); // 中断挂接（必须在 create_itc 之后调用）
void tmc2209_init(void);    // 底盘步进初始化（AccelStepper；函数名沿用早期版本）
void hall_homing(void);     // 霍尔两段式自动归零
void self_test(void);       // 外设自检
bool chassis_rotate_to_angle(int16_t angleDeg);  // 底盘转盘转到指定角度（0~359°），true=到位
bool chassis_rotate_turns(int32_t turns);        // 底盘沿同一方向连续转 turns 圈（顶层圈数），true=到位

// ---- 中止请求（STOP/RESET 用；底盘运动循环与发牌任务都会尽早退出）----
void motion_abort_request(void);
void motion_abort_clear(void);
bool motion_abort_requested(void);

// ---- 任务内占位函数（TODO：按架构实现具体逻辑）----
void busy_subboard_event(uint8_t type, const uint8_t *data, uint8_t len);
void busy_deal_step(const char *step);
void busy_state_enter(uint8_t state);
void busy_state_exit(uint8_t state);
void busy_monitor(void);
