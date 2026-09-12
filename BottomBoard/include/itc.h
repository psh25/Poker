#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/event_groups.h>

#include "app_config.h"

/**
 * 任务间通信与同步机制（架构 v2 第四章）
 * 队列：数据传递；信号量：事件同步；互斥量：共享资源；事件组：状态协调。
 */

// 蓝牙消息（占位：具体小程序协议待定）
typedef struct {
    uint8_t len;
    uint8_t data[64];
} bt_msg_t;

// ---- 队列 ----
extern QueueHandle_t xBluetoothRxQueue;  // 小程序指令字节 → 蓝牙任务解析
extern QueueHandle_t xSubboardRxQueue;   // 子板上报事件 → 发牌控制任务
extern QueueHandle_t xSubboardTxQueue;   // 底板下发指令 → 子板通信任务
extern QueueHandle_t xDisplayQueue;      // 屏幕显示命令 → 显示任务

// ---- 信号量 ----
extern SemaphoreHandle_t xDealSemaphore;     // 发牌就绪，唤醒发牌任务

// ---- 互斥量 ----
extern SemaphoreHandle_t xDeckDataMutex;     // 牌堆数据
extern SemaphoreHandle_t xStateMutex;        // 全局状态变量

// ---- 事件组 ----
extern EventGroupHandle_t xStateEventGroup;

#define BIT_DEAL_COMPLETE      (1 << 0)  // 发牌完成
#define BIT_DEAL_ERROR         (1 << 1)  // 发牌异常
#define BIT_RESET              (1 << 3)  // 重置 / 结束回 IDLE（GAME_ACTIVE 长按等）
#define BIT_DEAL_CONFIRM       (1 << 5)  // IDLE 下按下确认 → DEALING

// CLI 调试：强制切换到指定状态（测试各状态显示/功能）
#define BIT_TEST_IDLE          (1 << 6)
#define BIT_TEST_DEALING       (1 << 7)
#define BIT_TEST_ACTIVE        (1 << 8)

void create_itc(void);
