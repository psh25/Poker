/**
 * 硬件初始化与占位函数（架构 v2 第一章 / 第八章）
 * 步进转动由 AccelStepper 以 STEP/DIR/ENN 驱动（基础结构参考 重要信息/步进电机/main.cpp；
 * 为缓解启停瞬间电流突变，采用 AccelStepper 的梯形加减速 run()）；
 * 电流由驱动板 VREF 电位器设定（与参考程序一致，不使用 TMC 单线 UART）。
 */
#include <Arduino.h>
#include <AccelStepper.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

#include "pins_config.h"
#include "app_config.h"
#include "itc.h"
#include "hardware.h"
#include "protocol.h"
#include "display.h"
#include "ble_comms.h"
#include "state_machine.h"

// ---- 底盘步进（AccelStepper，参考 重要信息/步进电机/main.cpp）----
static AccelStepper g_chassis(AccelStepper::DRIVER, PIN_TMC_STEP, PIN_TMC_DIR);
static constexpr float kChassisStepRate =
    CHASSIS_RPM * (float)CHASSIS_STEPS_PER_REV / 60.0F;
static constexpr float kChassisAccelRate = CHASSIS_ACCEL_STEPS_PER_S2;
// 电机经 33:10 齿轮传动顶层：顶层转一整圈 = 电机 CHASSIS_GEAR_NUM/CHASSIS_GEAR_DEN 圈
static constexpr long kStepsPerTopRev = (long)(
    (double)CHASSIS_STEPS_PER_REV * (double)CHASSIS_GEAR_NUM /
        (double)CHASSIS_GEAR_DEN + 0.5);

// 参考 main.cpp：首次使能后等待驱动稳定，之后一直保持使能（锁轴），
// 超时/故障、以及"不锁轴的时段"（IDLE 空闲 / GAME_ACTIVE）都会 disableOutputs() 并把该标志复位。
static bool s_driverEnabled = false;
// 最后一次转动结束的时刻（IDLE 空闲断电判定用；见 power_manage_tick）
static uint32_t s_lastChassisMoveMs = 0;

// 中止请求：STOP/RESET 时置位，底盘运动循环与发牌任务都尽早退出。
// 代次（generation）消除"清除标志"的竞态：STOP/RESET 递增代次，发牌任务先采样代次、
// 只有代次没变才允许清标志——否则"新一局"会把启动窗口内刚到的 STOP/RESET 清掉，
// 这一局就继续转下去了（见 tasks_deal.cpp 的 vDealTask）。
static volatile bool     s_motionAbort = false;
static volatile uint32_t s_abortGen = 0;
static portMUX_TYPE      s_abortMux = portMUX_INITIALIZER_UNLOCKED;

void motion_abort_request(void) {
    portENTER_CRITICAL(&s_abortMux);
    s_abortGen++;
    s_motionAbort = true;
    portEXIT_CRITICAL(&s_abortMux);
}

bool motion_abort_requested(void) { return s_motionAbort; }

uint32_t motion_abort_generation(void) { return s_abortGen; }

bool motion_abort_clear_if_generation(uint32_t gen) {
    bool ok = false;
    portENTER_CRITICAL(&s_abortMux);
    if (s_abortGen == gen) { s_motionAbort = false; ok = true; }
    portEXIT_CRITICAL(&s_abortMux);
    return ok;
}

// ================= 中断：中断里只写缓冲 / 只累加计数 =================

// ---- 子板串口（Serial1）接收：中断写环形缓冲，子板通信任务读取 ----
// 为什么用中断：原来通信任务每 20ms 才轮询一次串口，子板上报的事件最多晚 20ms 才被解析；
// 发牌流程里每张牌有好几次"发命令 → 等事件"，这个延迟会累加成时序抖动。
// 中断接收后，字节立刻进缓冲，解析延迟只取决于任务节拍（SUB_COMM_POLL_MS）。
static volatile uint8_t  s_rx_ring[SUB_RX_RING_SIZE];
static volatile uint16_t s_rx_head = 0;   // 写入位置（中断里改）
static volatile uint16_t s_rx_tail = 0;   // 读取位置（任务里改）

static void IRAM_ATTR sub_uart_rx_isr(void) {
    while (Serial1.available() > 0) {
        uint8_t b = (uint8_t)Serial1.read();
        uint16_t next = (uint16_t)((s_rx_head + 1) % SUB_RX_RING_SIZE);
        if (next == s_rx_tail) continue;      // 缓冲满：丢弃（不完整帧会被 CRC 挡掉）
        s_rx_ring[s_rx_head] = b;
        s_rx_head = next;
    }
}

bool sub_uart_read_byte(uint8_t *b) {
    if (s_rx_tail == s_rx_head) return false;
    *b = s_rx_ring[s_rx_tail];
    s_rx_tail = (uint16_t)((s_rx_tail + 1) % SUB_RX_RING_SIZE);
    return true;
}

// ---- 旋转编码器 A/B 相：GPIO 中断 + 四态正交解码 ----
// 为什么改用中断：原来任务 1ms 轮询，快速旋转时两次跳变可能落在同一采样周期里 → 丢步；
// 中断能捕获每一个边沿，任务只消费累计格数（见 vEncoderTask）。
// 解码表：索引 = (上次 AB << 2) | 本次 AB，值 = 本次跳变方向
//   合法跳变（每次只有一相变化）→ ±1；两相同时变（抖动/噪声）→ 0，并把累计清零重来
static const int8_t kEncQuadDelta[16] = {
     0, +1, -1,  0,
    -1,  0,  0, +1,
    +1,  0,  0, -1,
     0, -1, +1,  0,
};
static volatile uint8_t  s_enc_state = 0;      // 上次 AB 组合（A<<1 | B）
static volatile int8_t   s_enc_quad = 0;       // 同方向累计跳变（满 ±4 = 一格）
static volatile int32_t  s_enc_total = 0;      // 已确认格数：中断里**单调**累加（+1/-1 每格）
static int32_t           s_enc_baseline = 0;   // 任务侧上次取走时的基准值（只有消费方读写）
static volatile int8_t   s_enc_last_dir = 0;   // 上次接受的跳变方向
static volatile uint32_t s_enc_last_us = 0;    // 上次接受的跳变时刻

static void IRAM_ATTR enc_ab_isr(void) {
    // 分两次读 A/B 之间电平可能被改变：A 读两次，变了就重读一次 B，取到一致的一对
    uint8_t a = (uint8_t)digitalRead(PIN_ENC_A);
    uint8_t b = (uint8_t)digitalRead(PIN_ENC_B);
    uint8_t a2 = (uint8_t)digitalRead(PIN_ENC_A);
    if (a2 != a) { a = a2; b = (uint8_t)digitalRead(PIN_ENC_B); }

    uint8_t cur = (uint8_t)((a << 1) | b);
    uint8_t prev = s_enc_state;
    if (cur == prev) return;                       // 电平没变（另一相的抖动）→ 忽略
    s_enc_state = cur;

    int8_t d = kEncQuadDelta[(uint8_t)((prev << 2) | cur)];
    if (d == 0) { s_enc_quad = 0; return; }        // 两相同时变 = 受扰，清零重来

    // 抖动过滤：同方向、间隔过短的跳变丢弃（真实转动最快约 1ms 一次跳变，不会误伤）
    uint32_t nowUs = micros();
    if (d == s_enc_last_dir &&
        (uint32_t)(nowUs - s_enc_last_us) < (uint32_t)ENCODER_ISR_GUARD_US) {
        return;
    }
    s_enc_last_dir = d;
    s_enc_last_us = nowUs;

    s_enc_quad = (int8_t)(s_enc_quad + d);
    if (s_enc_quad >= 4)       { s_enc_quad = 0; s_enc_total++; }
    else if (s_enc_quad <= -4) { s_enc_quad = 0; s_enc_total--; }
}

// 取走累计格数（同时把基准推到当前值）。
// 同步方式：**单写者（GPIO 中断）+ 单读者（编码器任务）**，32 位对齐读写本身是原子的；
// 用"单调计数 + 基准差值"就不存在"读-改-写"丢更新，也就不需要临界区。
// （原来用 portENTER_CRITICAL 保护读侧、ISR 却没进同一把锁，跨核时仍可能丢一格。）
int32_t encoder_take_steps(void) {
    int32_t now = s_enc_total;            // 原子读
    int32_t d = now - s_enc_baseline;     // 32 位环绕减法天然正确（增量远小于 2^31）
    s_enc_baseline = now;                 // 只动基准，不会丢掉中断里刚加的增量
    return d;
}

// 丢弃已累计的格数（开机 / 自检后调用）：只动任务侧基准 + 清半格累计
void encoder_reset_steps(void) {
    s_enc_baseline = s_enc_total;
    s_enc_quad = 0;
}

void init_hardware(void) {
    // 输出
    pinMode(PIN_TMC_STEP, OUTPUT);
    pinMode(PIN_TMC_DIR, OUTPUT);
    pinMode(PIN_TMC_ENN, OUTPUT);
    digitalWrite(PIN_TMC_ENN, HIGH);    // 默认驱动断电

    // SPI 屏（SCK/MOSI 与 SD 共用总线；SD 预留未接，不初始化）
    pinMode(PIN_SPI_SCK, OUTPUT);
    pinMode(PIN_SPI_MOSI, OUTPUT);
    pinMode(PIN_TFT_DC, OUTPUT);
    pinMode(PIN_TFT_RESET, OUTPUT);
    digitalWrite(PIN_TFT_RESET, HIGH);
    pinMode(PIN_TFT_CS, OUTPUT);
    digitalWrite(PIN_TFT_CS, HIGH);     // 片选默认拉高

    // 输入
    pinMode(PIN_ENC_A, INPUT_PULLUP);
    pinMode(PIN_ENC_B, INPUT_PULLUP);
    pinMode(PIN_ENC_SW, INPUT_PULLUP);

    // 子板串口：2 根线 UART（POS=TX → 子板 RX；NEG=RX ← 子板 TX，见 pins_config.h）
    Serial1.begin(SUB_UART_BAUD, SERIAL_8N1, SUB_UART_RX_PIN, SUB_UART_TX_PIN);
    Serial1.onReceive(sub_uart_rx_isr);   // 中断接收 → 环形缓冲（任务只做解析）

    // TODO: TFT_eSPI / SdFat 初始化（片选互斥，架构 v2 1.3）
}

void init_interrupts(void) {
    // 必须在 create_itc() 之后调用（本函数目前不依赖内核对象，保留该顺序约定）。
    // DIAG 引脚已随需求删除；底盘堵转保护暂由 chassis_run_relative 的软件超时兜底。

    // 旋转编码器 A/B 相：GPIO 中断 + 四态正交解码（取代原来的 1ms 轮询解码）
    // 两个引脚都要挂 CHANGE：只挂一相判不出方向。
    // 先读一次初始电平，避免第一次中断时 prev 是脏值、算出反向的一步。
    s_enc_state = (uint8_t)(((digitalRead(PIN_ENC_A) ? 1 : 0) << 1) |
                             (digitalRead(PIN_ENC_B) ? 1 : 0));
    s_enc_last_dir = 0;
    s_enc_last_us = 0;
    encoder_reset_steps();
    attachInterrupt(digitalPinToInterrupt(PIN_ENC_A), enc_ab_isr, CHANGE);
    attachInterrupt(digitalPinToInterrupt(PIN_ENC_B), enc_ab_isr, CHANGE);
    Serial.println("[ENC] A/B quadrature decode on GPIO interrupt (4 transitions = 1 step)");

    // SW 不挂中断：按键是慢事件，任务里按 ENCODER_POLL_MS 轮询 + ENCODER_DEBOUNCE_MS 消抖即可。
    // TODO: 霍尔接入后在这里挂 CHANGE 中断（一圈一个脉冲，用于归零 / 失步校准）。
}

void tmc2209_init(void) {
    // 底盘步进初始化（函数名沿用早期版本；内部已无 TMC UART）。
    // EN 反相（LOW=使能）；速度上限与加速度由 AppConfig 设定，
    // 运动由 run() 按“匀加速 → 匀速 → 匀减速”自动规划。
    g_chassis.setEnablePin(PIN_TMC_ENN);
    g_chassis.setPinsInverted(false, false, true);   // DIR/STEP 不反相；EN 反相：LOW=使能
    g_chassis.setMinPulseWidth(5);
    g_chassis.setMaxSpeed(kChassisStepRate);
    g_chassis.setAcceleration(kChassisAccelRate);
    g_chassis.disableOutputs();      // 上电默认断电（参考 setup 末尾）
    Serial.printf("[STEP] chassis AccelStepper ready: %.0f step/s, accel %.0f step/s^2, %ld step/rev\n",
                  kChassisStepRate, kChassisAccelRate, (long)CHASSIS_STEPS_PER_REV);
}

void hall_homing(void) {
    // 当前状态：霍尔传感器尚未接入，**以开机位置作为转盘零点**（不做归零动作）。
    // 因此 chassis_rotate_to_angle() 是在"开机时刻的位置"上按最短路径转相对角度；
    // 每次上电后需要人工把转盘摆到同一个起始位置，牌堆角度才与 kDeckAngles 对应。
    //
    // 后续接入霍尔后应实现两段式归零（交接指南要求）：
    //   ① 转动直至霍尔电平翻转（进入磁铁感应区）
    //   ② 反向微调至电平恢复（磁场边缘 = 机械零点）
    //   ③ 步进计数器清零；超时未检测到零点 → 锁死电机 + 屏幕报「零点传感器故障」
    // 调用方（main.cpp 的启动顺序）不需要改，届时把实现填进来即可。
}

// 首次使能：EN 拉低并等待 CHASSIS_SETTLE_MS 稳定（参考 enableDriver()）。
void chassis_enable_driver(void) {
    if (s_driverEnabled) return;
    g_chassis.enableOutputs();
    s_driverEnabled = true;
    vTaskDelay(pdMS_TO_TICKS(CHASSIS_SETTLE_MS));
    s_lastChassisMoveMs = millis();   // 空闲断电计时从"使能这一刻"起算
    Serial.println("[PWR] chassis driver ON (holding torque)");
}

// 断开底盘驱动：EN 拉高 → 绕组断电，不再吃保持电流（低功耗 A 级，见 app_config.h）。
// 不锁轴也没关系：转盘经齿轮/摩擦自锁，位置由机械保持；软件位置（g_chassis.currentPosition()）
// 不受断电影响，下次 chassis_at_angle()/chassis_rotate_to_angle() 照常按它算。
// ⚠️ 发牌进行中（DEALING）不要调用：那时需要绕组锁住角度（理由见 app_config.h）。
void chassis_power_release(void) {
    if (!s_driverEnabled) return;
    g_chassis.setSpeed(0.0F);
    g_chassis.disableOutputs();
    s_driverEnabled = false;
    Serial.println("[PWR] chassis driver OFF (standby, no holding torque)");
}

bool chassis_driver_enabled(void) {
    return s_driverEnabled;
}

// 相对移动 delta 步：调用方只需传步数（角度/圈数换算在上一层完成）。
//   运动由 AccelStepper run() 自动规划：匀加速 → 匀速(≤kChassisStepRate) → 匀减速，
//   到位时速度已降到 0，避免启停瞬间电流突变导致电源过载。
//   到位后保持 EN 使能锁轴（电流由驱动 VREF 电位器设定）。
// 超时按“预计用时 + 余量”计算，作为软件兜底（故障时断电并允许下次重新使能）。
// 所有发牌方案都经过本函数，策略统一。
static bool chassis_run_relative(long delta) {
    if (delta == 0) return true;   // 已在目标位置：保持当前使能状态

    chassis_enable_driver();
    long finalTarget = g_chassis.currentPosition() + delta;
    g_chassis.moveTo(finalTarget);          // run() 内部按加速度曲线逐步逼近

    long dist = (delta < 0) ? -delta : delta;
    double v = (double)kChassisStepRate;
    double a = (double)kChassisAccelRate;
    double rampSec = v / a;                 // 0→v（以及 v→0）各需的时间
    // 预计用时 = 加速 + 匀速 + 减速；距离短到没跑满速时，这个式子仍是安全上界
    double expectSec = (double)dist / v + 2.0 * rampSec;
    uint32_t expectMs = (uint32_t)(expectSec * 1000.0);
    // 超时 = 预计用时 × 系数 + 余量（下限兜底）：改转速/角度都不用重调常数
    uint32_t timeoutMs = (uint32_t)((float)expectMs * CHASSIS_TIMEOUT_FACTOR) +
                         (uint32_t)CHASSIS_TIMEOUT_MARGIN_MS;
    if (timeoutMs < (uint32_t)CHASSIS_TIMEOUT_MIN_MS) {
        timeoutMs = (uint32_t)CHASSIS_TIMEOUT_MIN_MS;
    }

    Serial.printf("[STEP] move %ld motor steps -> pos %ld (expect %ums, timeout %ums)\n",
                  delta, finalTarget, (unsigned)expectMs, (unsigned)timeoutMs);

    uint32_t t0 = millis();
    while (g_chassis.distanceToGo() != 0) {
        g_chassis.run();        // 高频调用：内部按加速度/减速度更新转速并产 STEP 脉冲
        if (s_motionAbort) {    // STOP/RESET：立即停脉冲（EN 先不动，回 IDLE 时由状态机断电）
            Serial.println("[STEP] aborted by stop request");
            g_chassis.setSpeed(0.0F);
            s_lastChassisMoveMs = millis();
            return false;
        }
        if ((uint32_t)(millis() - t0) > timeoutMs) {
            Serial.println("[STEP] rotate timeout");
            g_chassis.setSpeed(0.0F);
            g_chassis.disableOutputs();
            s_driverEnabled = false;
            s_lastChassisMoveMs = millis();
            return false;
        }
        taskYIELD();            // 只让出调度，不做 1ms 延时
    }
    // run() 到位即已完成减速；这里**保持 EN 有效**（发牌过程中要锁住转盘角度，
    // 否则推牌的反作用力会把转盘顶偏，后面每张牌就都落错堆了）。
    // 真正断电交给"不锁轴的时段"：回到 IDLE / 进入 GAME_ACTIVE 时由 power_manage_tick 统一处理。
    // 到位后的稳定等待**按转速算**：电机轴转一圈的时间 × 系数，再夹到上下限之间。
    // 转速越高，停下时的残余振动越小 → 等得越短；改 CHASSIS_RPM 后这里自动跟着变。
    uint32_t revMs    = (uint32_t)(60000.0 / (double)CHASSIS_RPM);   // 电机轴转一圈
    uint32_t settleMs = (uint32_t)((float)revMs * CHASSIS_SETTLE_K);
    if (settleMs < (uint32_t)CHASSIS_SETTLE_MIN_MS) settleMs = (uint32_t)CHASSIS_SETTLE_MIN_MS;
    if (settleMs > (uint32_t)CHASSIS_SETTLE_MAX_MS) settleMs = (uint32_t)CHASSIS_SETTLE_MAX_MS;
    Serial.printf("[STEP] done in %ums, settle %ums (rev %ums x %.2f), driver holding\n",
                  (unsigned)(millis() - t0), (unsigned)settleMs,
                  (unsigned)revMs, (double)CHASSIS_SETTLE_K);
    if (settleMs) vTaskDelay(pdMS_TO_TICKS(settleMs));
    s_lastChassisMoveMs = millis();   // IDLE 空闲断电计时的起点
    return true;
}

// 顶层角度 → 电机绝对步数（33:10 齿轮换算，与 kStepsPerTopRev 同一套算法）
static long chassis_angle_to_steps(int16_t angleDeg) {
    int16_t a = (int16_t)(angleDeg % 360);
    if (a < 0) a += 360;
    return (long)((double)a * (double)CHASSIS_STEPS_PER_REV * (double)CHASSIS_GEAR_NUM /
                      (360.0 * (double)CHASSIS_GEAR_DEN) + 0.5);
}

// 转盘当前是否已停在该角度上（按电机步数判断，容差 CHASSIS_AT_TOL_STEPS；角度按一圈取最短差）
bool chassis_at_angle(int16_t angleDeg) {
    long diff = chassis_angle_to_steps(angleDeg) - g_chassis.currentPosition();
    diff %= kStepsPerTopRev;
    if (diff < 0) diff += kStepsPerTopRev;
    if (diff > kStepsPerTopRev / 2) diff = kStepsPerTopRev - diff;
    return diff <= (long)CHASSIS_AT_TOL_STEPS;
}

// 顶层转盘转到指定角度（最短路径，0~359°；电机步数按 33:10 齿轮比换算）
bool chassis_rotate_to_angle(int16_t angleDeg) {
    int16_t a = (int16_t)(angleDeg % 360);
    if (a < 0) a += 360;

    long target = chassis_angle_to_steps(a);
    long cur = g_chassis.currentPosition();
    long delta = (target - cur) % kStepsPerTopRev;
    if (delta < 0) delta += kStepsPerTopRev;
    if (delta > kStepsPerTopRev / 2) delta -= kStepsPerTopRev;

    // 已经停在该角度：不调用运动函数、不打 [STEP] 日志（发牌任务会打印 "skip rotate"）
    if (delta == 0) return true;

    Serial.printf("[STEP] top %d deg (ratio %d:%d)\n",
                  a, CHASSIS_GEAR_NUM, CHASSIS_GEAR_DEN);
    return chassis_run_relative(delta);
}

// 顶层沿同一方向连续转 turns 圈（每圈 = kStepsPerTopRev 个电机步）
bool chassis_rotate_turns(int32_t turns) {
    long steps = (long)turns * kStepsPerTopRev;
    Serial.printf("[STEP] continuous rotate %ld top turns -> %ld motor steps\n",
                  (long)turns, steps);
    return chassis_run_relative(steps);
}

// ================= 外设自检（架构 v2 第一章 / 第八章；方案见 重要信息/自检流程方案.md）=================
// 只做**不需要人配合**的项目，分两类：
//   自动判定：引脚电平读回、BLE 广播、与子板串口握手（发 CMD_STATUS_QUERY 等应答）；
//   人眼/听声确认：屏幕色块、底盘微动 —— 这类没有数字反馈，只能看/听（位图置 1 = 动作已执行）。
// 交互项（转/按编码器、手转电机试锁轴力矩、拿磁铁试霍尔）不在开机自检里做。
static uint16_t s_selftestBits = 0;      // 最近一次自检结果位图（BOT_ST_*）

// 自检各分项（实现见本函数下方）
static bool selftest_chassis(void);
static bool selftest_sub_link(uint16_t *outBits);

void self_test(void) {
    s_selftestBits = 0;
    Serial.println("[ST] ======== self test ========");

    // 1) 屏幕：display_init() 已画过 红→绿→蓝→黑 色块（人眼确认颜色与顺序）
    Serial.println("[ST] tft      : color bars drawn -> 人眼确认 红/绿/蓝/黑 顺序正确");
    s_selftestBits |= BOT_ST_TFT;

    // 2) 编码器引脚：读一次空闲电平（自动项；旋转/按键本身要人配合，不在开机自检里做）
    int ea = digitalRead(PIN_ENC_A), eb = digitalRead(PIN_ENC_B), esw = digitalRead(PIN_ENC_SW);
    Serial.printf("[ST] encoder  : A=%d B=%d SW=%d（空闲应全为 1；SW=0 说明按键被按住或短路）\n",
                  ea, eb, esw);
    s_selftestBits |= BOT_ST_ENC;

    // 3) 霍尔：已删除该功能

    // 4) 蓝牙：ble_init() 已在 setup 里执行（广播已开），手机搜到即算通过
    Serial.printf("[ST] ble      : advertising as '%s'%s\n",
                  BLE_DEVICE_NAME, ble_connected() ? " (connected)" : "");
    s_selftestBits |= BOT_ST_BLE;

    // 5) SD 卡：未接线 → 跳过（不算失败）
    Serial.println("[ST] sd       : skipped（未接线）");

    // 6) 底盘步进微动（可观察项；净位移 0，不动转盘零点）
    if (selftest_chassis()) s_selftestBits |= BOT_ST_CHASSIS;
    else Serial.println("[ST] chassis  : twitch FAILED（查驱动供电 / EN / STEP 接线）");

    // 7) 与子板串口握手（自动判定）
    uint16_t subBits = 0;
    if (selftest_sub_link(&subBits)) s_selftestBits |= BOT_ST_SUBUART;

    s_selftestBits |= BOT_ST_DONE;
    Serial.printf("[ST] ======== done: bits=0x%04X ========\n", (unsigned)s_selftestBits);
    selftest_report();

    // 8) 屏幕汇总：显示 SELFTEST_SHOW_MS 后，由显示任务覆盖成 IDLE 屏
    display_show_selftest(s_selftestBits);
    delay(SELFTEST_SHOW_MS);
}

// ================= 自检辅助与结果输出（实现细节见文件头注释）=================
uint16_t selftest_bits(void) { return s_selftestBits; }

// 自检结果汇总打印；CLI `selftest` 也走这里（不动作、不阻塞）
void selftest_report(void) {
    uint16_t b = s_selftestBits;
    Serial.printf("[ST] ==== selftest bits=0x%04X ====\n", (unsigned)b);
    Serial.printf("[ST]   tft      : %s\n", (b & BOT_ST_TFT)     ? "drawn *" : "SKIP");
    Serial.printf("[ST]   chassis  : %s\n", (b & BOT_ST_CHASSIS) ? "twitch *" : "FAIL");
    Serial.printf("[ST]   hall     : %s\n", (b & BOT_ST_HALL)    ? "read" : "SKIP");
    Serial.printf("[ST]   encoder  : %s\n", (b & BOT_ST_ENC)     ? "read" : "SKIP");
    Serial.printf("[ST]   ble      : %s\n", (b & BOT_ST_BLE)     ? "on" : "SKIP");
    Serial.printf("[ST]   sub uart : %s\n", (b & BOT_ST_SUBUART) ? "OK" : "NO LINK");
    Serial.printf("[ST]   sd       : %s\n", (b & BOT_ST_SD)      ? "ok" : "skipped (未接线)");
    Serial.printf("[ST]   done     : %s\n", (b & BOT_ST_DONE)    ? "yes" : "no");
    Serial.println("[ST]   (* = 需人眼/听声确认)");

    // 现在就能读的实时状态（复检时不动作、不阻塞）
    Serial.printf("[ST] live: enc A/B/SW=%d/%d/%d ble=%s sub=%s\n",
                  digitalRead(PIN_ENC_A), digitalRead(PIN_ENC_B), digitalRead(PIN_ENC_SW),
                  ble_connected() ? "connected" : "advertising",
                  sub_comm_online() ? "ONLINE" : "OFFLINE");
    Serial.println("[ST] 交互项（转/按编码器、手转电机试锁轴力矩、拿磁铁试霍尔）需人工配合，未纳入开机自检");
}

// 底盘微动：顶层 ±SELFTEST_CHASSIS_DEG 度各一次，净位移 0。
// 净位移 0 的意义：既能让电机/驱动真的动一下（可观察），又不会把转盘挪走——
// hall_homing() 目前就是"以开机位置为零点"，转盘不能动。
static bool selftest_chassis(void) {
#if SELFTEST_CHASSIS_MOVE
    long steps = (long)((double)SELFTEST_CHASSIS_DEG * (double)CHASSIS_STEPS_PER_REV *
                        (double)CHASSIS_GEAR_NUM /
                        (360.0 * (double)CHASSIS_GEAR_DEN) + 0.5);
    if (steps < 1) steps = 1;
    Serial.printf("[ST] chassis: twitch +/- %.1f top-deg (%ld motor steps each way)\n",
                  (double)SELFTEST_CHASSIS_DEG, steps);
    chassis_enable_driver();
    // 去程 + 回程都到位才算通过（走不动说明驱动供电 / 接线 / EN 有问题）
    return chassis_run_relative(steps) && chassis_run_relative(-steps);
#else
    Serial.println("[ST] chassis: skipped (SELFTEST_CHASSIS_MOVE=0)");
    return true;
#endif
}

// ---- 与子板串口握手（自检用）----
// 此时子板通信任务还没创建，不能走 xSubboardTxQueue：直接写串口 + 轮询收帧。
static proto_rx_t       s_stRx;
static volatile bool    s_stGotFrame = false;
static volatile bool    s_stGotReady = false;
static volatile uint8_t s_stReadyBits[2] = { 0, 0 };

static void selftest_rx_cb(const proto_frame_t *f) {
    s_stGotFrame = true;
    if (f->type == EVT_READY) {
        s_stGotReady = true;
        if (f->len >= 2) { s_stReadyBits[0] = f->data[0]; s_stReadyBits[1] = f->data[1]; }
    }
    proto_note_sub_rx();   // 收到即视为“在线”，让 sub_comm_online() 立刻生效
}

// 打印子板自检位图（SUB_ST_*，定义见 protocol.h）
static void selftest_print_sub_bits(uint16_t b) {
    Serial.printf("[ST]   sub bits=0x%04X |%s%s%s%s%s%s\n", (unsigned)b,
                  (b & SUB_ST_MOTOR)     ? " motor"     : "",
                  (b & SUB_ST_CAM_CALIB) ? " cam-calib" : "",
                  (b & SUB_ST_CAM_UART)  ? " cam-uart"  : "",
                  (b & SUB_ST_PHOTO)     ? " photo"     : "",
                  (b & SUB_ST_HOST_UART) ? " host-uart" : "",
                  (b & SUB_ST_DONE)      ? " done"      : "");
    if (b & SUB_ST_CAM_CALIB) Serial.println("[ST]   ^ 摄像头已回应校准指令（校准已开始）");
    else                      Serial.println("[ST]   ^ 摄像头未回应校准（OpenMV 端可能还没实现 UART 命令接收）");
}

// 与子板握手：发 CMD_STATUS_QUERY，等子板回帧（优先等带位图的 EVT_READY）。
// 返回 true = 收到了至少一帧（链路通）。
static bool selftest_sub_link(uint16_t *outBits) {
    proto_rx_init(&s_stRx, selftest_rx_cb);
    s_stGotFrame = false;
    s_stGotReady = false;
    s_stReadyBits[0] = s_stReadyBits[1] = 0;

    // 串口接收已改由中断写环形缓冲，这里从环形缓冲取字节（直接读 Serial1 会读到空）
    uint8_t drop;
    while (sub_uart_read_byte(&drop)) { }                  // 丢掉开机瞬间的残留

    proto_frame_t ping = {};
    ping.type = CMD_STATUS_QUERY;
    ping.len = 0;
    proto_write_frame(&ping);

    uint32_t t0 = millis();
    uint32_t lastPing = t0;
    uint32_t firstFrameMs = 0;
    while ((uint32_t)(millis() - t0) < (uint32_t)SELFTEST_SUB_WAIT_MS) {
        uint8_t b;
        while (sub_uart_read_byte(&b)) proto_rx_feed(&s_stRx, b);

        if (s_stGotReady) break;                     // 拿到 EVT_READY：最好情况，直接收工
        if (s_stGotFrame && firstFrameMs == 0) firstFrameMs = millis();
        // 链路已通（收到 ACK/STATUS）：再等一小会儿看有没有 EVT_READY 就收工
        if (firstFrameMs && (uint32_t)(millis() - firstFrameMs) >=
                                (uint32_t)SELFTEST_SUB_READY_GRACE_MS) break;

        if ((uint32_t)(millis() - lastPing) >= (uint32_t)SELFTEST_SUB_PING_MS) {
            lastPing = millis();
            proto_write_frame(&ping);                // 子板可能刚启动，重发
        }
        delay(5);
    }

    uint16_t bits = (uint16_t)(s_stReadyBits[0] | ((uint16_t)s_stReadyBits[1] << 8));
    if (outBits) *outBits = bits;

    if (!s_stGotFrame) {
        Serial.printf("[ST] sub link  : FAIL - %u ms 内没收到任何帧"
                      "（查共地 / 接线 / 子板供电；单独测底板可敲 subsim on）\n",
                      (unsigned)SELFTEST_SUB_WAIT_MS);
        return false;
    }
    Serial.printf("[ST] sub link  : OK (%u ms)%s\n", (unsigned)(millis() - t0),
                  s_stGotReady ? ", EVT_READY received" : ", no EVT_READY (可能已错过)");
    if (s_stGotReady) selftest_print_sub_bits(bits);
    return true;
}

// ================= 任务内占位函数（TODO：按架构实现）=================
void busy_subboard_event(uint8_t type, const uint8_t *data, uint8_t len) {
    // TODO: 按 EVT_*/ERROR_* 更新牌堆、进度、屏幕等
    (void)type; (void)data; (void)len;
}

void busy_deal_step(const char *step) {
    // TODO: 旋转到目标牌堆 / 下发发牌指令 / 等待事件 / 更新牌堆数据
    (void)step;
}

// ================= 低功耗（A 级）=================
// 两件事：底盘驱动断电（不锁轴的时段）+ CPU 按状态降频。设计依据与时序论证见 app_config.h。
//
// CPU 降频为什么不会造成时序错误（经典 ESP32）：
//   80MHz 与 240MHz 下 APB 都是 80MHz（Arduino 核心 calculateApb()：freq >= 80 → 80MHz），
//   所以 UART 115200、屏幕 SPI 10MHz、AccelStepper 的 setMinPulseWidth 等时基都不变；
//   millis()/micros()/delay() 走 systimer + FreeRTOS tick，与 CPU 频率无关。
//   差别只是"代码跑多快"，而本工程所有对外时序都是 ms 级、由 millis() 计量。
static uint32_t s_cpuMhz = 0;   // 已设置的主频（0 = 还没设过；以芯片实际返回值为准）

void power_set_cpu(uint32_t mhz) {
    if (mhz == 0 || mhz == s_cpuMhz) return;
    if (!setCpuFrequencyMhz(mhz)) {
        Serial.printf("[PWR] cpu -> %u MHz FAILED (keep %u MHz)\n",
                      (unsigned)mhz, (unsigned)getCpuFrequencyMhz());
        return;
    }
    s_cpuMhz = getCpuFrequencyMhz();
    Serial.printf("[PWR] cpu -> %u MHz (apb %u MHz, unchanged)\n",
                  (unsigned)s_cpuMhz, (unsigned)(getApbFrequency() / 1000000UL));
}

// 低功耗巡检：由监控任务经 busy_monitor() 每 MONITOR_PERIOD_MS 调一次。
void power_manage_tick(void) {
    system_state_t st = state_get_current();

#if CHASSIS_POWER_DOWN_IDLE
    // IDLE 且已经"闲"了一会儿：断开底盘驱动（不锁轴）
    if (st == STATE_IDLE && s_driverEnabled &&
        (uint32_t)(millis() - s_lastChassisMoveMs) >= (uint32_t)CHASSIS_IDLE_POWER_DOWN_MS) {
        chassis_power_release();
    }
#endif

#if BOT_CPU_SCALE_ENABLE
    // 需要性能档：不在 IDLE（发牌中 / 一局已发完等交互）、或蓝牙已连接（要保证响应速度）
    const bool needPerf = (st != STATE_IDLE) || ble_connected();
    power_set_cpu(needPerf ? BOT_CPU_PERF_MHZ : BOT_CPU_IDLE_MHZ);
#endif
}

void busy_state_enter(uint8_t state) {
    // 状态进入动作。⚠️ 本函数在状态互斥量内被调用，只能做"不阻塞"的事（禁止 vTaskDelay）。
    switch ((system_state_t)state) {
    case STATE_IDLE:
#if CHASSIS_POWER_DOWN_IDLE
        // 回 IDLE：本局已结束/被取消，转盘不需要保持位置 → 直接断驱动（不锁轴）
        chassis_power_release();
#endif
        break;
    case STATE_GAME_ACTIVE:
#if CHASSIS_POWER_DOWN_GAME
        // 一局已发完，等用户长按结束 → 同样不需要保持位置
        chassis_power_release();
#endif
        break;
    default:
        break;   // DEALING：保持使能锁轴（推牌反作用力会顶偏转盘，见 app_config.h）
    }
}

void busy_state_exit(uint8_t state) {
    // TODO: 状态退出动作（例如 GAME_ACTIVE→IDLE：清空牌局数据、复位所有状态）
    (void)state;
}

void busy_monitor(void) {
    power_manage_tick();   // 低功耗巡检：底盘驱动断电 + CPU 降频/升频
    // TODO: 电机到位/超时状态巡检、子板心跳、告警上报
}
