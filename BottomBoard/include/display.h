#pragma once

#include <stdint.h>
#include <stdbool.h>

#define DEAL_ERROR_MAX 3   // 发牌界面最多同时显示的错误条数

/**
 * 屏幕显示：事件驱动（架构 v2 附录 A）
 * 显示任务只展示、不决策；其他任务通过 send_display_command 发命令。
 */

typedef enum {
    DISPLAY_CMD_IDLE,          // IDLE: 已选方案 + 确认按钮
    DISPLAY_CMD_SELECT,        // 高亮某个方案（含是否已确认）
    DISPLAY_CMD_DEALING,       // 发牌中（进度）
    DISPLAY_CMD_GAME_ACTIVE,   // 牌局进行中
    DISPLAY_CMD_ERROR,         // 错误信息
    DISPLAY_CMD_DEBUG          // 调试提示（如“已选择/已取消”）
} display_cmd_type_t;

typedef struct {
    display_cmd_type_t type;
    union {
        struct { uint8_t selectedIndex; uint8_t confirmed; char menuList[4][16]; } menu;
        struct {
            uint8_t progress;                       // 0~100
            uint8_t scheme;                         // 0-based 方案索引
            uint8_t deck;                           // 1-based 当前牌堆
            char status[24];                        // 当前阶段（如 rotate deck 2）
            char errors[DEAL_ERROR_MAX][24];        // 错误列表（多条一并显示）
            uint8_t errorCount;
        } dealing;
        struct { char cardInfo[32]; uint8_t remaining; } game;
        struct { char errorMsg[32]; } error;
        struct { char msg[32]; } debug;
    } payload;
} display_cmd_t;

// 非阻塞发送；队列满时丢弃（UI 只关心最终状态）
bool send_display_command(const display_cmd_t *cmd);

// 屏幕初始化（显示任务启动时调用一次）
void display_init(void);

// 显示任务收到命令后分发绘制
void display_handle_command(const display_cmd_t *cmd);

// 当前选中的方案索引（编码器任务更新，状态机发送菜单时使用）
void display_set_selected(uint8_t index);
uint8_t display_get_selected(void);

// 是否已通过按下确认方案（两段式：先确认方案，再按 CONFIRM 发牌）
void display_set_confirmed(bool on);
bool display_get_confirmed(void);

// 发送一条调试提示（显示任务会停留约 0.7s 再继续）
void send_display_debug(const char *msg);
