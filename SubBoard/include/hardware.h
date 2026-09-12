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

// ---- 光电门（原来的光敏传感器）：有牌 = 低电平(GND)，无牌 = 高 ----
void sub_photo_init(void);            // 配置引脚（上电调用一次）
void sub_photo_update(void);          // 主循环周期调用：采样 + 去抖
bool sub_photo_present(void);         // 去抖后：true = 当前有牌
uint32_t sub_photo_stable_ms(void);   // 当前（去抖后）电平已稳定保持多久（ms）

// ---- 摄像头：截图触发 + 回传接收（不回传 = 模组未就绪时的调试路径）----
// 回传帧格式（占位：模组确定后按手册修改）见 hardware.cpp 顶部注释。
void sub_camera_trigger(void);      // 底板 CMD_CAM_CAPTURE：拉高 PIN_CAM_TRIG（非阻塞，脉冲由服务函数收尾）
void sub_camera_service(void);      // 主循环调用：收脉冲尾、解析回传、上报 EVT_CARD_VALUE

// ---- 发牌电机（TB6612）----
void busy_motor_start(void);        // 正转出牌（PWM = MOTOR_DUTY）
void busy_motor_start_reverse(void);// 反转回退（PWM = MOTOR_REV_DUTY，摄像头拍牌底）
void busy_motor_brake(void);        // 短刹车（AIN1=AIN2=高，PWM=0）
void busy_motor_stop(void);         // 停止（滑行）
void motor_self_test(void);         // 电机自检：正转/反转各 300ms（开机与 mtest 命令用）

// ---- 占位函数（TODO：按架构实现具体逻辑）----
void busy_camera_capture(uint8_t *cardData, uint8_t *cardLen);  // 识别牌面，成功置 cardLen
void busy_self_test(void);
void busy_error_handle(uint8_t errorType);

