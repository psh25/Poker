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
// 只有超时/故障才 disableOutputs() 并把该标志复位。
static bool s_driverEnabled = false;

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
    pinMode(PIN_HALL, INPUT_PULLUP);    // 强制上拉（交接指南要求）
    pinMode(PIN_ENC_A, INPUT_PULLUP);
    pinMode(PIN_ENC_B, INPUT_PULLUP);
    pinMode(PIN_ENC_SW, INPUT_PULLUP);

    // 子板串口：2 根线 UART（POS=TX → 子板 RX；NEG=RX ← 子板 TX，见 pins_config.h）
    Serial1.begin(SUB_UART_BAUD, SERIAL_8N1, SUB_UART_RX_PIN, SUB_UART_TX_PIN);

    // TODO: TFT_eSPI / SdFat 初始化（片选互斥，架构 v2 1.3）
}

void init_interrupts(void) {
    // 必须在 create_itc() 之后调用，确保内核对象已创建。
    // DIAG 引脚已随需求删除；底盘堵转保护暂由 chassis_rotate_to_angle 的软件超时兜底。
    // TODO: 最终原理图若恢复硬件堵转检测引脚，再在此挂接中断。
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
    // 两段式归零（交接指南强制）：
    //   ① 高速旋转直至 IO1 低电平（进入磁铁感应区）
    //   ② 降速反向旋转直至 IO1 恢复高电平（磁场边缘 = 机械零点）
    //   ③ 步进计数器清零
    // 超时保护：限制最大一整圈步数，超时未检测到零点 → 锁死电机 + 屏幕上报
    //   「零点传感器故障」。
    // TODO: 用 AccelStepper 的恒速转动 + 霍尔沿检测实现。
}

// 首次使能：EN 拉低并等待 CHASSIS_SETTLE_MS 稳定（参考 enableDriver()）。
static void chassis_enable_driver(void) {
    if (s_driverEnabled) return;
    g_chassis.enableOutputs();
    s_driverEnabled = true;
    vTaskDelay(pdMS_TO_TICKS(CHASSIS_SETTLE_MS));
    Serial.println("[STEP] driver enabled");
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
    Serial.printf("[STEP] move %ld motor steps -> pos %ld\n", delta, finalTarget);

    long dist = (delta < 0) ? -delta : delta;
    double v = (double)kChassisStepRate;
    double a = (double)kChassisAccelRate;
    double rampSec = v / a;                 // 0→v 所需时间（作为用时上界的一部分）
    uint32_t expectMs = CHASSIS_SETTLE_MS + 5000U +
                        (uint32_t)(((double)dist / v + rampSec) * 1000.0);
    uint32_t timeoutMs = (expectMs > (uint32_t)CHASSIS_MOVE_TIMEOUT_MS)
                             ? expectMs
                             : (uint32_t)CHASSIS_MOVE_TIMEOUT_MS;

    uint32_t t0 = millis();
    while (g_chassis.distanceToGo() != 0) {
        g_chassis.run();        // 高频调用：内部按加速度/减速度更新转速并产 STEP 脉冲
        if ((uint32_t)(millis() - t0) > timeoutMs) {
            Serial.println("[STEP] rotate timeout");
            g_chassis.setSpeed(0.0F);
            g_chassis.disableOutputs();
            s_driverEnabled = false;
            return false;
        }
        taskYIELD();            // 只让出调度，不做 1ms 延时
    }
    // run() 到位即已完成减速；保持 EN 有效，锁轴等待下一条指令
    Serial.println("[STEP] done: ramped stop, driver remains enabled and holding");
    return true;
}

// 顶层转盘转到指定角度（最短路径，0~359°；电机步数按 33:10 齿轮比换算）
bool chassis_rotate_to_angle(int16_t angleDeg) {
    int16_t a = (int16_t)(angleDeg % 360);
    if (a < 0) a += 360;

    // 电机目标步数 = a/360 * 电机步数/圈 * 齿轮比（33:10）
    long target = (long)(
        (double)a * (double)CHASSIS_STEPS_PER_REV * (double)CHASSIS_GEAR_NUM /
            (360.0 * (double)CHASSIS_GEAR_DEN) + 0.5);
    long cur = g_chassis.currentPosition();
    long delta = (target - cur) % kStepsPerTopRev;
    if (delta < 0) delta += kStepsPerTopRev;
    if (delta > kStepsPerTopRev / 2) delta -= kStepsPerTopRev;

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

void self_test(void) {
    // 外设自检：霍尔电平、编码器采样、SD 卡挂载、电机驱动（EN/STEP）、滑环串口回环。
    // 任一失败 → 屏幕显示错误码，等待处理。
    // TODO: 实现各外设检测函数。
    Serial.println("[SELFTEST] TODO: 霍尔/编码器/SD/电机驱动/滑环自检");
}

// ================= 任务内占位函数（TODO：按架构实现）=================
void busy_bluetooth(void) {
    // TODO: BLE 初始化、小程序连接、选牌/查询指令解析、状态上报
}

void busy_subboard_event(uint8_t type, const uint8_t *data, uint8_t len) {
    // TODO: 按 EVT_*/ERROR_* 更新牌堆、进度、屏幕等
    (void)type; (void)data; (void)len;
}

void busy_deal_step(const char *step) {
    // TODO: 旋转到目标牌堆 / 下发发牌指令 / 等待事件 / 更新牌堆数据
    (void)step;
}

void busy_encoder(uint8_t kind) {
    // TODO: 方案索引切换、确认、重置
    (void)kind;
}

void busy_display(uint8_t cmdType) {
    // TODO: 按 DISPLAY_CMD_* 调用 TFT_eSPI 绘制（架构 v2 附录 A）
    (void)cmdType;
}

void busy_state_enter(uint8_t state) {
    // TODO: 状态进入动作（例如 DEALING：清空计数、DEALING→GAME_ACTIVE：上传小程序）
    (void)state;
}

void busy_state_exit(uint8_t state) {
    // TODO: 状态退出动作（例如 GAME_ACTIVE→IDLE：清空牌局数据、复位所有状态）
    (void)state;
}

void busy_monitor(void) {
    // TODO: 电机到位/超时状态巡检、子板心跳、告警上报
}
