#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/**
 * 子板 ↔ 底板 UART 协议（docs/subboard_architecture.md 第五章）
 * 帧格式与底板一致：0xA5 | type(1B) | len(1B) | data(len) | crc8(1B) | 0xAA
 * 子板为从设备：接收命令（CMD_*），上报事件（EVT_*、ERROR_*）。
 *
 * type 分配：
 *   0x01~0x0F  底板 → 子板：命令（已用 0x01~0x06）
 *   0x81~0x8F  子板 → 底板：事件（已用 0x81~0x89，统一 EVT_ 前缀，错误事件为 EVT_ERROR_*）
 *   其余 0x10~0x7F / 0x90~0xFF 预留
 * 新增命令：两端 protocol.h 同步加枚举 → 子板 state_machine.cpp 加 case →
 *          （可选）底板 CLI 加文本映射；帧结构 / CRC 无需修改。
 * 数据长度 ≤ PROTO_MAX_DATA(32)；更大载荷需两端同步扩容或分帧。
 */

#define PROTO_HEADER      0xA5
#define PROTO_TAIL        0xAA
#define PROTO_MAX_DATA    32

// 底板 → 子板：命令（0x01~0x0F）
typedef enum {
    CMD_DEAL_START   = 0x01,  // 发一张牌
    CMD_STOP         = 0x02,  // 停止 / 急停
    CMD_STATUS_QUERY = 0x03,  // 查询状态
    CMD_SELF_TEST    = 0x04,  // 触发自检
    CMD_RESET        = 0x05,  // 复位状态机（错误恢复）
    CMD_CAM_CAPTURE  = 0x06   // 触发摄像头截图（拉高 PIN_CAM_TRIG）
} proto_cmd_t;

// 子板 → 底板：事件（0x81~0x8F）
typedef enum {
    EVT_READY         = 0x81,  // 上电自检完成
    EVT_CARD_OUT      = 0x82,  // 光敏检测到一张牌发出
EVT_CARD_VALUE    = 0x83,  // 牌面识别结果
EVT_DEAL_DONE     = 0x84,  // 单张发牌流程完成
EVT_ERROR_CARD_JAM    = 0x85,  // 光敏超时 / 卡牌
EVT_ERROR_MOTOR_STALL = 0x86,  // 发牌电机堵转
    EVT_ERROR_CAM_FAIL    = 0x87,  // 摄像头识别失败
    EVT_ACK               = 0x88,  // 收到命令的确认回执（data = 原 type + 原 data）
    EVT_STATUS            = 0x89   // 子板状态回执（心跳应答）：data = [state, error, countLo, countHi]
} proto_evt_t;

typedef struct {
    uint8_t type;
    uint8_t len;
    uint8_t data[PROTO_MAX_DATA];
} proto_frame_t;

uint8_t proto_crc8(const uint8_t *buf, size_t len);

// 主循环使用：把串口收到的字节喂给解析状态机，完整命令由 proto_on_command 分发
void proto_rx_byte(uint8_t b);
void proto_on_command(const proto_frame_t *frame);

// 上报事件（直接写串口，裸机无队列）
bool proto_send(uint8_t type, const uint8_t *data, uint8_t len);
