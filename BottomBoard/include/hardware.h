#pragma once

#include <stdint.h>
#include <stdbool.h>

void init_hardware(void);   // GPIO / UART / SPI 初始化
void init_interrupts(void); // 中断挂接：编码器 A/B 相（必须在 create_itc 之后调用）
void tmc2209_init(void);    // 底盘步进初始化（AccelStepper；函数名沿用早期版本）
void hall_homing(void);     // 霍尔两段式自动归零

// ---- 旋转编码器：A/B 相走 GPIO 中断 + 四态正交解码（实现在 hardware.cpp）----
// 中断里只累加计数，任务通过 encoder_take_steps() 消费，避免 1ms 轮询在快转时丢步。
int32_t encoder_take_steps(void);   // 取走累计格数（同时清零）；+1/-1 = 顺/逆时针一格
void    encoder_reset_steps(void);  // 丢弃累计（开机/自检后清一次）

// ---- 子板串口接收：中断写环形缓冲 → 子板通信任务读取 ----
bool sub_uart_read_byte(uint8_t *b);   // true = 取到一个字节

// ---- 外设自检（架构 v2 第一章 / 第八章；实现在 hardware.cpp）----
void self_test(void);        // 开机自检：自动项 + 可观察项（屏幕汇总 + 底盘微动 + 与子板握手）
void selftest_report(void);  // 复检报告：不动作、不阻塞，打印上次结果 + 当前可读状态（CLI `selftest`）
uint16_t selftest_bits(void); // 最近一次自检结果位图（BOT_ST_*）

// 底板侧自检位（本地用，不跨板传输；子板侧位定义在 protocol.h 的 SUB_ST_*）
// ⚠️ 规则（2026-09-17）：**只保留"固件自己能判定成败"的项目**。
//    判定标准：固件能不能**不依赖人**给出通过/不通过。做不到的一律不进位图，只打印。
//    被删掉的（以前都无条件报 OK，属于假通过）：
//      · 屏幕色块 —— 要人看颜色/顺序；
//      · 编码器 A/B/SW 电平 —— INPUT_PULLUP 下"没接"和"空闲高"读数完全一样，判不出来；
//      · 蓝牙广播 —— "手机能搜到"要人找，协议栈内部状态也无法证明射频真的在工作；
//      · 底盘微动 —— 没有霍尔/编码器反馈，只能听/看电机转不转；
//      · 霍尔（功能已删除）、SD（未接线）。
#define BOT_ST_SUBUART   0x0001u  // bit0 与子板串口：握手成功（真正的请求/应答往返）
#define BOT_ST_DONE      0x0080u  // bit7 自检流程完整执行完毕

bool chassis_rotate_to_angle(int16_t angleDeg);  // 底盘转盘转到指定角度（0~359°），true=到位
bool chassis_rotate_turns(int32_t turns);        // 底盘沿同一方向连续转 turns 圈（顶层圈数），true=到位
bool chassis_at_angle(int16_t angleDeg);         // 转盘当前是否已停在该角度上（按电机步数判断，容差 CHASSIS_AT_TOL_STEPS）

// ---- 低功耗（A 级，见 app_config.h）----
// 底盘驱动：不锁轴的时段（IDLE 空闲 / GAME_ACTIVE）断电；DEALING 期间保持使能锁轴。
void chassis_enable_driver(void);    // 使能底盘驱动（EN 拉低 + CHASSIS_SETTLE_MS 稳定等待，会阻塞）
bool chassis_driver_enabled(void);   // 当前 EN 是否使能（锁轴中）
void chassis_power_release(void);    // 断开底盘驱动（EN 拉高，绕组断电，不锁轴）
void power_set_cpu(uint32_t mhz);    // 设置 CPU 主频（0 或相同值 = 不动）
void power_manage_tick(void);        // 低功耗巡检（由 busy_monitor 每 MONITOR_PERIOD_MS 调用）

// ---- 中止请求（STOP/RESET 用；底盘运动循环与发牌任务都会尽早退出）----
// 带"代次"防竞态：STOP/RESET 递增代次；发牌任务先采样代次、只有代次没变才允许清除标志，
// 避免"新一局把启动窗口内到达的 STOP/RESET 清掉"（见 tasks_deal.cpp 的 vDealTask）。
void     motion_abort_request(void);
bool     motion_abort_requested(void);
uint32_t motion_abort_generation(void);                    // 取当前代次
bool     motion_abort_clear_if_generation(uint32_t gen);   // 代次未变才清除；true = 已清除

// ---- 任务内占位函数（TODO：按架构实现具体逻辑）----
void busy_subboard_event(uint8_t type, const uint8_t *data, uint8_t len);
void busy_deal_step(const char *step);
void busy_state_enter(uint8_t state);
void busy_state_exit(uint8_t state);
void busy_monitor(void);
