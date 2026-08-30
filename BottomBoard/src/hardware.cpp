/**
 * 硬件初始化与占位函数（架构 v2 第一章 / 第八章）
 * 具体驱动逻辑（TMC 寄存器、SPI 屏幕、步进控制等）后续按交接指南实现。
 */
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "pins_config.h"
#include "app_config.h"
#include "itc.h"
#include "hardware.h"

static volatile bool g_tmc_stall = false;
bool tmc_stall_flag(void);
bool tmc_stall_flag(void) { return g_tmc_stall; }

// ---- TMC DIAG 堵转中断：紧急断电（架构 v2 8.2）----
static void IRAM_ATTR diag_isr(void) {
    g_tmc_stall = true;
    // TODO: 生产代码建议用 GPIO 寄存器直写，避免 digitalWrite 非 IRAM 安全
    digitalWrite(PIN_TMC_ENN, HIGH);   // 拉高 ENN = 驱动断电
    digitalWrite(PIN_LED, HIGH);       // 故障指示
}

void init_hardware(void) {
    // 输出
    pinMode(PIN_TMC_STEP, OUTPUT);
    pinMode(PIN_TMC_DIR, OUTPUT);
    pinMode(PIN_TMC_ENN, OUTPUT);
    digitalWrite(PIN_TMC_ENN, HIGH);    // 默认驱动断电
    pinMode(PIN_TMC_STDBY, OUTPUT);
    digitalWrite(PIN_TMC_STDBY, HIGH);  // 默认休眠，动作前拉低唤醒
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
    pinMode(PIN_TMC_DIAG, INPUT_PULLUP);
    pinMode(PIN_TMC_INDEX, INPUT_PULLUP);
    pinMode(PIN_TMC_RX, INPUT_PULLUP);

    // 子板串口：2 根线 UART（IO42 TX / IO41 RX）
    Serial1.begin(SUB_UART_BAUD, SERIAL_8N1, SUB_UART_RX_PIN, SUB_UART_TX_PIN);

    // TODO: TFT_eSPI / SdFat 初始化（片选互斥，架构 v2 1.3）
}

void init_interrupts(void) {
    // 必须在 create_itc() 之后调用，确保 xEncoderQueue 已创建
    attachInterrupt(digitalPinToInterrupt(PIN_TMC_DIAG), diag_isr, CHANGE); // TODO: 按实际电平极性配置
}

void tmc2209_init(void) {
    // TODO: 单线 UART 初始化：
    //   - GCONF.I_scale_analog = 0（关闭悬空 VREF，切内部 5V 基准）
    //   - 配置 StallGuard 阈值；电流上限 ≤ 1.4A RMS（采样电阻 0.1Ω）
    //   - 唤醒：拉低 DRIVER_STDBY
    // 上电前几十毫秒扭矩偏小属正常（交接指南）。
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

void self_test(void) {
    // 外设自检：霍尔电平、编码器采样、SD 卡挂载、TMC 寄存器读回、滑环串口回环。
    // 任一失败 → 屏幕显示错误码，等待处理。
    // TODO: 实现各外设检测函数。
    Serial.println("[SELFTEST] TODO: 霍尔/编码器/SD/TMC/滑环自检");
}

void led_set(bool on) {
    digitalWrite(PIN_LED, on ? HIGH : LOW);
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
    // TODO: TMC 温度 / SG_RESULT 巡检、子板心跳、LED 状态、INDEX 失步校准
}
