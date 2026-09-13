#pragma once

/**
 * 底板任务清单（架构 v2 第三章）
 * 创建顺序：低优先级 → 高优先级（见 main.cpp / create_all_tasks）。
 */

void create_all_tasks(void);

// 状态变化 → 主机（BLE notify 0x91），由状态管理任务在切换后调用
void host_notify_state(uint8_t state);

void vBluetoothTask(void *pv);      // 高(3)   队列阻塞
void vSubboardTask(void *pv);       // 中(2)   子板串口中断 + 队列，固定核心 0
void vDealTask(void *pv);           // 中高(2) 信号量触发，固定核心 1
void vEncoderTask(void *pv);        // 中(2)   A/B 中断解码 + SW 轮询
void vDisplayTask(void *pv);        // 低(1)   队列触发（事件驱动）
void vStateManagerTask(void *pv);   // 低(1)   事件组触发
void vMonitorTask(void *pv);        // 低(1)   定时器周期触发
