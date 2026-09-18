#pragma once

/**
 * 子板 IO 映射
 * ⚠️ 当前按实际飞线测试接口配置；量产前必须与原理图核对。
 */

// ---- 发牌电机：TB6612FNG ----
// PWMA = PWM（LEDC 20kHz）；AIN1/AIN2 = 方向；STBY = 高使能
// 方向：AIN1=0,AIN2=1 → 正转出牌；AIN1=1,AIN2=0 → 反转回退
#define PIN_MOTOR_STBY       2   // TB6612 STBY（高=使能）
#define PIN_MOTOR_AIN1       42  // TB6612 AIN1
#define PIN_MOTOR_AIN2       41  // TB6612 AIN2
#define PIN_MOTOR_PWM        40  // TB6612 PWMA

// ---- 光电门 ----
// 有牌 = 低电平(GND)，无牌 = 高电平。
#define PIN_PHOTO            47

// ---- 摄像头 / 视觉模组 ----
// 子板 TX=GPIO48 → 摄像头 RX
// 摄像头 TX → 子板 RX=GPIO45
// TRIG=GPIO35，低脉冲触发
#define PIN_CAM_TX           48
#define PIN_CAM_RX           45
#define PIN_CAM_TRIG         35

// ---- 与底板通信：2 线 UART ----
// 底板 TX2 → 子板 GPIO21 RX
// 子板 GPIO20 TX → 底板 RX2
// 两板必须共地。
#define PIN_UART_POS         21  // RX（连底板 TX2）
#define PIN_UART_NEG         20  // TX（连底板 RX2）