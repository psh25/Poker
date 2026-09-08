#pragma once

#include <stdint.h>
#include <stdbool.h>

/**
 * 子板硬件层：初始化、中断、环形缓冲、电机/摄像头/光敏驱动。
 * 发牌电机为 TB6612FNG（PWM + AIN1/AIN2 + STBY），见 pins_config.h。
 */

void sub_hardware_init(void);
void sub_self_test(void);          // 上电自检 → 上报 EVT_READY

// 串口接收：中断回调 → 环形缓冲；主循环通过 sub_uart_read_byte 取字节
void sub_uart_rx_isr(void);
bool sub_uart_read_byte(uint8_t *b);

// 光敏：中断置标志，主循环用 sub_photo_take 读取并清除
bool sub_photo_take(void);

// ---- 发牌电机（TB6612）----
void busy_motor_start(void);        // 正转出牌（PWM = MOTOR_DUTY）
void busy_motor_start_reverse(void);// 反转回退（PWM = MOTOR_REV_DUTY，摄像头拍牌底）
void busy_motor_stop(void);         // 停止（滑行）
void motor_self_test(void);         // 电机自检：正转/反转各 300ms（开机与 mtest 命令用）

// ---- 占位函数（TODO：按架构实现具体逻辑）----
void busy_camera_capture(uint8_t *cardData, uint8_t *cardLen);  // 识别牌面，成功置 cardLen
void busy_self_test(void);
void busy_status_query(void);
void busy_error_handle(uint8_t errorType);

