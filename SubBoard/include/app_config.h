#pragma once

#include "pins_config.h"

/**
 * 子板系统级配置：时序、超时、串口、缓冲。
 * 对应 docs/subboard_architecture.md 第四、五章。
 */

// ===== 发牌电机：定时出牌模式参数（ms / PWM 占空比 0-255）=====
#define MOTOR_FWD_MS 390   // 正转出牌时长：一张牌送出的时间
#define MOTOR_REV_MS 200   // 出牌后反转回退时长（让下一张退到摄像头可拍位置；0=不反转）
#define MOTOR_BRAKE_MS 100 // 正转→反转之间的短刹车（AIN1=AIN2=高，PWM=0；0=直接换向）
#define MOTOR_PAUSE_MS 100 // 每张牌之间的停顿（让牌完全出去、牌堆复位；0=不停）
#define MOTOR_DUTY 200     // 正转 PWM 占空比（约 78%）
#define MOTOR_REV_DUTY 160 // 反转 PWM 占空比（约 63%）

// 出牌触发方式：
//   0 = 定时出牌（无光敏时调试用，推荐）：正转 MOTOR_FWD_MS → 刹车 MOTOR_BRAKE_MS → 反转 MOTOR_REV_MS → 停顿 → 回传完成
//   1 = 光敏触发（量产）：启动电机后等光敏检测到牌通过（PHOTO_TIMEOUT_MS 超时 = 卡牌）
#define USE_PHOTO_SENSOR 0

// ===== 自动连续发牌（调试用：不需要底板，自己一张接一张发）=====
// AUTO_DEAL_ENABLE = 1：上电延时 AUTO_DEAL_START_DELAY_MS 后自动开始连续发牌
//   （留时间在串口敲 "auto off" 取消）
// 运行时串口命令：auto            → 无限自动发牌
//                 auto <n>        → 自动发 n 张后停
//                 auto off        → 停止自动发牌（当前这张发完为止）
#define AUTO_DEAL_ENABLE 0
#define AUTO_DEAL_COUNT -1            // 自动发牌张数：-1 = 无限循环；N = 发 N 张后停
#define AUTO_DEAL_START_DELAY_MS 1000 // 上电延时（ms），留时间取消

// ===== 时序 / 超时（ms，与底板协议一致）=====
#define MOTOR_STARTUP_MS 50    // 电机启动完成判定（光敏模式用）
#define PHOTO_TIMEOUT_MS 500   // 光敏超时：发牌电机启动后未检测到牌 → 卡牌/漏发
#define CAMERA_TIMEOUT_MS 2000 // 摄像头识别超时 → 按未知牌处理

// ===== 串口（与底板通信）=====
#define SUB_UART_BAUD 115200
#define SUB_UART_RX_PIN PIN_UART_POS   // 物理 RX：POS=38（底板 POS=TX45 送来）
#define SUB_UART_TX_PIN PIN_UART_NEG   // 物理 TX：NEG=39（底板 NEG=RX48 接收）

// ===== 串口接收环形缓冲 =====
#define UART_RX_RING_SIZE 128

// ===== 单卡数据缓冲（架构第六章：只保留当前一张牌，无历史缓存）=====
#define CARD_DATA_MAX 32
