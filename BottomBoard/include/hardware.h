#pragma once

#include <stdint.h>
#include <stdbool.h>

void init_hardware(void);   // GPIO / UART / SPI 初始化
void init_interrupts(void); // 中断挂接（必须在 create_itc 之后调用）
void tmc2209_init(void);    // 底盘步进初始化（AccelStepper；函数名沿用早期版本）
void hall_homing(void);     // 霍尔两段式自动归零

// ---- 外设自检（架构 v2 第一章 / 第八章；实现在 hardware.cpp）----
void self_test(void);        // 开机自检：自动项 + 可观察项（屏幕汇总 + 底盘微动 + 与子板握手）
void selftest_report(void);  // 复检报告：不动作、不阻塞，打印上次结果 + 当前可读状态（CLI `selftest`）
uint16_t selftest_bits(void); // 最近一次自检结果位图（BOT_ST_*）

// 底板侧自检位（本地用，不跨板传输；子板侧位定义在 protocol.h 的 SUB_ST_*）
#define BOT_ST_TFT       0x0001u  // bit0 屏幕：已初始化并画完上电色块（需人眼确认）
#define BOT_ST_CHASSIS   0x0002u  // bit1 底盘步进：已执行微动（需人眼/听声确认）
#define BOT_ST_HALL      0x0004u  // bit2 霍尔：引脚电平已读取（自动；单次读数不能判定极性）
#define BOT_ST_ENC       0x0008u  // bit3 编码器：A/B/SW 电平已读取（自动查空闲电平）
#define BOT_ST_BLE       0x0010u  // bit4 蓝牙：已初始化并开始广播
#define BOT_ST_SUBUART   0x0020u  // bit5 与子板串口：握手成功
#define BOT_ST_SD        0x0040u  // bit6 SD 卡（未接线，跳过）
#define BOT_ST_DONE      0x0080u  // bit7 自检流程完整执行完毕

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
