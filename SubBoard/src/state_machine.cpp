/**
 * 子板裸机状态机（docs/subboard_architecture.md 第三章）
 *
 * 单张牌的动作序列（两种模式统一，**固定时长**，不再由光电门决定何时换相）：
 *   MOTOR_ON 正转 MOTOR_FWD_MS → BRAKE → REVERSE → PAUSE → 判定 → SEND_BACK
 *
 * 光电门在这里只当"记录器"（USE_PHOTO_SENSOR=1）：
 *   动作期间（正转/刹车/反转）只采样记录"是否出现过有牌"，**不做任何判断**；
 *   动作结束、电机停稳后（PAUSE 末尾）再根据"记录 + 当前电平"判定这一张：
 *     成功发出   = 过程中出现过“有牌”，结束时已恢复“无牌”
 *     卡在出牌口 = 结束时仍是“有牌”
 *     根本没出去 = 整个过程都没出现过“有牌”（漏发 / 卡在牌源里）
 *   失败 → RETRACT 反转撤回 → RETRACT_WAIT 等门恢复；
 *          恢复成功 → 自动重试这一张（最多 PHOTO_JAM_RETRY_MAX 次）
 *          撤回失败 / 重试超限 → ERROR（停机上报，等底板复位指令）
 *
 * USE_PHOTO_SENSOR=0（没有光电门时的调试路径）：跳过判定，一律按"成功发出"处理。
 *
 * 摄像头：由底板 CMD_CAM_CAPTURE 驱动，见 hardware.cpp 的 sub_camera_service()
 */
#include <Arduino.h>
#include <string.h>

#include "app_config.h"
#include "protocol.h"
#include "state_machine.h"
#include "hardware.h"
#include "debug.h"

static sub_state_t g_state = SUB_STATE_IDLE;
static uint32_t g_state_start_ms = 0;
static uint8_t g_error = 0;
static uint32_t s_deal_count = 0;   // 出牌计数（调试日志用）

// 说明：牌面数据不再经过子板缓存——摄像头回传由 sub_camera_service() 直接翻译成
// EVT_CARD_VALUE 上报（见 hardware.cpp），这里不再保留单卡缓冲。
static uint8_t s_jam_count = 0;     // 本张牌已重试次数（撤回成功后重试，超出则报错）

// ================= 光电门记录 + 发牌结果判定 =================
// 动作期间只记录，不做任何判断；判定统一放到动作结束（电机停稳）之后。
typedef enum {
    CARD_RESULT_OUT = 0,   // 成功发出：过程中出现过“有牌”，结束时已“无牌”
    CARD_RESULT_MISSED,    // 根本没出去：整个过程都没出现过“有牌”
    CARD_RESULT_STUCK,     // 卡在出牌口：结束时仍是“有牌”
} card_result_t;

static bool s_pt_seen_present = false;   // 动作期间是否出现过“有牌”（去抖后）

// 开始一张牌的动作：清空记录
static void photo_trace_reset(void) {
    s_pt_seen_present = false;
}

// 动作期间每次调用：只记录，不判断
static void photo_trace_sample(void) {
    if (sub_photo_present()) s_pt_seen_present = true;
}

// 动作结束（电机已停稳）后判定这一张
static card_result_t photo_trace_eval(void) {
    if (sub_photo_present()) return CARD_RESULT_STUCK;    // 门口还压着牌 → 卡住
    if (!s_pt_seen_present)  return CARD_RESULT_MISSED;   // 全程没见过牌 → 没出去
    return CARD_RESULT_OUT;                               // 见过、现在没了 → 成功
}

static const char *card_result_name(card_result_t r) {
    switch (r) {
    case CARD_RESULT_OUT:    return "OUT";
    case CARD_RESULT_MISSED: return "MISSED (never reached gate)";
    default:                 return "STUCK (card still at gate)";
    }
}

// ---- 自动连续发牌（调试用）----
static bool  s_auto_active  = false;
static long  s_auto_left    = 0;     // >0 剩余张数；-1 无限
static bool  s_auto_pending = false; // 上电延时等待中
static uint32_t s_auto_start_ms = 0;

sub_state_t sub_state_get(void) {
    return g_state;
}

// 进入“正转出牌”（不检查当前状态，供卡牌重试等内部路径使用）
static void deal_begin_forward(void) {
    photo_trace_reset();              // 新一张牌：清空光电门记录
    busy_motor_start();               // 正转出牌
    g_state = SUB_STATE_MOTOR_ON;
    g_state_start_ms = millis();
}

// 启动一张牌：从 IDLE（手动/自动首次）或 SEND_BACK（自动下一张）进入 MOTOR_ON
static void deal_start_now(void) {
    if (g_state != SUB_STATE_IDLE && g_state != SUB_STATE_SEND_BACK) return;
    s_jam_count = 0;                  // 新的一张牌：重置卡牌重试计数
    deal_begin_forward();
}

// ---- 自动连续发牌 API ----
void sub_auto_deal_arm(long count) {
    s_auto_pending = true;
    s_auto_active  = false;
    s_auto_left    = (count <= 0) ? -1 : count;
    s_auto_start_ms = millis() + AUTO_DEAL_START_DELAY_MS;
    dbg_printf("[SUB] auto deal armed: starts in %u ms (type 'auto off' to cancel, left=%ld)\n",
                  (unsigned)AUTO_DEAL_START_DELAY_MS, s_auto_left);
}

void sub_auto_deal_set(long count) {
    s_auto_pending = false;
    s_auto_active  = true;
    s_auto_left    = (count <= 0) ? -1 : count;
    if (g_state == SUB_STATE_IDLE) {
        dbg_printf("[SUB] auto deal started, left=%ld\n", s_auto_left);
        deal_start_now();
    } else {
        dbg_printf("[SUB] auto deal queued, left=%ld (current card finishes first)\n", s_auto_left);
    }
}

void sub_auto_deal_stop(void) {
    s_auto_pending = false;
    s_auto_active  = false;
    s_auto_left    = 0;
    dbg_println("[SUB] auto deal stopped");
}

// 底板命令分发
void sub_state_handle_command(uint8_t cmd, const uint8_t *data, uint8_t len) {
    (void)data; (void)len;
    switch (cmd) {
    case CMD_DEAL_START:
        deal_start_now();             // 手动单张（自动模式运行中会被状态保护忽略）
        break;
    case CMD_STOP:
    case CMD_RESET:
        busy_motor_stop();
        sub_camera_cancel();          // 终止截图会话：丢掉可能迟到的识别结果（见 hardware.cpp）
        g_error = 0;
        sub_auto_deal_stop();         // 停止/复位同时取消自动发牌
        g_state = SUB_STATE_IDLE;
        break;
    case CMD_SELF_TEST:
        busy_self_test();
        // 复检结果同样上报（底板能看到最新位图）；只在开机自检时上报会漏掉后续复检
        {
            uint16_t b = sub_selftest_bits();
            uint8_t d[2] = { (uint8_t)(b & 0xFF), (uint8_t)((b >> 8) & 0xFF) };
            proto_send(EVT_READY, d, sizeof(d));
        }
        break;
    case CMD_CAM_CAPTURE:
        sub_camera_trigger();         // 把 PIN_CAM_TRIG 拉低（下降沿）触发截图
        dbg_println("[SUB] CAM trigger");
        break;
    case CMD_STATUS_QUERY:
        sub_status_report();   // 心跳应答（EVT_STATUS）
        break;
    default:
        break;
    }
}

// 物理层异常：立即上报，进入 ERROR 等待底板恢复（架构第七章）
void sub_mark_error(uint8_t errorType) {
    g_error = errorType;
    busy_motor_stop();
    busy_error_handle(errorType);
    proto_send(errorType, NULL, 0);
    sub_auto_deal_stop();             // 异常时取消自动发牌，等底板处理
    g_state = SUB_STATE_ERROR;
}

// 判为失败（卡住 / 没出去）：先停正转，再反转把牌撤回
// （撤回是否成功、要不要重试，在 SUB_STATE_RETRACT_WAIT 里判定）
static void photo_enter_retract(card_result_t r) {
    dbg_printf("[SUB] retract: %s -> reverse %dms\n", card_result_name(r), PHOTO_RETRACT_MS);
    busy_motor_stop();
    busy_motor_start_reverse();
    g_state = SUB_STATE_RETRACT;
    g_state_start_ms = millis();
}

// 状态回执（心跳应答）：底板 CMD_STATUS_QUERY → 回 EVT_STATUS
// data = [state, error, countLo, countHi]
void sub_status_report(void) {
    uint8_t d[4];
    d[0] = (uint8_t)g_state;
    d[1] = g_error;
    d[2] = (uint8_t)(s_deal_count & 0xFF);
    d[3] = (uint8_t)((s_deal_count >> 8) & 0xFF);
    if (proto_send(EVT_STATUS, d, sizeof(d))) {
        dbg_printf("[SUB] TX: STATUS state=%u error=%u count=%lu\n",
                   (unsigned)d[0], (unsigned)d[1], (unsigned long)s_deal_count);
    }
}

void sub_state_run(void) {
    uint32_t now = millis();

    switch (g_state) {
    case SUB_STATE_IDLE:
        // 上电自动发牌：延时到点后开始
        if (s_auto_pending && (int32_t)(now - s_auto_start_ms) >= 0) {
            s_auto_pending = false;
            s_auto_active  = true;
            dbg_printf("[SUB] auto deal started, left=%ld\n", s_auto_left);
            deal_start_now();
        }
        break;

    case SUB_STATE_MOTOR_ON:
        // 正转出牌：主动维持驱动（GPIO 状态异常自愈）；期间只记录光电门，不做判断
        busy_motor_start();
        photo_trace_sample();
        if (now - g_state_start_ms >= MOTOR_FWD_MS) {
            if (MOTOR_BRAKE_MS > 0) {
                busy_motor_brake();               // 先短刹车，缓解换向冲击
                g_state = SUB_STATE_BRAKE;
            } else if (MOTOR_REV_MS > 0) {
                busy_motor_start_reverse();
                g_state = SUB_STATE_REVERSE;
            } else {
                busy_motor_stop();
                g_state = SUB_STATE_PAUSE;
            }
            g_state_start_ms = now;
            // TODO: 电流检测 → 堵转立即 sub_mark_error(EVT_ERROR_MOTOR_STALL)
        }
        break;

    case SUB_STATE_BRAKE:
        // 保持短刹车，再进入反转（或直接进入停顿）
        busy_motor_brake();
        photo_trace_sample();
        if (now - g_state_start_ms >= MOTOR_BRAKE_MS) {
            if (MOTOR_REV_MS > 0) {
                busy_motor_start_reverse();
                g_state = SUB_STATE_REVERSE;
            } else {
                busy_motor_stop();
                g_state = SUB_STATE_PAUSE;
            }
            g_state_start_ms = now;
        }
        break;

    case SUB_STATE_REVERSE:
        // 主动维持反转驱动（GPIO 状态异常自愈）
        busy_motor_start_reverse();
        photo_trace_sample();
        if (now - g_state_start_ms >= MOTOR_REV_MS) {
            busy_motor_stop();
            g_state = SUB_STATE_PAUSE;
            g_state_start_ms = now;
        }
        break;

    case SUB_STATE_PAUSE:
        // 电机已停：这一段既是"牌堆复位"的停顿，也是判定前的稳定窗口
        busy_motor_stop();
        if (now - g_state_start_ms >= MOTOR_PAUSE_MS) {
            // 动作结束（正转/刹车/反转都跑完、电机停稳）→ 判定这一张
            card_result_t r = USE_PHOTO_SENSOR ? photo_trace_eval() : CARD_RESULT_OUT;
            if (r == CARD_RESULT_OUT) {
                proto_send(EVT_CARD_OUT, NULL, 0);    // 确认这一张已成功发出
                g_state = SUB_STATE_SEND_BACK;
            } else {
                photo_enter_retract(r);               // 失败 → 反转撤回 + 重试（见 RETRACT_WAIT）
            }
        }
        break;

    case SUB_STATE_RETRACT:
        busy_motor_start_reverse();                // 持续反转撤回
        if (now - g_state_start_ms >= PHOTO_RETRACT_MS) {
            busy_motor_stop();
            g_state = SUB_STATE_RETRACT_WAIT;
            g_state_start_ms = now;
        }
        break;

    case SUB_STATE_RETRACT_WAIT:
        // 等门恢复“无牌”，确认牌已被拉回
        if (!sub_photo_present() && sub_photo_stable_ms() >= PHOTO_GONE_MS) {
            s_jam_count++;
            if (s_jam_count <= PHOTO_JAM_RETRY_MAX) {
                dbg_printf("[SUB] retract OK -> retry #%u\n", (unsigned)s_jam_count);
                deal_begin_forward();              // 重新发这一张
            } else {
                dbg_println("[SUB] retract OK but retry exhausted");
                sub_mark_error(EVT_ERROR_CARD_JAM);
            }
        } else if (now - g_state_start_ms >= PHOTO_CLEAR_MS) {
            dbg_println("[SUB] retract FAILED (card still at gate)");
            sub_mark_error(EVT_ERROR_CARD_JAM);
        }
        break;

    case SUB_STATE_SEND_BACK:
        // 单张流程完成回传。牌面值（EVT_CARD_VALUE）改由摄像头回传路径在
        // “截图→识别”阶段发出（见 hardware.cpp sub_camera_service），此处不再重复发送。
        proto_send(EVT_DEAL_DONE, NULL, 0);
        s_deal_count++;
        s_jam_count = 0;                  // 这张成功完成：重置重试计数
        dbg_printf("[SUB] card #%lu out (fwd=%dms brake=%dms rev=%dms)\n",
                   (unsigned long)s_deal_count, MOTOR_FWD_MS, MOTOR_BRAKE_MS, MOTOR_REV_MS);

        if (s_auto_active) {
            // 自动发牌：直接开始下一张
            if (s_auto_left > 0) {
                s_auto_left--;
                if (s_auto_left == 0) {
                    s_auto_active = false;
                    dbg_println("[SUB] auto deal finished");
                    g_state = SUB_STATE_IDLE;
                    break;
                }
            }
            deal_start_now();                       // s_auto_left = -1(无限) 或 >0
            if (s_auto_left > 0) dbg_printf("[SUB] auto next card, left=%ld\n", s_auto_left);
            break;                                  // 已进入 MOTOR_ON
        }
        g_state = SUB_STATE_IDLE;
        break;

    case SUB_STATE_ERROR:
        // 等待底板 CMD_RESET / CMD_STOP 恢复（在 handle_command 中处理）
        break;

    default:
        g_state = SUB_STATE_IDLE;
        break;
    }
}



