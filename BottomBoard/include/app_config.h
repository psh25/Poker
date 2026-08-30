#pragma once

#include "pins_config.h"

/**
 * 系统级配置：任务优先级、核心绑定、栈大小、队列容量、时序。
 * 对应 docs/architecture_v2.md 第三、四章。
 */

// ================= 任务优先级 =================
// ESP32 FreeRTOS：数字越大优先级越高（0~24）。
#define PRIO_BLUETOOTH  3   // 高 (3)   蓝牙通信
#define PRIO_DEAL       2   // 中高 (2) 发牌控制（与子板通信同级，必要时再上调）
#define PRIO_SUBBOARD   2   // 中 (2)   子板通信
#define PRIO_ENCODER    2   // 中 (2)   编码器处理
#define PRIO_DISPLAY    1   // 低 (1)   屏幕显示（事件驱动）
#define PRIO_STATE      1   // 低 (1)   状态管理
#define PRIO_MONITOR    1   // 低 (1)   系统监控（周期巡检）

// ================= 核心绑定 =================
#define CORE_SUBBOARD   0   // 子板通信：固定核心 0
#define CORE_DEAL       1   // 发牌控制：固定核心 1

// ================= 任务栈大小（字节）=================
#define STACK_BLUETOOTH   4096
#define STACK_SUBBOARD    4096
#define STACK_DEAL        4096
#define STACK_ENCODER     3072
#define STACK_DISPLAY     4096
#define STACK_STATE       4096
#define STACK_MONITOR     3072

// ================= 队列容量 =================
#define QUEUE_ENCODER_LEN  10   // 编码器事件
#define QUEUE_BT_RX_LEN    10   // 小程序指令
#define QUEUE_BT_TX_LEN    20   // 待发送数据
#define QUEUE_SUB_RX_LEN   20   // 子板上报事件
#define QUEUE_SUB_TX_LEN   10   // 底板下发指令
#define QUEUE_CAMERA_LEN   20   // 牌面识别结果
#define QUEUE_DISPLAY_LEN  5    // 屏幕显示命令（事件驱动）

// ================= 发牌方案（占位：方案 1~4，具体方案待定）=================
#define SCHEME_COUNT        4

// ================= 时序 / 超时（ms）=================
#define ENCODER_DEBOUNCE_MS    5     // 编码器消抖（架构 v2 异常处理 8.1）
#define PHOTO_TIMEOUT_MS       500   // 光敏超时（与子板协议一致）
#define CAMERA_TIMEOUT_MS      2000  // 摄像头识别超时（与子板协议一致）
#define SUB_RESP_TIMEOUT_MS    1000  // 等待子板响应超时
#define MONITOR_PERIOD_MS      500   // 系统监控巡检周期
#define COMM_HEARTBEAT_MS      1000  // 子板心跳查询间隔
#define COMM_DEAD_TIMEOUT_MS   3000  // 子板掉线判定阈值

// ================= 子板串口 =================
// 物理层为滑环 2 线串口（RX/TX），软件按普通 UART 处理；
// 若板上有差分收发器，对本层透明。
#define SUB_UART_BAUD        115200
#define SUB_UART_RX_PIN      PIN_RING_UART_RX
#define SUB_UART_TX_PIN      PIN_RING_UART_TX
