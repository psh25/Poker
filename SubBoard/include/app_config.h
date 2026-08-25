#pragma once

#include "pins_config.h"

/**
 * 子板系统级配置：时序、超时、串口、缓冲。
 * 对应 docs/subboard_architecture.md 第四、五章。
 */

// ===== 时序 / 超时（ms，与底板协议一致）=====
#define MOTOR_STARTUP_MS      50    // 电机启动完成判定（占位）
#define PHOTO_TIMEOUT_MS      500   // 光敏超时：发牌电机启动后未检测到牌 → 卡牌/漏发
#define CAMERA_TIMEOUT_MS     2000  // 摄像头识别超时 → 按未知牌处理

// ===== 串口（与底板通信）=====
#define SUB_UART_BAUD         115200
#define SUB_UART_RX_PIN       PIN_UART_RX
#define SUB_UART_TX_PIN       PIN_UART_TX

// ===== 串口接收环形缓冲 =====
#define UART_RX_RING_SIZE     128

// ===== 单卡数据缓冲（架构第六章：只保留当前一张牌，无历史缓存）=====
#define CARD_DATA_MAX         32
