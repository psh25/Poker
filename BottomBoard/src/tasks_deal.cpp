/**
 * 发牌控制任务（从 tasks.cpp 拆出）：把发牌计划执行成实际的转盘/发牌动作。
 * 计划生成见 deal_config，选择状态见 deal_selection。
 */
#include <Arduino.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/event_groups.h>

#include "app_config.h"
#include "deal_config.h"
#include "deal_selection.h"
#include "itc.h"
#include "tasks.h"
#include "tasks_deal.h"
#include "state_machine.h"
#include "protocol.h"
#include "display.h"
#include "hardware.h"

// ================= 发牌控制：当前发牌运行状态（发牌任务写，CLI/编码器读）=================
static uint8_t  s_deal_scheme = 0;            // 当前方案（0-based）
static uint8_t  s_deal_deck = 0;              // 当前目标牌堆（0-based）
static uint8_t  s_deal_progress = 0;          // 0~100
static uint16_t s_dealt_count = 0;            // 已成功发出张数
static bool     s_deal_error_active = false;  // 是否处于发牌错误（等待编码器重置）
static uint8_t  s_deal_error_count = 0;
static char     s_deal_errors[DEAL_ERROR_MAX][24];
// 发牌任务的工作计划缓冲（顺序/随机每局现场生成；CLI dealinfo 也读它）
static deal_plan_t s_runningPlan;
// 模拟事件开关：**默认关闭**，且只对 TEST 方案生效（见 vDealTask）。
// 真实方案一律走真实外设，避免模拟事件掩盖硬件故障。
static bool     s_sim_auto = false;

// 已发牌面数据：每张 2 字节（[card, src]，见 protocol.h 的牌面编码），
// 按发牌顺序保存，供后续整局上传小程序使用。
static uint8_t  s_dealt_cards[DEAL_TOTAL_CARDS_MAX][2];
static uint8_t  s_dealt_lens[DEAL_TOTAL_CARDS_MAX];

// ================= 发牌控制任务 =================
bool deal_error_active(void) { return s_deal_error_active; }

static void deal_add_error(const char *msg) {
    if (s_deal_error_count < DEAL_ERROR_MAX) {
        strncpy(s_deal_errors[s_deal_error_count], msg, sizeof(s_deal_errors[0]) - 1);
        s_deal_errors[s_deal_error_count][sizeof(s_deal_errors[0]) - 1] = '\0';
        s_deal_error_count++;
    }
}

// 发牌界面刷新：携带当前牌堆、进度、阶段状态与全部错误
static void deal_update_screen(const char *status) {
    display_cmd_t cmd = {};
    cmd.type = DISPLAY_CMD_DEALING;
    cmd.payload.dealing.scheme = s_deal_scheme;
    cmd.payload.dealing.deck = s_deal_deck + 1;
    cmd.payload.dealing.progress = s_deal_progress;
    strncpy(cmd.payload.dealing.status, status, sizeof(cmd.payload.dealing.status) - 1);
    for (uint8_t i = 0; i < s_deal_error_count && i < DEAL_ERROR_MAX; i++) {
        strncpy(cmd.payload.dealing.errors[i], s_deal_errors[i], sizeof(cmd.payload.dealing.errors[0]) - 1);
    }
    cmd.payload.dealing.errorCount = s_deal_error_count;
    send_display_command(&cmd);
}

// 发牌失败：向子板发停机 → 屏幕显示错误 → 停在 DEALING 等编码器重置
static void deal_fail(void) {
    s_deal_error_active = true;
    proto_send(CMD_STOP, NULL, 0);            // 底板向子板发送停机信息
    deal_update_screen("DEAL ERROR");
    xEventGroupSetBits(xStateEventGroup, BIT_DEAL_ERROR);
}

// 方案四测试：自动补发事件参数（摄像头/光敏未就绪时使用）
typedef struct {
    uint8_t  type;                  // 自动注入的事件类型
    uint8_t  len;
    uint8_t  data[PROTO_MAX_DATA];
    uint32_t delayMs;               // 开始等待该时长后注入
} deal_sim_t;

// 等待指定类型事件：忽略其他事件；子板 EVT_ERROR_* 记录错误后立即返回 false；
// sim 非空且超过 sim->delayMs 仍无真实事件时，自动注入模拟事件；超时返回 false
static bool deal_wait_evt_core(QueueHandle_t q, uint8_t want, uint32_t timeoutMs,
                               proto_frame_t *out, const deal_sim_t *sim) {
    uint32_t start = millis();
    uint32_t deadline = start + timeoutMs;
    bool injected = false;
    while ((int32_t)(millis() - deadline) < 0) {
        if (motion_abort_requested()) return false;   // STOP/RESET：立即退出等待
        if (sim && !injected && (uint32_t)(millis() - start) >= sim->delayMs) {
            injected = true;
            proto_frame_t f = {};
            f.type = sim->type;
            f.len = sim->len;
            if (sim->len) memcpy(f.data, sim->data, sim->len);
            Serial.printf("[SIM] auto 0x%02X len=%u after %u ms\n",
                          f.type, f.len, (unsigned)sim->delayMs);
            xQueueSend(q, &f, 0);
        }
        proto_frame_t evt;
        if (xQueueReceive(q, &evt, pdMS_TO_TICKS(50)) != pdPASS) continue;
        if (evt.type == want) {
            if (out) *out = evt;
            return true;
        }
        // 子板错误事件：记录后立即返回 false（触发停机），不再等超时
        if (evt.type == EVT_ERROR_CARD_JAM)         { deal_add_error("sub: card jam"); return false; }
        else if (evt.type == EVT_ERROR_MOTOR_STALL) { deal_add_error("sub: motor stall"); return false; }
        else if (evt.type == EVT_ERROR_CAM_FAIL)    { deal_add_error("sub: cam fail"); return false; }
        // 其他事件（EVT_DEAL_DONE / EVT_CARD_VALUE 等）忽略，继续等目标事件
    }
    return false;
}

// 底盘转盘转到目标牌堆（AccelStepper 实际转动；超时返回 false）
static bool rotate_to_deck(uint8_t deck) {
    Serial.printf("[DEAL] rotate to deck %u (angle %d deg)\n",
                  deck + 1, (int)kDeckAngles[deck]);
    return chassis_rotate_to_angle(kDeckAngles[deck]);
}

void vDealTask(void *pv) {
    // 中高(2) | 固定核心 1 | 信号量触发（IDLE 确认 / CLI deal）
    for (;;) {
        if (xSemaphoreTake(xDealSemaphore, portMAX_DELAY) != pdPASS) continue;

        // 等状态机切到 DEALING（最多 500ms）：编码器/主机是"先给信号量、再置事件位"，
        // 这里等一下可保证屏幕与状态先就位；若期间被 STOP/RESET 打断就放弃本次请求。
        uint32_t t_state = millis();
        while (state_get_current() != STATE_DEALING &&
               (uint32_t)(millis() - t_state) < 500) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (state_get_current() != STATE_DEALING) {
            Serial.println("[DEAL] 放弃启动：状态未进入 DEALING（可能刚被 STOP/RESET）");
            continue;
        }

        // 新一轮发牌：清空错误/计数，锁存方案
        s_deal_error_count = 0;
        s_deal_error_active = false;
        s_dealt_count = 0;
        s_deal_progress = 0;
        s_deal_deck = 0;
        s_deal_scheme = deal_selection_get_scheme();
        busy_deal_step("deal_start");

        // 按“方案参数 + 当前发牌方式（顺序/随机）”现场生成计划
        char perr[48];
        if (!deal_build_plan(&s_runningPlan, s_deal_scheme,
                             deal_selection_get_order_random() ? DEAL_ORDER_RANDOM
                                                        : DEAL_ORDER_SEQUENTIAL,
                             perr, sizeof(perr))) {
            deal_add_error(perr);
            deal_fail();
            continue;
        }
        const deal_plan_t *plan = &s_runningPlan;
        motion_abort_clear();          // 新一局：清掉上一次 STOP/RESET 留下的中止标志

        // 模拟事件：只对 TEST 方案生效，且默认关闭（真实方案一律走真实外设）
        bool simOn = s_sim_auto && (s_deal_scheme == DEAL_INDEX_TEST);

        // 真实模式下要求子板在线，否则直接拒绝，避免白等一串超时
        if (!simOn && !sub_comm_online()) {
            deal_add_error("sub offline");
            Serial.println("[DEAL] 子板不在线，已取消（先检查共地/串口接线）");
            deal_fail();
            continue;
        }
        if (simOn) Serial.println("[SIM] TEST 方案：使用模拟事件（simauto off 可关闭）");

        // 旋转测试：不发牌，底盘连续转若干圈后自动回 IDLE
        if (plan->mode == DEAL_MODE_ROTATE_TEST) {
            char status[24];
            snprintf(status, sizeof(status), "rotate %u turns", (unsigned)plan->rotateTurns);
            deal_update_screen(status);
            Serial.printf("[GAME] rotate test %u top turns ...\n", (unsigned)plan->rotateTurns);
            if (!chassis_rotate_turns((int32_t)plan->rotateTurns)) {
                if (motion_abort_requested()) continue;   // STOP/RESET：静默退出
                deal_add_error("rotate test timeout");
                deal_fail();
                continue;
            }
            s_deal_progress = 100;
            deal_update_screen("rotate test done");
            Serial.println("[GAME] rotate test done, back to IDLE");
            xEventGroupSetBits(xStateEventGroup, BIT_RESET);
            continue;
        }

        if (plan->groupCount == 0 || plan->totalCards == 0) {
            deal_add_error("scheme empty");
            deal_fail();
            continue;
        }

        Serial.printf("[GAME] start %s: %u groups, %u cards\n",
                      plan->name, (unsigned)plan->groupCount, (unsigned)plan->totalCards);

        uint16_t dealt = 0;

        // 按发牌计划逐组执行；每张牌的时序：
        //   发截图命令 → 转盘到位 → 等牌面(EVT_CARD_VALUE) → 下发发牌
        //   → 等出牌成功(EVT_CARD_OUT) → 等子板收尾完成(EVT_DEAL_DONE) → 下一张
        // 等 DEAL_DONE 是为了确保子板已完成 刹车/反转/停顿 回到 IDLE，
        // 否则下一张的 CMD_DEAL_START 可能早到并被子板静默忽略。
        for (uint8_t gi = 0; gi < plan->groupCount && !s_deal_error_active; gi++) {
            if (motion_abort_requested()) break;
            const deal_group_t *g = &plan->groups[gi];
            s_deal_deck = g->deck;
            char status[24];

            for (uint8_t k = 0; k < g->count && !s_deal_error_active; k++) {
                if (motion_abort_requested()) break;
                proto_frame_t face;
                deal_sim_t sim = {};

                // 1) 底板 → 子板：截图指令（子板把 PIN_CAM_TRIG 拉低一个脉冲）
                deal_update_screen("cam capture");
                if (!proto_send(CMD_CAM_CAPTURE, NULL, 0)) {
                    deal_add_error("tx queue full");
                    deal_fail();
                    break;
                }

                // 2) 转到本张牌对应的实体牌堆（同组后续张 delta=0，立即返回）
                snprintf(status, sizeof(status), "rotate %s", g->label);
                deal_update_screen(status);
                if (!rotate_to_deck(g->deck)) {
                    if (motion_abort_requested()) break;      // STOP/RESET：静默退出
                    deal_add_error("chassis rotate timeout");
                    deal_fail();
                    break;
                }

                // 3) 等待识别完成 → EVT_CARD_VALUE（与其它事件同一个队列）
                snprintf(status, sizeof(status), "%s face %u/%u",
                         g->label, (unsigned)k + 1, (unsigned)g->count);
                deal_update_screen(status);
                if (simOn) {
                    sim.type = EVT_CARD_VALUE;
                    sim.len = 2;
                    sim.data[0] = (uint8_t)(dealt % 52);   // 模拟牌面：0~51 轮换
                    sim.data[1] = CARD_SRC_DEBUG;          // src 标记为调试/模拟
                    sim.delayMs = SIM_CAMERA_DELAY_MS;
                }
                bool haveFace = deal_wait_evt_core(xSubboardRxQueue, EVT_CARD_VALUE, CAMERA_TIMEOUT_MS,
                                                   &face, simOn ? &sim : NULL);
                if (!haveFace) {
                    if (motion_abort_requested()) break;   // STOP/RESET：静默退出
                    deal_add_error("card face timeout");   // 未识别到牌面 → 立即停机
                    deal_fail();
                    break;
                }

                // 4) 下发子板发牌指令
                snprintf(status, sizeof(status), "deal %s %u/%u",
                         g->label, (unsigned)k + 1, (unsigned)g->count);
                deal_update_screen(status);
                if (!proto_send(CMD_DEAL_START, NULL, 0)) {
                    deal_add_error("tx queue full");
                    deal_fail();
                    break;
                }

                // 5) 等待本张牌发出（光电门 EVT_CARD_OUT）
                if (simOn) { sim.type = EVT_CARD_OUT; sim.len = 0; sim.delayMs = SIM_PHOTO_DELAY_MS; }
                if (!deal_wait_evt_core(xSubboardRxQueue, EVT_CARD_OUT, PHOTO_TIMEOUT_MS, NULL,
                                        simOn ? &sim : NULL)) {
                    if (motion_abort_requested()) break;   // STOP/RESET：静默退出
                    deal_add_error("card not out");
                    deal_fail();
                    break;
                }

                // 6) 等子板收尾完成（刹车/反转/停顿）→ EVT_DEAL_DONE，才允许下一张
                if (simOn) { sim.type = EVT_DEAL_DONE; sim.len = 0; sim.delayMs = SIM_PHOTO_DELAY_MS; }
                if (!deal_wait_evt_core(xSubboardRxQueue, EVT_DEAL_DONE, PHOTO_TIMEOUT_MS, NULL,
                                        simOn ? &sim : NULL)) {
                    if (motion_abort_requested()) break;   // STOP/RESET：静默退出
                    deal_add_error("deal done timeout");
                    deal_fail();
                    break;
                }

                // 7) 保存牌面数据（摄像头识别结果，[card, src]）
                if (dealt < DEAL_TOTAL_CARDS_MAX) {
                    xSemaphoreTake(xDeckDataMutex, portMAX_DELAY);
                    uint8_t n = (face.len < 2) ? face.len : 2;
                    if (n) memcpy(s_dealt_cards[dealt], face.data, n);
                    s_dealt_lens[dealt] = n;
                    xSemaphoreGive(xDeckDataMutex);
                }

                dealt++;
                s_dealt_count = dealt;
                s_deal_progress = (uint8_t)((uint32_t)dealt * 100U / plan->totalCards);
                snprintf(status, sizeof(status), "%s %u/%u ok",
                         g->label, (unsigned)k + 1, (unsigned)g->count);
                deal_update_screen(status);
            }
        }

        if (s_deal_error_active) continue;   // 错误已显示，等待编码器重置后重新开始
        if (motion_abort_requested()) {      // STOP/RESET 主动中止：静默退出，不报错、不置完成位
            Serial.printf("[DEAL] aborted by stop request (%u cards dealt)\n", (unsigned)dealt);
            continue;
        }
        s_deal_progress = 100;
        Serial.printf("[GAME] done: %u cards\n", (unsigned)dealt);

        // 全部发完 → DEALING → GAME_ACTIVE
        xEventGroupSetBits(xStateEventGroup, BIT_DEAL_COMPLETE);
    }
}

// ---- 供 CLI 读取的运行状态快照（发牌任务写，这里做一次性拷贝）----
void deal_get_status(deal_status_t *out) {
    if (!out) return;
    out->scheme = s_deal_scheme;
    out->deck = s_deal_deck;
    out->progress = s_deal_progress;
    out->dealt = s_dealt_count;
    out->total = s_runningPlan.totalCards;
    out->errorActive = s_deal_error_active;
    out->errorCount = s_deal_error_count;
    for (uint8_t i = 0; i < DEAL_ERROR_MAX; i++) {
        strncpy(out->errors[i], s_deal_errors[i], sizeof(out->errors[i]) - 1);
        out->errors[i][sizeof(out->errors[i]) - 1] = 0;
    }
}

void deal_sim_set_auto(bool on) { s_sim_auto = on; }
bool deal_sim_get_auto(void) { return s_sim_auto; }
