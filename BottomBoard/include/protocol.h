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
// 子板自检结果，随 EVT_READY 上报；data[0] = 低 8 位，data[1] = 高 8 位。
// ⚠️ 规则（2026-09-17）：**只收"子板自己能判定成败"的项目**（与 SubBoard/include/protocol.h 保持一致）。
//    电机微动、光电门静态电平这些只能靠人看/听的项**不置位**，绝不会因为"动过了"就报 OK。
#define SUB_ST_CAM_CALIB  0x0001u  // bit0 摄像头校准：校准指令已发出且收到非 ERROR 回应（真判定）
#define SUB_ST_HOST_UART  0x0002u  // bit1 与底板串口：自检期间收到过底板数据（真判定）
#define SUB_ST_DONE       0x0080u  // bit7 自检流程完整执行完毕

// 底板 → 子板：命令（0x01~0x0F）
typedef enum {
    CMD_DEAL_START   = 0x01,  // 发一张牌
    CMD_STOP         = 0x02,  // 停止/急停
    CMD_STATUS_QUERY = 0x03,  // 查询状态
    CMD_SELF_TEST    = 0x04,  // 触发自检
    CMD_RESET        = 0x05,  // 复位状态机（错误恢复）
    CMD_CAM_CAPTURE  = 0x06   // 触发摄像头截图（子板把 PIN_CAM_TRIG 拉低，OpenMV 下降沿）
} proto_cmd_t;

// 子板 → 底板：事件（0x81~0x8F）
// ⚠️ 这个范围是**唯一**的"子板在线证据"：只有落在这个区间里的帧，才说明子板真的在应答
//    （见 protocol.cpp 的 proto_on_event 与 sub_comm_online）。范围外的帧一律丢弃——
//    底板自己发出的命令（0x01~0x06）出现在 RX 上只可能是回环（TX/RX 短接）或外来帧。
#define PROTO_SUB_EVT_MIN   0x81
#define PROTO_SUB_EVT_MAX   0x8F
typedef enum {
    EVT_READY         = 0x81,  // 上电自检完成
    EVT_CARD_OUT      = 0x82,  // 光敏检测到一张牌发出
    EVT_CARD_VALUE    = 0x83,  // 牌面识别结果
    EVT_DEAL_DONE     = 0x84,  // 单张发牌流程完成
    EVT_ERROR_CARD_JAM    = 0x85,  // 光敏超时/卡牌
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

// ---- 主机（小程序 BLE / 串口 CLI）↔ 底板：与板间命令同语义复用 ----
// 0x01 发牌 / 0x02 停机 / 0x03 查询 / 0x04 自检 / 0x05 重置 / 0x06 截图 与底板→子板命令同号，
// 底板收到后执行“底板级动作”，其中需要子板的再由底板转发板间帧；
// 底板级独有操作使用 0x10+，底板 → 主机回执/状态使用 0x90+。
typedef enum {
    HOST_CMD_SELECT_SCHEME  = 0x10,  // data[0] = 方案索引 0~SCHEME_COUNT-1
    HOST_CMD_CONFIRM_SCHEME = 0x11,  // 确认当前方案（两段式第一步）
    HOST_CMD_PLAY_TEXT      = 0x13,  // 线上出牌文本，UTF-8
    HOST_CMD_PLAN_BEGIN     = 0x14,  // data=[scheme,totalLo,totalHi,pileCount]
    HOST_CMD_PLAN_CHUNK     = 0x15,  // data=[offset,deckIndex0,deckIndex1,...]
    HOST_CMD_PLAN_COMMIT    = 0x16,  // 提交完整发牌计划
} host_cmd_t;

typedef enum {
    HOST_EVT_ACK       = 0x90,  // data = 原 type + 原 data（调试回执）
    HOST_EVT_STATE     = 0x91,  // data = [state, scheme, confirmed]
    HOST_EVT_CARD      = 0x92,  // data = [idxLo,idxHi,pile,suit,rank,simulated]
    HOST_EVT_DEAL_DONE = 0x93,  // data = [totalLo,totalHi]
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

// ---- 子板心跳（只有"子板事件帧"0x81~0x8F 会刷新时间戳）----
bool     proto_is_sub_event(uint8_t type);  // true = 属于子板 → 底板的事件（0x81~0x8F）
void     proto_note_sub_rx(void);
bool     sub_comm_online(void);            // true = COMM_DEAD_TIMEOUT_MS 内有收到子板事件帧
uint32_t sub_comm_last_rx_ms(void);        // 最近一次收到子板事件帧的 millis()
uint16_t sub_comm_foreign_frames(void);    // 被 guard 丢弃的"非子板帧"个数（>0 = 这条线不干净）

// ---- 自检请求闩锁（P4：区分"复检上报"与"子板自己重启"）----
// 底板每次 proto_send(CMD_SELF_TEST) 会自动置位；子板随后回的 EVT_READY 属于**复检上报**，
// 被本函数取走即清空（一次请求只对应一帧）。若取不到却收到 EVT_READY，说明子板是**自己重启**的。
bool proto_take_selftest_ready(void);
