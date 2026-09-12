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
        struct { uint8_t selectedIndex; uint8_t confirmed; uint8_t orderRandom; } menu;
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

// 把当前"发牌选择状态"（方案/是否确认/顺序或随机）刷到屏幕上。
// 业务状态存在 deal_selection 模块里，显示层只负责画——调用方不需要自己拼显示命令。
void display_send_menu(void);   // 增量刷新（旋转/确认/切换发牌方式时用）
void display_send_idle(void);   // 整屏重绘 IDLE（进入 IDLE 状态、屏幕测试用）

// 上电自检汇总屏（setup() 里在显示任务启动前直接绘制；调用前必须先 display_init()）
void display_show_selftest(uint16_t bits);

// 发送一条调试提示（显示任务会停留约 0.7s 再继续）
void send_display_debug(const char *msg);
