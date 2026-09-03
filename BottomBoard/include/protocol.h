#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/**
 * 底板 ↔ 子板 UART 协议（架构 v2 第七章，草案）
 * 帧格式：0xA5 | type(1B) | len(1B) | data(len) | crc8(1B) | 0xAA
 * CRC 覆盖 type + len + data。
 *
 * type 分配：
 *   0x01~0x0F  底板 → 子板：命令（已用 0x01~0x05）
 *   0x81~0x8F  子板 → 底板：事件（已用 0x81~0x88，统一 EVT_ 前缀，错误事件为 EVT_ERROR_*）
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
    EVT_ERROR_CARD_JAM    = 0x85,  // 光敏超时/卡牌
    EVT_ERROR_MOTOR_STALL = 0x86,  // 发牌电机堵转
    EVT_ERROR_CAM_FAIL    = 0x87,  // 摄像头识别失败
    EVT_ACK               = 0x88   // 收到命令的确认回执（data = 原 type + 原 data）
} proto_evt_t;

typedef struct {
    uint8_t type;
    uint8_t len;
    uint8_t data[PROTO_MAX_DATA];
} proto_frame_t;

// ---- 主机（小程序 BLE / 串口 CLI）↔ 底板：与板间命令同语义复用 ----
// 0x01 发牌 / 0x02 停机 / 0x03 查询 / 0x04 自检 / 0x05 重置 与底板→子板命令同号，
// 底板收到后执行“底板级动作”，其中需要子板的再由底板转发板间帧；
// 底板级独有操作使用 0x10+，底板 → 主机回执/状态使用 0x90+。
typedef enum {
    HOST_CMD_SELECT_SCHEME  = 0x10,  // data[0] = 方案索引 0~SCHEME_COUNT-1
    HOST_CMD_CONFIRM_SCHEME = 0x11,  // 确认当前方案（两段式第一步）
} host_cmd_t;

typedef enum {
    HOST_EVT_ACK   = 0x90,  // data = 原 type + 原 data（调试回执，镜像 EVT_ACK 语义）
    HOST_EVT_STATE = 0x91,  // data = [state, scheme, confirmed]
} host_evt_t;

// ---- 接收解析器（可多实例：Serial1 子板事件 / BLE 主机命令共用同一解析逻辑）----
typedef void (*proto_frame_cb_t)(const proto_frame_t *frame);

typedef struct {
    uint8_t state;              // 内部解析状态（RX_IDLE/RX_TYPE/...）
    proto_frame_t frame;
    uint8_t idx;
    proto_frame_cb_t on_frame;  // 完整帧回调（按通道分发）
} proto_rx_t;

void proto_rx_init(proto_rx_t *rx, proto_frame_cb_t on_frame);
void proto_rx_feed(proto_rx_t *rx, uint8_t b);

uint8_t proto_crc8(const uint8_t *buf, size_t len);

// 发送：入 xSubboardTxQueue，由子板通信任务统一写串口
bool proto_send(uint8_t type, const uint8_t *data, uint8_t len);

// 把帧组装成字节流（帧头..帧尾，含 CRC），返回长度；供板间写串口 / BLE notify 复用
size_t proto_build_frame(uint8_t *buf, const proto_frame_t *frame);

// 子板通信任务使用：
void proto_write_frame(const proto_frame_t *frame);  // 实际写 Serial1
void proto_on_event(const proto_frame_t *frame);     // 解析完成的事件分发
