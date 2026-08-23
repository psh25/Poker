#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/**
 * 底板 ↔ 子板 UART 协议（架构 v2 第七章，草案）
 * 帧格式：0xA5 | type(1B) | len(1B) | data(len) | crc8(1B) | 0xAA
 * CRC 覆盖 type + len + data。
 */

#define PROTO_HEADER      0xA5
#define PROTO_TAIL        0xAA
#define PROTO_MAX_DATA    32

// 底板 → 子板：命令（0x01~0x0F）
typedef enum {
    CMD_DEAL_START   = 0x01,  // 发一张牌
    CMD_STOP         = 0x02,  // 停止/急停
    CMD_STATUS_QUERY = 0x03,  // 查询状态
    CMD_SELF_TEST    = 0x04,  // 触发自检
    CMD_RESET        = 0x05   // 复位状态机（错误恢复）
} proto_cmd_t;

// 子板 → 底板：事件（0x81~0x8F）
typedef enum {
    EVT_READY         = 0x81,  // 上电自检完成
    EVT_CARD_OUT      = 0x82,  // 光敏检测到一张牌发出
    EVT_CARD_VALUE    = 0x83,  // 牌面识别结果
    EVT_DEAL_DONE     = 0x84,  // 单张发牌流程完成
    ERROR_CARD_JAM    = 0x85,  // 光敏超时/卡牌
    ERROR_MOTOR_STALL = 0x86,  // 发牌电机堵转
    ERROR_CAM_FAIL    = 0x87   // 摄像头识别失败
} proto_evt_t;

typedef struct {
    uint8_t type;
    uint8_t len;
    uint8_t data[PROTO_MAX_DATA];
} proto_frame_t;

uint8_t proto_crc8(const uint8_t *buf, size_t len);

// 发送：入 xSubboardTxQueue，由子板通信任务统一写串口
bool proto_send(uint8_t type, const uint8_t *data, uint8_t len);

// 子板通信任务使用：
void proto_write_frame(const proto_frame_t *frame);  // 实际写 Serial1
void proto_rx_byte(uint8_t b);                       // 喂给接收解析状态机
void proto_on_event(const proto_frame_t *frame);     // 解析完成的事件分发
