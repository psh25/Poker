#pragma once

#include "deal_config.h"
#include "pins_config.h"

/**
 * 系统级配置：任务优先级、核心绑定、栈大小、队列容量、时序。
 * 对应 docs/architecture_v2.md 第三、四章。
 */

// ================= 任务优先级 =================
// ESP32 FreeRTOS：数字越大优先级越高（0~24）。
#define PRIO_BLUETOOTH  3   // 高 (3)   蓝牙通信
#define PRIO_DEAL       2   // 中高 (2) 发牌控制（与子板通信同级，必要时再上调）
#define PRIO_SUBBOARD   2   // 中 (2)   子板通信
#define PRIO_ENCODER    2   // 中 (2)   编码器处理
#define PRIO_DISPLAY    1   // 低 (1)   屏幕显示（事件驱动）
#define PRIO_STATE      1   // 低 (1)   状态管理
#define PRIO_MONITOR    1   // 低 (1)   系统监控（周期巡检）

// ================= 核心绑定 =================
#define CORE_SUBBOARD   0   // 子板通信：固定核心 0
#define CORE_DEAL       1   // 发牌控制：固定核心 1

// ================= 任务栈大小（字节）=================
#define STACK_BLUETOOTH   4096
#define STACK_SUBBOARD    4096
#define STACK_DEAL        4096
#define STACK_ENCODER     3072
#define STACK_DISPLAY     4096
#define STACK_STATE       4096
#define STACK_MONITOR     3072

// ================= 队列容量 =================
#define QUEUE_BT_RX_LEN    10   // 小程序指令字节（蓝牙任务解析）
#define QUEUE_SUB_RX_LEN   20   // 子板上报事件（牌面/出牌/错误统一走这里）
#define QUEUE_SUB_TX_LEN   10   // 底板下发指令
#define QUEUE_DISPLAY_LEN  5    // 屏幕显示命令（事件驱动）

// ================= 发牌方案 / 牌堆 =================
// 方案定义（预置游戏 + 自定义参数）见 deal_config.h：
// 每种牌局只描述参数，发牌任务按生成好的“发牌组列表”统一执行。
#define DECK_COUNT          8     // 实体牌堆位数量（转盘最多 8 个位置）
// 牌堆位置：顶层转盘目标角度（deg；电机实际转角 = ×33/10，见 CHASSIS_GEAR_*）
static const int16_t kDeckAngles[DECK_COUNT] = { 0, 45, 90, 135, 180, 225, 270, 315 };

// ================= 时序 / 超时（ms）=================
#define ENCODER_DEBOUNCE_MS    5     // 编码器消抖（架构 v2 异常处理 8.1）
#define ENCODER_LONG_PRESS_MS  2000  // 长按判定（GAME_ACTIVE：确认结束并回 IDLE）
// 等待子板 EVT_CARD_OUT / EVT_DEAL_DONE 的超时。
// 必须大于子板最坏情况（卡牌 + 撤回 + 重试全部走完才报错）：
//   等牌 1.5s + 撤回 0.8s + 等门清空 0.5s + 重试 1.5s + 撤回 0.8s + 等门清空 0.5s ≈ 5.7s
// 真正的“卡牌”由子板主动上报 EVT_ERROR_CARD_JAM，不靠超时判断；
// 这里留足余量，避免子板还在自救、底板就先误判超时。
#define PHOTO_TIMEOUT_MS       8000
// 等待子板 EVT_CARD_VALUE 的超时。必须大于子板的 CAM_RESULT_TIMEOUT_MS(1200ms)，
// 否则子板还没兜底上报，底板就先超时了（OpenMV 识别约 0.3~0.5s）
#define CAMERA_TIMEOUT_MS      2000
#define MONITOR_PERIOD_MS      500   // 系统监控巡检周期
#define COMM_HEARTBEAT_MS      1000  // 子板心跳查询间隔
#define COMM_DEAD_TIMEOUT_MS   3000  // 子板掉线判定阈值

// ================= 开机自检（见 重要信息/自检流程方案.md）=================
// 只做“不需要人配合”的项目：
//   自动判定：引脚电平读回、BLE 广播、与子板串口握手（发 CMD_STATUS_QUERY 等应答）；
//   人眼/听声确认：屏幕色块、底盘微动 —— 这类没有数字反馈，只能看/听；
//   交互项（转编码器、手转电机试锁轴力矩、拿磁铁试霍尔）不在开机自检里做。
#define SELFTEST_SUB_WAIT_MS        5000  // 等子板 EVT_READY/应答的最长时间（子板不在时才会等满）
#define SELFTEST_SUB_READY_GRACE_MS 300   // 已收到其它帧后，再多等一会儿看有没有 EVT_READY
#define SELFTEST_SUB_PING_MS        500   // 等待期间重发 CMD_STATUS_QUERY 的间隔
#define SELFTEST_CHASSIS_MOVE       1     // 1 = 底盘做 ±SELFTEST_CHASSIS_DEG 微动（可观察）；0 = 跳过
#define SELFTEST_CHASSIS_DEG        3.0F  // 微动角度（顶层，度）；来回各一次，净位移 0，不改变零点
#define SELFTEST_SHOW_MS            2000  // 屏幕显示自检汇总的时间（ms）

// ================= 测试模拟（方案四 TEST；摄像头/光敏未就绪）=================
#define SIM_CAMERA_DELAY_MS    500   // 【临时测试】模拟摄像头识别耗时 0.5s；恢复时改回 5000
#define SIM_PHOTO_DELAY_MS     300   // 模拟光敏确认：下发发牌指令后多久认为已出牌

// ================= 底盘步进（TMC2209 STEP/DIR/ENN，电流由驱动板 VREF 电位器设定；参考 重要信息/步进电机/main.cpp）=================
#define CHASSIS_FULL_STEPS_PER_REV  200      // D42HS3418-13B11：1.8°，200 整步/圈
#define CHASSIS_MICROSTEPS          8        // MS1/MS2 悬空时 1/8 微步
#define CHASSIS_STEPS_PER_REV       (CHASSIS_FULL_STEPS_PER_REV * CHASSIS_MICROSTEPS)
#define CHASSIS_GEAR_NUM            33       // 齿轮传动比：电机 33 齿 : 顶层 10 齿
#define CHASSIS_GEAR_DEN            10
#define CHASSIS_RPM                 180.0F   // 电机轴转速（rpm）；顶层转速 = CHASSIS_RPM ÷ 3.3
#define CHASSIS_ACCEL_STEPS_PER_S2  80000.0F // 梯形加减速（step/s²）,越小启停越柔和
#define CHASSIS_SETTLE_MS           500      // 使能后稳定等待（ms）
#define CHASSIS_MOVE_SETTLE_MS      100      // 每次转到目标位置后的稳定等待（ms）：等机械停稳再发牌
#define CHASSIS_MOVE_TIMEOUT_MS     60000    // 旋转超时保护基准（ms）；长距离按预计用时自动放宽

// ================= 子板串口 =================
// 物理层为滑环 2 线串口（RX/TX），软件按普通 UART 处理；
// 若板上有差分收发器，对本层透明。
#define SUB_UART_BAUD        115200
#define SUB_UART_RX_PIN      PIN_RING_UART_NEG   // 物理 RX：NEG（子板 TX 送来）
#define SUB_UART_TX_PIN      PIN_RING_UART_POS   // 物理 TX：POS（子板 RX 接收）
