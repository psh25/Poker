/**
 * 硬件初始化与占位函数（架构 v2 第一章 / 第八章）
 * 步进转动由 AccelStepper 以 STEP/DIR/ENN 驱动（参考 重要信息/步进电机/main.cpp）；
 * TMC 寄存器、SPI 屏幕等后续按交接指南实现。
 */
#include <Arduino.h>
#include <AccelStepper.h>
#include <TMCStepper.h>
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
// 电机经 33:10 齿轮传动顶层：顶层转一整圈 = 电机 CHASSIS_GEAR_NUM/CHASSIS_GEAR_DEN 圈
static constexpr long kStepsPerTopRev = (long)(
    (double)CHASSIS_STEPS_PER_REV * (double)CHASSIS_GEAR_NUM /
        (double)CHASSIS_GEAR_DEN + 0.5);

// ---- TMC2209 单线 UART（UART2：IO18 RX / IO17 TX，MKS TMC2209）----
static HardwareSerial s_tmcSerial(2);
static TMC2209Stepper s_tmc(&s_tmcSerial, TMC_RSENSE, TMC_DRIVER_ADDR);
static bool s_tmcUartOk = false;    // UART 配置成功时才能“低电流锁轴”；失败回退“停转断电”

void init_hardware(void) {
    // 输出
    pinMode(PIN_TMC_STEP, OUTPUT);
    pinMode(PIN_TMC_DIR, OUTPUT);
    pinMode(PIN_TMC_ENN, OUTPUT);
    digitalWrite(PIN_TMC_ENN, HIGH);    // 默认驱动断电
    pinMode(PIN_LED, OUTPUT);
    digitalWrite(PIN_LED, LOW);

    // SPI 屏 + SD（共用 SCK/MOSI，独立片选）
    pinMode(PIN_SPI_SCK, OUTPUT);
    pinMode(PIN_SPI_MOSI, OUTPUT);
    pinMode(PIN_TFT_DC, OUTPUT);
    pinMode(PIN_TFT_RESET, OUTPUT);
    digitalWrite(PIN_TFT_RESET, HIGH);
    pinMode(PIN_TFT_CS, OUTPUT);
    digitalWrite(PIN_TFT_CS, HIGH);     // 片选默认拉高
    pinMode(PIN_SD_CS, OUTPUT);
    digitalWrite(PIN_SD_CS, HIGH);
    pinMode(PIN_SD_MISO, INPUT_PULLUP);

    // 输入
    pinMode(PIN_HALL, INPUT_PULLUP);    // 强制上拉（交接指南要求）
    pinMode(PIN_ENC_A, INPUT_PULLUP);
    pinMode(PIN_ENC_B, INPUT_PULLUP);
    pinMode(PIN_ENC_SW, INPUT_PULLUP);
    pinMode(PIN_TMC_RX, INPUT_PULLUP);

    // 子板串口：2 根线 UART（IO42 TX / IO41 RX）
    Serial1.begin(SUB_UART_BAUD, SERIAL_8N1, SUB_UART_RX_PIN, SUB_UART_TX_PIN);

    // TODO: TFT_eSPI / SdFat 初始化（片选互斥，架构 v2 1.3）
}

void init_interrupts(void) {
    // 必须在 create_itc() 之后调用，确保内核对象已创建。
    // DIAG 引脚已随需求删除；底盘堵转保护暂由 chassis_rotate_to_angle 的软件超时兜底。
    // TODO: 最终原理图若恢复硬件堵转检测引脚，再在此挂接中断。
}

void tmc2209_init(void) {
    // 底盘转动采用 AccelStepper（STEP/DIR/ENN），参考 重要信息/步进电机/main.cpp；
    // 转速修复：setSpeed 恒定转速 + runSpeed 连续产脉冲，不加加速度曲线。
    g_chassis.setEnablePin(PIN_TMC_ENN);
    g_chassis.setPinsInverted(false, false, true);   // DIR/STEP 不反相；EN 反相：LOW=使能
    g_chassis.setMinPulseWidth(5);
    g_chassis.setMaxSpeed(kChassisStepRate);
    g_chassis.setSpeed(0.0F);        // 与参考 main.cpp 一致：恒定转速，不加加速度曲线
    g_chassis.disableOutputs();
    Serial.printf("[STEP] TMC2209 AccelStepper ready: %.0f step/s, %ld step/rev\n",
                  kChassisStepRate, (long)CHASSIS_STEPS_PER_REV);

    // ---- TMC2209 UART：转动用 IRUN，停转自动降到 IHOLD 低电流锁轴 ----
    s_tmcSerial.begin(TMC_UART_BAUD, SERIAL_8N1, PIN_TMC_RX, PIN_TMC_TX);
    delay(50);
    s_tmc.begin();                    // 打开 PDN_UART 寄存器访问
    s_tmc.mstep_reg_select(false);    // 微步继续由 MS1/MS2 引脚决定（1/8），UART 不覆盖
    s_tmc.toff(5);                    // 使 chopper 工作（与之前可正常驱动的配置一致）
    s_tmc.en_spreadCycle(true);       // 明确用 spreadCycle，避免 StealthChop 待机电流异常
    s_tmc.freewheel(0);               // 停转后正常电流控制，不禁用/滑行
    s_tmc.irun(TMC_RUN_CS);           // 运行电流档
    s_tmc.ihold(TMC_HOLD_CS);         // 停转保持电流档（锁轴且省电）
    s_tmc.iholddelay(TMC_IHOLD_DELAY);
    s_tmc.I_scale_analog(false);      // 关闭悬空 VREF，使用内部基准
    delay(30);

    s_tmcUartOk = (s_tmc.test_connection() == 0);
    if (s_tmcUartOk) {
        Serial.printf("[TMC] UART OK: IRUN=%u IHOLD=%u delay=%u\n",
                      (unsigned)TMC_RUN_CS, (unsigned)TMC_HOLD_CS,
                      (unsigned)TMC_IHOLD_DELAY);
    } else {
        Serial.println("[TMC] UART FAIL: 无法配置低电流保持，停转后将直接断电（防过载）");
    }
    g_chassis.disableOutputs();
}

void hall_homing(void) {
    // 两段式归零（交接指南强制）：
    //   ① 高速旋转直至 IO1 低电平（进入磁铁感应区）
    //   ② 降速反向旋转直至 IO1 恢复高电平（磁场边缘 = 机械零点）
    //   ③ 步进计数器清零
    // 超时保护：限制最大一整圈步数，超时未检测到零点 → 锁死电机 + 屏幕上报
    //   「零点传感器故障」。
    // TODO: 调用 TMC 步进控制实现。
}

// 相对移动 delta 步（参考 main.cpp：setSpeed 恒定转速 + runSpeed 连续产脉冲）；
// 不按 1ms 延时轮询，避免高转速时脉冲频率被拉低；超时按“预计用时 + 余量”计算。
// 统一电源策略：UART 配置成功时到位后保持 EN、由 IHOLD 低电流锁轴；
// UART 配置失败时回退为“停转即 disableOutputs()”，避免保持电流导致电源过载。
// 该策略对所有方案一致生效（所有旋转都经过本函数）。
static bool chassis_run_relative(long delta) {
    if (delta == 0) {
        g_chassis.disableOutputs();
        return true;
    }

    g_chassis.enableOutputs();
    vTaskDelay(pdMS_TO_TICKS(CHASSIS_SETTLE_MS));   // 使能后短暂稳定

    g_chassis.setSpeed(delta > 0 ? kChassisStepRate : -kChassisStepRate);
    long finalTarget = g_chassis.currentPosition() + delta;
    Serial.printf("[STEP] move %ld motor steps -> pos %ld\n", delta, finalTarget);

    long dist = (delta < 0) ? -delta : delta;
    uint32_t expectMs = CHASSIS_SETTLE_MS + 5000U +
                        (uint32_t)((double)dist * 1000.0 / (double)kChassisStepRate);
    uint32_t timeoutMs = (expectMs > (uint32_t)CHASSIS_MOVE_TIMEOUT_MS)
                             ? expectMs
                             : (uint32_t)CHASSIS_MOVE_TIMEOUT_MS;

    uint32_t t0 = millis();
    while (g_chassis.currentPosition() != finalTarget) {
        g_chassis.runSpeed();   // 返回 true 表示本周期发出一个 STEP 脉冲
        if ((uint32_t)(millis() - t0) > timeoutMs) {
            Serial.println("[STEP] rotate timeout");
            g_chassis.setSpeed(0.0F);
            g_chassis.disableOutputs();
            return false;
        }
        taskYIELD();            // 只让出调度，不做 1ms 延时
    }
    g_chassis.setSpeed(0.0F);   // 停发脉冲
    if (s_tmcUartOk) {
        // UART 已配置：保持 EN 使能，驱动自动把电流降到 IHOLD，实现低电流锁轴
        Serial.println("[STEP] holding (low current IHOLD)");
    } else {
        // UART 配置失败回退：停转即断电，避免保持电流导致电源过载
        g_chassis.disableOutputs();
        Serial.println("[STEP] driver released (UART fallback)");
    }
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
    // 外设自检：霍尔电平、编码器采样、SD 卡挂载、TMC 寄存器读回、滑环串口回环。
    // 任一失败 → 屏幕显示错误码，等待处理。
    // TODO: 实现各外设检测函数。
    Serial.println("[SELFTEST] TODO: 霍尔/编码器/SD/TMC/滑环自检");
}

void led_set(bool on) {
    digitalWrite(PIN_LED, on ? HIGH : LOW);
}

bool tmc_uart_ready(void) {
    return s_tmcUartOk;
}

bool tmc_set_hold_current(uint8_t cs) {
    if (!s_tmcUartOk || cs > 31) return false;
    s_tmc.ihold(cs);
    return true;
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
    // TODO: 状态退出动作（例如 GAME_END：清空牌堆数据、复位所有状态）
    (void)state;
}

void busy_monitor(void) {
    // TODO: TMC 温度 / SG_RESULT 巡检（如启用 UART）、子板心跳、LED 状态
}
