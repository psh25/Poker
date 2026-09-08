#pragma once

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
#define QUEUE_ENCODER_LEN  10   // 编码器事件
#define QUEUE_BT_RX_LEN    10   // 小程序指令
#define QUEUE_BT_TX_LEN    20   // 待发送数据
#define QUEUE_SUB_RX_LEN   20   // 子板上报事件
#define QUEUE_SUB_TX_LEN   10   // 底板下发指令
#define QUEUE_CAMERA_LEN   20   // 牌面识别结果
#define QUEUE_DISPLAY_LEN  5    // 屏幕显示命令（事件驱动）

// ================= 发牌方案（占位：方案 1~4，具体方案待定）=================
#define SCHEME_COUNT        4
// 方案三（索引 2）= 底盘连续旋转测试（不发牌）；方案四（索引 3）= TEST 发牌测试；方案 1~2 未定义
#define SCHEME_ROTATE_TEST_INDEX  2
#define SCHEME_TEST_INDEX         3
#define ROTATE_TEST_TOPTURNS      10   // 方案三：顶层连续旋转圈数（电机步数按 33:10 齿轮比换算）

// ================= 发牌模式预设（占位：模式未定，先定共同结构）=================
#define DECK_COUNT          4     // 牌堆数量（占位）
#define ROTATE_WAIT_MS      1500  // 步进电机到位后的稳定等待（ms；备用，实际旋转由 AccelStepper 完成）
#define DEAL_TOTAL_CARDS    4     // 每局发牌总张数（占位）

// 牌堆位置：顶层转盘目标角度（deg；电机实际转角 = ×33/10，见 CHASSIS_GEAR_*）
static const int16_t kDeckAngles[DECK_COUNT] = { 0, 90, 180, 270 };

// 每张牌的目标牌堆（按发牌顺序；占位：依次从 1→4 号牌堆各发一张）
static const uint8_t kDealDeckSequence[DEAL_TOTAL_CARDS] = { 0, 1, 2, 3 };

// ================= 时序 / 超时（ms）=================
#define ENCODER_DEBOUNCE_MS    5     // 编码器消抖（架构 v2 异常处理 8.1）
#define ENCODER_LONG_PRESS_MS  2000  // 长按判定（GAME_ACTIVE：确认结束并回 IDLE）
#define PHOTO_TIMEOUT_MS       500   // 光敏超时（与子板协议一致）
#define CAMERA_TIMEOUT_MS      1000  // 【临时测试】牌面识别等待 0.5s，超时 1s；恢复 5s 识别时改回 6000
#define SUB_RESP_TIMEOUT_MS    1000  // 等待子板响应超时
#define MONITOR_PERIOD_MS      500   // 系统监控巡检周期
#define COMM_HEARTBEAT_MS      1000  // 子板心跳查询间隔
#define COMM_DEAD_TIMEOUT_MS   3000  // 子板掉线判定阈值

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
#define CHASSIS_MOVE_TIMEOUT_MS     60000    // 旋转超时保护基准（ms）；长距离按预计用时自动放宽

// ================= 子板串口 =================
// 物理层为滑环 2 线串口（RX/TX），软件按普通 UART 处理；
// 若板上有差分收发器，对本层透明。
#define SUB_UART_BAUD        115200
#define SUB_UART_RX_PIN      PIN_RING_UART_NEG   // 物理 RX：NEG=48（子板 NEG=TX39 送来）
#define SUB_UART_TX_PIN      PIN_RING_UART_POS   // 物理 TX：POS=45（子板 POS=RX38 接收）
