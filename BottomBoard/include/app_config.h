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
#define ENCODER_DEBOUNCE_MS    10    // SW 消抖：电平需稳定这么久才认可（原先代码硬编码 10，这里改回用本宏）
#define ENCODER_LONG_PRESS_MS  2000  // 长按判定（IDLE：切换发牌方式；GAME_ACTIVE：确认结束并回 IDLE）
// A/B 相已改由 GPIO 中断做四态解码（见 hardware.cpp）：
// 同方向两次跳变间隔小于此值判为抖动丢弃。人手最快约 1ms 一次跳变，200µs 不会误伤真实转动。
#define ENCODER_ISR_GUARD_US   200
// 编码器任务节拍：A/B 已在中断里解码，任务只消费格数 + 轮询 SW，不需要再跑 1ms。
#define ENCODER_POLL_MS        5
// 等待子板 EVT_CARD_OUT / EVT_DEAL_DONE 的超时。
// 必须大于子板最坏情况（失败 + 撤回 + 重试 + 再失败，全部走完才报错）：
//   单张动作 0.79s（正转0.39 + 刹车0.1 + 反转0.2 + 停顿0.1）
//   + 撤回 0.8s + 等门清空 0.5s + 重试动作 0.79s + 撤回 0.8s + 等门清空 0.5s ≈ 4.2s
// 真正的“卡牌/没出去”由子板主动上报 EVT_ERROR_CARD_JAM，不靠超时判断；
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

// ================= 子板 / 无子板模式 =================
// 底板与子板经常分开测试，这个开关决定"上电时默认认为子板在不在"：
//   USE_SUBBOARD = 1（默认）：正常模式——要求子板在线，一切都走真实子板；
//   USE_SUBBOARD = 0        ：上电即进入"无子板模式"，等同开机后在串口敲 `subsim on`。
// 运行时切换不需要重新编译：底板串口 `subsim on|off`（`simauto` 是旧别名）。
//
// 无子板模式下的行为：
//   ① 跳过"子板不在线就拒绝开局"的检查；
//   ② 不再向子板下发**动作类**命令（截图 / 发牌 / 停机 / 复位），免得误驱动还连着的真子板；
//   ③ 每张牌的牌面、出牌成功、单张完成由底板自己按下面的 SIM_* 延时补发模拟事件。
//   心跳 CMD_STATUS_QUERY 仍然发（只用于在线状态显示，不产生动作）。
#define USE_SUBBOARD 1

// ================= 测试模拟延时（无子板模式 / 方案四 TEST 用）=================
#define SIM_CAMERA_DELAY_MS    500   // 【临时测试】模拟摄像头识别耗时 0.5s；恢复时改回 5000
#define SIM_PHOTO_DELAY_MS     300   // 模拟光敏确认：下发发牌指令后多久认为已出牌

// ================= 底盘步进（TMC2209 STEP/DIR/ENN，电流由驱动板 VREF 电位器设定；参考 重要信息/步进电机/main.cpp）=================
#define CHASSIS_FULL_STEPS_PER_REV  200      // D42HS3418-13B11：1.8°，200 整步/圈
#define CHASSIS_MICROSTEPS          8        // MS1/MS2 悬空时 1/8 微步
#define CHASSIS_STEPS_PER_REV       (CHASSIS_FULL_STEPS_PER_REV * CHASSIS_MICROSTEPS)
#define CHASSIS_GEAR_NUM            33       // 齿轮传动比：电机 33 齿 : 顶层 10 齿
#define CHASSIS_GEAR_DEN            10
#define CHASSIS_RPM                 30.0F   // 电机轴转速（rpm）；顶层转速 = CHASSIS_RPM ÷ 3.3
#define CHASSIS_ACCEL_STEPS_PER_S2  5000.0F // 梯形加减速（step/s²）,越小启停越柔和
#define CHASSIS_SETTLE_MS           500      // 使能后稳定等待（ms）
// 到位稳定等待（等机械停稳再发牌）：**按转速自适应** —— 取"电机轴转一圈的时间 × 系数"。
// 转速提高 → 停下时的残余振动变小 → 等待自动缩短，改 CHASSIS_RPM 后不用再重调这里的常数。
// 系数 0.05 在 30rpm（2s/圈）下正好给出原来的 100ms。
#define CHASSIS_SETTLE_K            0.05F    // 稳定等待 = (60000 / CHASSIS_RPM) × 该系数
#define CHASSIS_SETTLE_MIN_MS       40       // 下限（ms）
#define CHASSIS_SETTLE_MAX_MS       200      // 上限（ms）
// 转动超时（堵转/失步/被挡住的软件兜底）：按"预计用时 × 系数 + 余量"自适应，并保留下限。
// 原来是"预计用时 + 5s，且下限 60s"，实际等于永不触发；现在能真正当保护用。
#define CHASSIS_TIMEOUT_FACTOR      3.0F     // 超时 = 预计用时 × 该系数 + 余量
#define CHASSIS_TIMEOUT_MARGIN_MS   2000     // 余量（ms）
#define CHASSIS_TIMEOUT_MIN_MS      3000     // 超时下限（ms）
// "转盘已经停在某个角度上"的判定容差（电机步数；顶层 1 步 ≈ 0.0068°）
#define CHASSIS_AT_TOL_STEPS        2

// ================= 低功耗（A 级）=================
// 只砍"待机白烧"的电，不改功能、不改任何 ms 级时序。三处：
//
// ① 底盘步进驱动 EN（不锁轴的时段断电）
//    不转的时候把 EN 拉高断开驱动，绕组不再吃保持电流；转盘靠齿轮/摩擦自锁，不需要保持力矩。
//    ⚠️ **DEALING 期间保持使能**：推牌的反作用力会把转盘顶偏，一旦实际角度和软件记录的
//       位置分叉，后面每张牌都会落错堆。所以只在 IDLE / GAME_ACTIVE 断电。
// ② CPU 降频
//    经典 ESP32 在 80MHz 和 240MHz 下 APB **都是 80MHz**（80MHz 时 CPU 直接跑在 APB 上），
//    所以 UART(115200)、屏幕 SPI(10MHz)、AccelStepper 的 setMinPulseWidth 等时基全都不变；
//    millis()/micros()/delay() 走 systimer + FreeRTOS tick，同样与 CPU 频率无关。
//    → 降频只影响"代码跑多快"，不会造成时序错误。发牌/转动性能档仍在 240MHz。
// ③ 蓝牙（见 ble_comms.cpp）
//    控制器休眠 + 放宽广播间隔：手机发现该设备会慢一点点（百 ms 级），待机电流下降明显。
#define CHASSIS_POWER_DOWN_IDLE     1      // 1 = IDLE 空闲一段时间后断开底盘驱动
#define CHASSIS_POWER_DOWN_GAME     1      // 1 = GAME_ACTIVE（一局已发完、等长按）也断开
#define CHASSIS_IDLE_POWER_DOWN_MS  10000  // IDLE 下"最后一次转动之后"多久断电（ms）

#define BOT_CPU_SCALE_ENABLE        1      // 1 = 按状态降频
#define BOT_CPU_IDLE_MHZ            80     // 空闲档（IDLE 且蓝牙未连接）
#define BOT_CPU_PERF_MHZ            240    // 性能档（DEALING / GAME_ACTIVE / 蓝牙已连接）

#define BLE_MODEM_SLEEP_ENABLE      1      // 1 = 打开蓝牙控制器休眠（esp_bt_sleep_enable）
#define BLE_ADV_INTERVAL_UNITS      160    // 广播间隔（单位 0.625ms）160 = 100ms；库默认 32 = 20ms

// ================= 子板串口 =================
// 物理层为滑环 2 线串口（RX/TX），软件按普通 UART 处理；
// 若板上有差分收发器，对本层透明。
#define SUB_UART_BAUD        115200
#define SUB_UART_RX_PIN      PIN_RING_UART_NEG   // 物理 RX：NEG（子板 TX 送来）
#define SUB_UART_TX_PIN      PIN_RING_UART_POS   // 物理 TX：POS（子板 RX 接收）
// 接收改由串口中断写环形缓冲（见 hardware.cpp），通信任务只做解析，不再整帧轮询串口。
#define SUB_RX_RING_SIZE     256   // 环形缓冲大小（字节）
#define SUB_COMM_POLL_MS     5     // 通信任务节拍：决定"解析 + 发队列 + CLI"的兜底频率
