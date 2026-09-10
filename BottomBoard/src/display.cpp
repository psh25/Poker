/**
 * 屏幕显示（架构 v2 附录 A）：事件驱动、非阻塞发送、只展示不决策。
 * 初步实现：方案选择菜单（旋转切换/取消、短按确认、再按 CONFIRM 发牌）。
 * 屏幕：1.8" ST7735 面板 128x160，软件按横屏 160x128 使用（SPI 引脚见 platformio.ini）。
 * 注：当前用 ASCII 文本（TFT_eSPI 默认字体不含中文），中文显示后续可加字体。
 */
#include <Arduino.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <TFT_eSPI.h>

#include "app_config.h"
#include "deal_config.h"
#include "itc.h"
#include "display.h"
#include "hardware.h"

static TFT_eSPI tft;

// 开机自检色块开关：确认屏幕与接线正常后可改为 0 去掉（白屏调试用）
#define DISPLAY_DIAG_COLORS 1

// 方案名由 deal_config 统一提供（预置游戏 + Custom）

static uint8_t g_selected = 0;   // 当前选中的方案索引

void display_set_selected(uint8_t index) {
    g_selected = index;
}

uint8_t display_get_selected(void) {
    return g_selected;
}

static uint8_t g_confirmed = 0;  // 是否已通过按下确认方案（0=未选择，顶部显示 -）

void display_set_confirmed(bool on) {
    g_confirmed = on ? 1 : 0;
}

bool display_get_confirmed(void) {
    return g_confirmed != 0;
}

void display_init(void) {
    tft.init();
    tft.setRotation(1);            // 横屏 160x128（若左右颠倒改为 3）

#if DISPLAY_DIAG_COLORS
    // 依次显示 红→绿→蓝→黑，各 300ms：
    //  - 能正常显示色块：面板/接线/驱动 OK，问题在后续菜单绘制；
    //  - 一直白屏：多为驱动 IC 不匹配、RST/CS/背光未接或 VCC 供电问题。
    uint16_t colors[] = {TFT_RED, TFT_GREEN, TFT_BLUE, TFT_BLACK};
    for (int i = 0; i < 4; i++) {
        tft.fillScreen(colors[i]);
        delay(300);
    }
#endif

    tft.fillScreen(TFT_BLACK);
}

static uint8_t s_drawn = 0;   // 当前屏幕上高亮的行
static uint8_t s_menu_offset = 0;   // 菜单当前显示的首个方案索引（列表可滚动）
#define MENU_ROWS 4                 // 横屏一屏显示 4 个方案
// 当前屏幕布局类型：IDLE 屏旋转时需要同步刷新顶部 “Selected: N”
enum { SCREEN_OTHER = 0, SCREEN_IDLE };
static uint8_t s_screen = SCREEN_OTHER;

// 画一行方案（visualRow=屏幕第几行；schemeIndex=方案索引；选中项蓝底白字）
static void draw_row(uint8_t visualRow, uint8_t schemeIndex, uint8_t selectedIndex) {
    uint16_t y = 13 + visualRow * 19;   // 横屏：行高 16，行距 19，一屏 4 行
    uint16_t bg = (schemeIndex == selectedIndex) ? TFT_BLUE : TFT_DARKGREY;
    uint16_t fg = (schemeIndex == selectedIndex) ? TFT_WHITE : TFT_LIGHTGREY;
    tft.fillRoundRect(8, y, 144, 16, 3, bg);
    tft.setTextColor(fg, bg);
    tft.setCursor(14, y + 4);
    tft.setTextSize(1);
    tft.print(deal_scheme_name(schemeIndex));
}

// 保证选中项在可视窗口内，返回窗口首索引
static uint8_t menu_offset_for(uint8_t selectedIndex) {
    if (selectedIndex < s_menu_offset) return selectedIndex;
    if (selectedIndex >= s_menu_offset + MENU_ROWS) {
        return (uint8_t)(selectedIndex - MENU_ROWS + 1);
    }
    return s_menu_offset;
}

// 按当前窗口绘制可见的方案行
static void draw_menu(uint8_t selectedIndex) {
    for (uint8_t r = 0; r < MENU_ROWS; r++) {
        uint8_t idx = (uint8_t)(s_menu_offset + r);
        if (idx >= SCHEME_COUNT) break;
        draw_row(r, idx, selectedIndex);
    }
}

// IDLE 屏顶部：已确认显示 “Selected: N”，未确认显示 “Selected: -”
static void draw_idle_header(uint8_t selectedIndex, uint8_t confirmed) {
    tft.fillRect(8, 1, 144, 9, TFT_BLACK);      // 清掉旧数字
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextSize(1);
    tft.setCursor(8, 2);
    if (confirmed) {
        tft.print("Selected: ");
        tft.print(selectedIndex + 1);
    } else {
        tft.print("Selected: -");
    }
    // 方案多于 4 个时显示页码（如 1/2）
    if (SCHEME_COUNT > MENU_ROWS) {
        tft.setTextColor(TFT_CYAN, TFT_BLACK);
        tft.setCursor(134, 2);
        tft.print(s_menu_offset / MENU_ROWS + 1);
        tft.print("/");
        tft.print((SCHEME_COUNT + MENU_ROWS - 1) / MENU_ROWS);
    }
}

// 底部 CONFIRM 按钮：已确认方案 = 绿色可用（再按进入发牌）；未确认 = 置灰
static void draw_confirm_button(uint8_t confirmed) {
    uint16_t bg = confirmed ? TFT_GREEN : TFT_DARKGREY;
    tft.fillRoundRect(8, 92, 144, 18, 4, bg);
    tft.setTextColor(TFT_BLACK, bg);
    tft.setCursor(59, 97);                      // 7 字符居中（144 宽 / 6px 每字符）
    tft.print("CONFIRM");

    tft.setTextColor(bg, TFT_BLACK);
    tft.setCursor(8, 114);
    tft.print(confirmed ? "Press=OK" : "Press=Sel");
}

// IDLE 屏幕：顶部（未选择/已选方案）+ 方案列表 + 底部确认按钮
static void draw_idle(uint8_t selectedIndex, uint8_t confirmed) {
    tft.fillScreen(TFT_BLACK);

    s_menu_offset = menu_offset_for(selectedIndex);
    draw_idle_header(selectedIndex, confirmed);
    tft.drawFastHLine(8, 11, 144, TFT_WHITE);

    draw_menu(selectedIndex);

    draw_confirm_button(confirmed);
    s_drawn = selectedIndex;
    s_screen = SCREEN_IDLE;
}

// 增量更新：顶部 + 确认按钮 + 高亮行（旋转 / 确认 / 取消时调用）
static void draw_select(uint8_t selectedIndex, uint8_t confirmed) {
    uint8_t newOffset = menu_offset_for(selectedIndex);
    if (newOffset != s_menu_offset) {
        s_menu_offset = newOffset;
        draw_idle(selectedIndex, confirmed);   // 需要滚动 → 整屏重绘
        return;
    }
    if (s_screen == SCREEN_IDLE) {
        draw_idle_header(selectedIndex, confirmed);
        draw_confirm_button(confirmed);
    }
    if (selectedIndex != s_drawn) {
        draw_row((uint8_t)(s_drawn - s_menu_offset), s_drawn, selectedIndex);       // 旧行 → 灰
        draw_row((uint8_t)(selectedIndex - s_menu_offset), selectedIndex, selectedIndex); // 新行 → 蓝
        s_drawn = selectedIndex;
    }
}

// 发牌界面：方案名 + 当前牌堆 + 进度 + 阶段状态 + 错误（最多 DEAL_ERROR_MAX 条）
static void draw_dealing(const display_cmd_t *cmd) {
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextSize(1);

    tft.setCursor(8, 2);
    tft.print(deal_scheme_name(cmd->payload.dealing.scheme % SCHEME_COUNT));

    tft.setCursor(8, 12);
    tft.print("Deck: ");
    tft.print(cmd->payload.dealing.deck);
    tft.print("  ");
    tft.print(cmd->payload.dealing.progress);
    tft.print("%");

    tft.setCursor(8, 22);
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.print(cmd->payload.dealing.status);

    for (uint8_t i = 0; i < cmd->payload.dealing.errorCount && i < DEAL_ERROR_MAX; i++) {
        tft.setCursor(8, 33 + i * 10);
        tft.setTextColor(TFT_RED, TFT_BLACK);
        tft.print(cmd->payload.dealing.errors[i]);
    }

    // 出错时提示：按下编码器重置回 IDLE
    if (cmd->payload.dealing.errorCount > 0) {
        tft.setCursor(8, 116);
        tft.setTextColor(TFT_YELLOW, TFT_BLACK);
        tft.print("Press=Reset");
    }
}

// GAME_ACTIVE 屏：信息放顶部（紧凑），长按提示放底部；
// 屏幕中部与短按编码器预留给后续功能（如选牌、预览等）。
static void draw_game_active(const display_cmd_t *cmd) {
    tft.fillScreen(TFT_BLACK);
    tft.setTextSize(1);

    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setCursor(8, 2);
    tft.print("Game Active");

    if (cmd->payload.game.cardInfo[0]) {          // 有牌面信息时显示第二行
        tft.setTextColor(TFT_CYAN, TFT_BLACK);
        tft.setCursor(8, 12);
        tft.print(cmd->payload.game.cardInfo);
    }

    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.setCursor(8, 116);
    tft.print("Hold=End,Reset");
}

void display_handle_command(const display_cmd_t *cmd) {
    switch (cmd->type) {
    case DISPLAY_CMD_IDLE:
        draw_idle(cmd->payload.menu.selectedIndex, cmd->payload.menu.confirmed);
        break;
    case DISPLAY_CMD_SELECT:
        draw_select(cmd->payload.menu.selectedIndex, cmd->payload.menu.confirmed);
        break;

    case DISPLAY_CMD_DEALING:
        draw_dealing(cmd);
        break;

    case DISPLAY_CMD_GAME_ACTIVE:
        draw_game_active(cmd);
        break;

    case DISPLAY_CMD_ERROR:
        tft.fillScreen(TFT_RED);
        tft.setTextColor(TFT_WHITE, TFT_RED);
        tft.setTextSize(1);
        tft.setCursor(8, 60);
        tft.print(cmd->payload.error.errorMsg);
        break;

    case DISPLAY_CMD_DEBUG:
        tft.fillScreen(TFT_BLACK);
        tft.setTextColor(TFT_YELLOW, TFT_BLACK);
        tft.setTextSize(1);
        tft.setCursor(8, 60);
        tft.print(cmd->payload.debug.msg);
        delay(700);   // 调试用：让提示可见一段时间，之后下一条命令覆盖
        break;

    default:
        break;
    }
}

void send_display_debug(const char *msg) {
    display_cmd_t cmd = {};
    cmd.type = DISPLAY_CMD_DEBUG;
    strncpy(cmd.payload.debug.msg, msg, sizeof(cmd.payload.debug.msg) - 1);
    send_display_command(&cmd);
}

bool send_display_command(const display_cmd_t *cmd) {
    // 非阻塞发送；队列满丢弃（UI 只关心最终状态，快速旋转时中间态可丢）
    return xQueueSend(xDisplayQueue, cmd, 0) == pdPASS;
}
