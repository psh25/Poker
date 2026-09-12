/**
 * 内核对象创建（架构 v2 第四章）
 * 队列、信号量、互斥量、事件组；任何对象创建失败都视为致命错误。
 */
#include <Arduino.h>

#include "app_config.h"
#include "itc.h"
#include "hardware.h"
#include "protocol.h"
#include "display.h"

QueueHandle_t xBluetoothRxQueue = NULL;
QueueHandle_t xSubboardRxQueue = NULL;
QueueHandle_t xSubboardTxQueue = NULL;
QueueHandle_t xDisplayQueue = NULL;

SemaphoreHandle_t xDealSemaphore = NULL;
SemaphoreHandle_t xDeckDataMutex = NULL;
SemaphoreHandle_t xStateMutex = NULL;
SemaphoreHandle_t xSelectionMutex = NULL;

EventGroupHandle_t xStateEventGroup = NULL;

void create_itc(void) {
    xBluetoothRxQueue = xQueueCreate(QUEUE_BT_RX_LEN,   sizeof(bt_msg_t));
    xSubboardRxQueue  = xQueueCreate(QUEUE_SUB_RX_LEN,  sizeof(proto_frame_t));
    xSubboardTxQueue  = xQueueCreate(QUEUE_SUB_TX_LEN,  sizeof(proto_frame_t));
    xDisplayQueue     = xQueueCreate(QUEUE_DISPLAY_LEN, sizeof(display_cmd_t));

    xDealSemaphore    = xSemaphoreCreateBinary();
    xDeckDataMutex    = xSemaphoreCreateMutex();
    xStateMutex       = xSemaphoreCreateMutex();
    xSelectionMutex   = xSemaphoreCreateMutex();

    xStateEventGroup  = xEventGroupCreate();

    bool ok = (xBluetoothRxQueue && xSubboardRxQueue && xSubboardTxQueue &&
               xDisplayQueue && xDealSemaphore &&
               xDeckDataMutex && xStateMutex && xSelectionMutex && xStateEventGroup);
    if (!ok) {
        Serial.println("[FATAL] 内核对象创建失败");
        while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }
}
