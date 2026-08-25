/**
 * 屏幕显示（架构 v2 附录 A）：事件驱动、非阻塞发送、只展示不决策。
 * 初步实现：方案选择菜单（旋转高亮、按下确认、长按返回）。
 * 屏幕：1.8" ST7735 128x160，SPI（引脚编译参数见 platformio.ini）。
 * 注：当前用 ASCII 文本（TFT_eSPI 默认字体不含中文），中文显示后续可加字体。
 */
#include <Arduino.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <TFT_eSPI.h>

#include "app_config.h"
#include "itc.h"
#include "display.h"
#include "hardware.h"

static TFT_eSPI tft;

// 开机自检色块开关：确认屏幕与接线正常后可改为 0 去掉（白屏调试用）
#define DISPLAY_DIAG_COLORS 1

// 占位方案名：发牌方案 1~4（具体方案待定）
static const char *kSchemeNames[SCHEME_COUNT] = {
    "Deal Scheme 1",
    "Deal Scheme 2",
    "Deal Scheme 3",
    "Deal Scheme 4",
};

static uint8_t g_selected = 0;   // 当前选中的方案索引

void display_set_selected(uint8_t index) {
    g_selected = index;
}

uint8_t display_get_selected(void) {
    return g_selected;
}

void display_init(void) {
    tft.init();
    tft.setRotation(2);            // 128x160 竖屏（若方向不对改为 0）

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

// 画单行方案（选中项蓝底白字，未选中灰底浅字）
static void draw_row(int i, uint8_t selectedIndex) {
    uint16_t y = 24 + i * 30;
    uint16_t bg = (i == selectedIndex) ? TFT_BLUE : TFT_DARKGREY;
    uint16_t fg = (i == selectedIndex) ? TFT_WHITE : TFT_LIGHTGREY;
    tft.fillRoundRect(10, y, 108, 24, 3, bg);
    tft.setTextColor(fg, bg);
    tft.setCursor(16, y + 8);
    tft.setTextSize(1);
    tft.print(kSchemeNames[i]);
}

// 整屏重绘菜单（首次进入 / 状态切换时）
static void draw_menu(uint8_t selectedIndex) {
    tft.fillScreen(TFT_BLACK);

    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextSize(1);
    tft.setCursor(10, 4);
    tft.print("Deal Scheme:");
    tft.drawFastHLine(10, 16, 108, TFT_WHITE);

    for (int i = 0; i < SCHEME_COUNT; i++) {
        draw_row(i, selectedIndex);
    }

    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.setCursor(10, 150);
    tft.print("Rotate:sel Press:OK");
    s_drawn = selectedIndex;
}

// 增量更新高亮：只重绘旧行和新行，旋转时不再整屏闪烁
static void draw_select(uint8_t selectedIndex) {
    if (selectedIndex == s_drawn) return;
    draw_row(s_drawn, selectedIndex);            // 旧行 → 灰
    draw_row(selectedIndex, selectedIndex);      // 新行 → 蓝
    s_drawn = selectedIndex;
}

void display_handle_command(const display_cmd_t *cmd) {
    switch (cmd->type) {
    case DISPLAY_CMD_MENU:
        draw_menu(cmd->payload.menu.selectedIndex);
        break;
    case DISPLAY_CMD_SELECT:
        draw_select(cmd->payload.menu.selectedIndex);
        break;

    case DISPLAY_CMD_DEALING:
        tft.fillScreen(TFT_BLACK);
        tft.setTextColor(TFT_WHITE, TFT_BLACK);
        tft.setTextSize(1);
        tft.setCursor(10, 70);
        tft.print("Dealing... ");
        tft.print(cmd->payload.dealing.progress);
        tft.print("%");
        break;

    case DISPLAY_CMD_GAME_ACTIVE:
        tft.fillScreen(TFT_BLACK);
        tft.setTextColor(TFT_WHITE, TFT_BLACK);
        tft.setTextSize(1);
        tft.setCursor(10, 70);
        tft.print("Game Active");
        break;

    case DISPLAY_CMD_GAME_END:
        tft.fillScreen(TFT_BLACK);
        tft.setTextColor(TFT_WHITE, TFT_BLACK);
        tft.setTextSize(1);
        tft.setCursor(10, 70);
        tft.print("Game End");
        break;

    case DISPLAY_CMD_CARD_PREVIEW:
        tft.fillScreen(TFT_BLACK);
        tft.setTextColor(TFT_WHITE, TFT_BLACK);
        tft.setTextSize(1);
        tft.setCursor(10, 70);
        tft.print(cmd->payload.game.cardInfo);
        break;

    case DISPLAY_CMD_ERROR:
        tft.fillScreen(TFT_RED);
        tft.setTextColor(TFT_WHITE, TFT_RED);
        tft.setTextSize(1);
        tft.setCursor(10, 70);
        tft.print(cmd->payload.error.errorMsg);
        break;

    case DISPLAY_CMD_DEBUG:
        tft.fillScreen(TFT_BLACK);
        tft.setTextColor(TFT_YELLOW, TFT_BLACK);
        tft.setTextSize(1);
        tft.setCursor(10, 70);
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
