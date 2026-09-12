/**
 * 子板裸机状态机（docs/subboard_architecture.md 第三章）
 *
 * 定时模式（USE_PHOTO_SENSOR=0，无光电门时的调试路径）：
 *   MOTOR_ON 正转 MOTOR_FWD_MS → BRAKE → REVERSE → PAUSE → SEND_BACK
 *
 * 光电门模式（USE_PHOTO_SENSOR=1，生产路径）：
 *   MOTOR_ON 正转 → WAIT_CARD 等“有牌” → WAIT_GONE 等“无牌”确认牌完整通过
 *   → EVT_CARD_OUT → BRAKE → REVERSE → PAUSE → SEND_BACK
 *   卡牌保护：停留太久判为卡住 → RETRACT 反转撤回 → RETRACT_WAIT 等门恢复；
 *             恢复成功 → 自动重试这一张（最多 PHOTO_JAM_RETRY_MAX 次）
 *             撤回失败 / 重试超限 → ERROR（停机上报，等底板复位指令）
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
        g_error = 0;
        sub_auto_deal_stop();         // 停止/复位同时取消自动发牌
        g_state = SUB_STATE_IDLE;
        break;
    case CMD_SELF_TEST:
        busy_self_test();
        break;
    case CMD_CAM_CAPTURE:
        sub_camera_trigger();         // 拉高 PIN_CAM_TRIG 触发截图
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

// 判为卡牌：先停正转，再反转把牌撤回（撤回是否成功在 SUB_STATE_RETRACT_WAIT 里判定）
static void photo_enter_jam(const char *why) {
    dbg_printf("[SUB] JAM: %s -> retract %dms\n", why, PHOTO_RETRACT_MS);
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
        // 主动维持正转驱动（GPIO 状态异常自愈）
        busy_motor_start();

        if (!USE_PHOTO_SENSOR) {
            // 定时出牌：正转 MOTOR_FWD_MS 后认为一张已出
            if (now - g_state_start_ms >= MOTOR_FWD_MS) {
                proto_send(EVT_CARD_OUT, NULL, 0);   // 实时上报：已出一张
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
            }
        } else {
            // 光敏模式：电机启动完成 → 等待光敏检测到牌
            if (now - g_state_start_ms >= MOTOR_STARTUP_MS) {
                g_state = SUB_STATE_WAIT_CARD;
                g_state_start_ms = now;
            }
            // TODO: 电流检测 → 堵转立即 sub_mark_error(EVT_ERROR_MOTOR_STALL)
        }
        break;

    case SUB_STATE_BRAKE:
        // 保持短刹车，再进入反转（或直接进入停顿）
        busy_motor_brake();
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
        if (now - g_state_start_ms >= MOTOR_REV_MS) {
            busy_motor_stop();
            g_state = SUB_STATE_PAUSE;
            g_state_start_ms = now;
        }
        break;

    case SUB_STATE_PAUSE:
        // 每张牌之间的停顿：让牌完全出去、牌堆复位
        busy_motor_stop();
        if (now - g_state_start_ms >= MOTOR_PAUSE_MS) {
            g_state = SUB_STATE_SEND_BACK;
        }
        break;

    case SUB_STATE_WAIT_CARD:
        // 光电门模式：等“有牌”（电平已去抖）
        if (sub_photo_present()) {
            g_state = SUB_STATE_WAIT_GONE;         // 牌到门口了，继续等它完全离开
            g_state_start_ms = now;
        } else if (now - g_state_start_ms >= PHOTO_TIMEOUT_MS) {
            photo_enter_jam("no card seen");       // 一直没看到牌：漏发或卡在里面
        }
        break;

    case SUB_STATE_WAIT_GONE:
        // 等“无牌”并稳定 PHOTO_GONE_MS → 这张牌完整通过
        if (!sub_photo_present() && sub_photo_stable_ms() >= PHOTO_GONE_MS) {
            busy_motor_stop();
            proto_send(EVT_CARD_OUT, NULL, 0);     // 实时上报：已出一张
            // 复用原有收尾动作：刹车 → 反转回退 → 停顿
            if (MOTOR_BRAKE_MS > 0) {
                busy_motor_brake();
                g_state = SUB_STATE_BRAKE;
            } else if (MOTOR_REV_MS > 0) {
                busy_motor_start_reverse();
                g_state = SUB_STATE_REVERSE;
            } else {
                g_state = SUB_STATE_PAUSE;
            }
            g_state_start_ms = now;
        } else if (sub_photo_present() && sub_photo_stable_ms() >= PHOTO_JAM_MS) {
            photo_enter_jam("card stuck at exit"); // 有牌一直不走 = 卡在出牌口
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



