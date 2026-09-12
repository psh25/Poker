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

// 中止请求：STOP/RESET 时置位，底盘运动循环与发牌任务都尽早退出
static volatile bool s_motionAbort = false;

void motion_abort_request(void) { s_motionAbort = true; }
void motion_abort_clear(void)   { s_motionAbort = false; }
bool motion_abort_requested(void) { return s_motionAbort; }

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
        if (s_motionAbort) {    // STOP/RESET：立即停脉冲，保持 EN 锁轴
            Serial.println("[STEP] aborted by stop request");
            g_chassis.setSpeed(0.0F);
            return false;
        }
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
    // 到位后再等机械停稳（避免转盘还在振动就发牌导致偏位）
    if (CHASSIS_MOVE_SETTLE_MS > 0) vTaskDelay(pdMS_TO_TICKS(CHASSIS_MOVE_SETTLE_MS));
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

    // 3) 霍尔：单次读数只能说明引脚可读，判定极性要拿磁铁靠近（人工项）
    int hall = digitalRead(PIN_HALL);
    Serial.printf("[ST] hall     : %d (%s) —— 未进磁场时应为 1；极性需磁铁靠近再看\n",
                  hall, hall ? "HIGH" : "LOW");
    s_selftestBits |= BOT_ST_HALL;

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
    Serial.printf("[ST] live: hall=%d enc A/B/SW=%d/%d/%d ble=%s sub=%s\n",
                  digitalRead(PIN_HALL),
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

    while (Serial1.available() > 0) (void)Serial1.read();   // 丢掉开机瞬间的残留

    proto_frame_t ping = {};
    ping.type = CMD_STATUS_QUERY;
    ping.len = 0;
    proto_write_frame(&ping);

    uint32_t t0 = millis();
    uint32_t lastPing = t0;
    uint32_t firstFrameMs = 0;
    while ((uint32_t)(millis() - t0) < (uint32_t)SELFTEST_SUB_WAIT_MS) {
        while (Serial1.available() > 0) proto_rx_feed(&s_stRx, (uint8_t)Serial1.read());

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
        Serial.printf("[ST] sub link  : FAIL - %u ms 内没收到任何帧（查共地 / 接线 / 子板供电）\n",
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
