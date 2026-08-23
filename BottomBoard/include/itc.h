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
extern QueueHandle_t xEncoderQueue;      // 编码器事件 → 编码器处理任务
extern QueueHandle_t xBluetoothRxQueue;  // 小程序指令 → 状态管理任务
extern QueueHandle_t xBluetoothTxQueue;  // 待发送数据 → 蓝牙任务
extern QueueHandle_t xSubboardRxQueue;   // 子板上报事件 → 发牌控制任务
extern QueueHandle_t xSubboardTxQueue;   // 底板下发指令 → 子板通信任务
extern QueueHandle_t xCameraQueue;       // 牌面识别结果 → 发牌/状态管理
extern QueueHandle_t xDisplayQueue;      // 屏幕显示命令 → 显示任务

// ---- 信号量 ----
extern SemaphoreHandle_t xDealSemaphore;     // 发牌就绪，唤醒发牌任务
extern SemaphoreHandle_t xCardDetectedSem;   // EVT_CARD_OUT 到达
extern SemaphoreHandle_t xDeckReadySem;      // EVT_READY / 识别完成

// ---- 互斥量 ----
extern SemaphoreHandle_t xDeckDataMutex;     // 牌堆数据
extern SemaphoreHandle_t xStateMutex;        // 全局状态变量

// ---- 事件组 ----
extern EventGroupHandle_t xStateEventGroup;

#define BIT_USER_INPUT         (1 << 0)  // 编码器旋转/按下
#define BIT_CANCEL             (1 << 1)  // 编码器长按取消选择
#define BIT_DEAL_COMPLETE      (1 << 2)  // 发牌完成
#define BIT_DEAL_ERROR         (1 << 3)  // 发牌异常
#define BIT_CONFIRM_RECEIVED   (1 << 4)  // 小程序确认收到牌堆信息
#define BIT_GAME_END           (1 << 5)  // 牌局结束
#define BIT_RESET              (1 << 6)  // 重置
#define BIT_STATE_CHANGED      (1 << 7)  // 状态已切换（广播）

void create_itc(void);
