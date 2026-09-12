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

// ================= 牌面编码（EVT_CARD_VALUE 用）=================
// data[0] = card（1 字节，0~55，共 56 类：54 张牌 + 背面 + 未知）
// data[1] = src （可选，来源/状态；len=1 时按 CARD_SRC_CAMERA 处理）
//
// 花色顺序：黑桃 红桃 梅花 方块；code = 花色*13 + 点数
//   code 0~12   = 黑桃 A,2,3,4,5,6,7,8,9,10,J,Q,K
//   code 13~25  = 红桃 …   26~38 = 梅花 …   39~51 = 方块 …
#define CARD_SUIT_SPADE    0   // 黑桃
#define CARD_SUIT_HEART    1   // 红桃
#define CARD_SUIT_CLUB     2   // 梅花
#define CARD_SUIT_DIAMOND  3   // 方块

#define CARD_RANK_A        0
#define CARD_RANK_2        1
#define CARD_RANK_3        2
#define CARD_RANK_4        3
#define CARD_RANK_5        4
#define CARD_RANK_6        5
#define CARD_RANK_7        6
#define CARD_RANK_8        7
#define CARD_RANK_9        8
#define CARD_RANK_10       9
#define CARD_RANK_J        10
#define CARD_RANK_Q        11
#define CARD_RANK_K        12

#define CARD_CODE(suit, rank) ((uint8_t)((uint8_t)(suit) * 13u + (uint8_t)(rank)))

#define CARD_JOKER_SMALL   52  // 小王
#define CARD_JOKER_BIG     53  // 大王
#define CARD_BACK          54  // 背面（识别到牌背）
#define CARD_UNKNOWN       55  // 未知（没认出来 / 兜底）

// 来源 / 状态
#define CARD_SRC_CAMERA    0   // 摄像头正常识别
#define CARD_SRC_TIMEOUT   1   // 识别超时（未收到回传，按未知兜底）
#define CARD_SRC_LOWCONF   2   // 低置信度（按未知处理）
#define CARD_SRC_DEBUG     3   // 调试 / 模拟

// ================= 开机自检结果位图（EVT_READY 的 data，共 2 字节）=================
// data[0] = 低 8 位，data[1] = 高 8 位；bit = 1 表示“该项已执行且未发现异常”。
//   - 只能靠人眼/听声确认的项目（电机微动等）：置 1 表示“动作已执行，待人工观察”；
//   - bit = 0 表示 未执行 / 无法判定 / 检测到异常。
// 兼容：旧固件 EVT_READY 的 len=0，接收方按“结果未知”处理。
#define SUB_ST_MOTOR      0x0001u  // bit0 发牌电机：已执行正反转微动（需人眼确认）
#define SUB_ST_CAM_CALIB  0x0002u  // bit1 摄像头校准：校准指令已发出并收到回应
#define SUB_ST_CAM_UART   0x0004u  // bit2 摄像头串口：已初始化（子板可下发文本指令）
#define SUB_ST_PHOTO      0x0008u  // bit3 光电门：已读到有效电平（当前电平已记录）
#define SUB_ST_HOST_UART  0x0010u  // bit4 与底板串口：自检期间收到过底板数据
#define SUB_ST_DONE       0x0080u  // bit7 自检流程完整执行完毕

// 底板 → 子板：命令（0x01~0x0F）
typedef enum {
    CMD_DEAL_START   = 0x01,  // 发一张牌
    CMD_STOP         = 0x02,  // 停止 / 急停
    CMD_STATUS_QUERY = 0x03,  // 查询状态
    CMD_SELF_TEST    = 0x04,  // 触发自检
    CMD_RESET        = 0x05,  // 复位状态机（错误恢复）
    CMD_CAM_CAPTURE  = 0x06   // 触发摄像头截图（把 PIN_CAM_TRIG 拉低，OpenMV 下降沿）
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
