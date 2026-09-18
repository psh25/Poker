#pragma once

#include "pins_config.h"

/**
 * 子板系统级配置：时序、超时、串口、缓冲。
 * 对应 docs/subboard_architecture.md 第四、五章。
 */

// ===== 发牌电机：单张动作时序（ms / PWM 占空比 0-255）=====
// 单张动作固定为：正转 MOTOR_FWD_MS → 刹车 MOTOR_BRAKE_MS → 反转 MOTOR_REV_MS → 停顿 MOTOR_PAUSE_MS
// ⚠️ MOTOR_FWD_MS 现在是**唯一的出牌推进时长**（不再"等牌离开光门"才换相），
//    必须保证能把一张牌完整推过出牌口并留出余量；太短会把"还在路上"的牌判成卡住。
#define MOTOR_FWD_MS 420   // 正转出牌时长：一张牌送出的时间
#define MOTOR_REV_MS 280   // 出牌后反转回退时长（让下一张退到摄像头可拍位置；0=不反转）
#define MOTOR_BRAKE_MS 50 // 正转→反转之间的短刹车（AIN1=AIN2=高，PWM=0；0=直接换向）
#define MOTOR_PAUSE_MS 100 // 每张牌之间的停顿（让牌完全出去、牌堆复位；0=不停）
#define MOTOR_DUTY 200     // 正转 PWM 占空比（约 78%）
#define MOTOR_REV_DUTY 160 // 反转 PWM 占空比（约 63%）

// 出牌结果判定方式：
//   0 = 不做判定（没有光电门时的调试路径）：动作跑完一律按"成功发出"处理
//   1 = 光电门判定（生产路径）：动作期间**只记录**光电门电平，动作结束（反转停止 + 停顿）后
//       再根据"记录 + 当前电平"判定 成功发出 / 卡在出牌口 / 根本没出去（见 state_machine.cpp）
#define USE_PHOTO_SENSOR 1

// ===== 光电门（原来的光敏传感器）=====
// 电平：**有牌 = GND(低电平)**，无牌 = 高电平。
// 用法：发牌动作期间只"采样记录"，动作结束后统一判定，所以这里只留三个时间参数：
//   去抖（采样用）、门清空确认（撤回后判断是否已恢复）、撤回时长/重试次数。
#define PHOTO_DEBOUNCE_MS    20    // 电平去抖：连续稳定多久才认可该电平
#define PHOTO_GONE_MS        60    // “牌已完全离开”确认时间（撤回后等门恢复；防下一张牌微露头）
#define PHOTO_RETRACT_MS     800   // 判失败（卡住/没出去）后反转撤回的时长
#define PHOTO_CLEAR_MS       500   // 撤回后等“门恢复无牌”的时间（超时→报错）
#define PHOTO_JAM_RETRY_MAX  1     // 撤回成功后最多自动重试几次（超出则报错等重启）

// ===== 自动连续发牌（调试用：不需要底板，自己一张接一张发）=====
// AUTO_DEAL_ENABLE = 1：上电延时 AUTO_DEAL_START_DELAY_MS 后自动开始连续发牌
//   （留时间在串口敲 "auto off" 取消）
// 运行时串口命令：auto            → 无限自动发牌
//                 auto <n>        → 自动发 n 张后停
//                 auto off        → 停止自动发牌（当前这张发完为止）
#define AUTO_DEAL_ENABLE 0
#define AUTO_DEAL_COUNT -1            // 自动发牌张数：-1 = 无限循环；N = 发 N 张后停
#define AUTO_DEAL_START_DELAY_MS 1000 // 上电延时（ms），留时间取消

// ===== 时序 / 超时（ms，与底板协议一致）=====
#define CAM_TRIG_PULSE_MS 200   // PIN_CAM_TRIG 触发脉冲宽度（ms，**低电平有效**；OpenMV 要求 ≥5ms）

// ===== 摄像头回传（OpenMV 主动发回 ASCII 文本行，子板只接收解析）=====
// 硬件：摄像头 TX → 子板 PIN_CAM_RX；子板 PIN_CAM_TX → 摄像头 RX（只用于下发文本指令，见下）
#define CAM_UART_BAUD 115200        // 与 OpenMV UART3 一致（115200 8N1）
#define CAM_RESULT_TIMEOUT_MS 1200  // 触发后等待 "RESULT:..." 的最长时间（OpenMV 文档建议 1.2s）
// 1 = 超时未收到结果时，按“未知牌 + 超时来源”上报（保留调试路径，整局能跑完）
// 0 = 超时直接上报 EVT_ERROR_CAM_FAIL
#define CAM_EMPTY_ON_TIMEOUT 1

// ===== 子板 → 摄像头：文本指令（与回传同风格：ASCII + \r\n 结尾）=====
// ⚠️ OpenMV 端 main_standalone.py 目前不读 UART（见 重要信息/STANDALONE_IO_PROTOCOL(1).md 第 7 节），
//    摄像头端加一行接收处理后本指令才生效；在那之前自检只会打印“无回应”，不算失败。
// 改指令内容只改这里（保持“纯 ASCII + \r\n 结尾”即可与 RESULT: 回传同风格）。
#define CAM_CMD_CALIBRATE "CALIBRATE\r\n"

// ===== 开机自检（子板）=====
// 自检里的电机微动要明显短于正常出牌时间，避免真的把牌发出去。
#define SELFTEST_MOTOR_FWD_MS 150   // 自检正转时长（正常出牌 MOTOR_FWD_MS=390）
#define SELFTEST_MOTOR_REV_MS 300   // 自检反转时长（把可能被推出去的牌退回）
#define CAM_CALIB_WAIT_MS     3000  // 发完校准指令后等摄像头回应的时长（也是“先校准、后转电机”的间隔）

// ===== 串口（与底板通信）=====
#define SUB_UART_BAUD 115200
#define SUB_UART_RX_PIN PIN_UART_POS   // 物理 RX：GPIO21（连底板 TX2）
#define SUB_UART_TX_PIN PIN_UART_NEG   // 物理 TX：GPIO20（连底板 RX2）

// ===== 串口接收环形缓冲 =====
#define UART_RX_RING_SIZE 128

// ===== 单卡数据缓冲（架构第六章：只保留当前一张牌，无历史缓存）=====
#define CARD_DATA_MAX 32

// ===== 低功耗（A 级：只砍"待机白烧"的电，不改功能与时序）=====
// 三条措施，都不依赖任何新硬件：
//   ① 主循环节拍：裸机 loop() 原本全速空转（几十万次/秒），加一次 delay 后按 SUB_LOOP_DELAY_MS
//      节拍跑，空转耗电从 ~100% CPU 掉到个位数 %。采样率仍是 1kHz，远高于光电门去抖
//      PHOTO_DEBOUNCE_MS(20ms)，判定逻辑完全不受影响。
//   ② CPU 降频：ESP32-S3 的 APB 时钟**固定 80MHz**（CPU 分频不改 APB，见 Arduino 核心
//      esp32-hal-cpu.c 的 calculateApb()）。UART 波特率、LEDC(PWM) 的 20kHz 都挂在 APB 上，
//      所以降频只影响"代码跑多快"，不改任何外设时基；millis()/delay() 走 systimer + FreeRTOS
//      tick，同样与 CPU 频率无关 → 所有 ms 级时序（触发脉冲宽度、出牌动作、光电门去抖）不变。
//   ③ 电机驱动待机：TB6612 的 STBY 拉低 = 输出关断（待机电流 µA 级），见 hardware.cpp。
#define SUB_LOOP_DELAY_MS 1   // 主循环空转节拍（ms）；0 = 不延时（恢复原来的全速空转）
#define SUB_CPU_MHZ       80  // 运行主频（MHz）；0 = 不动（保持开发环境默认 240MHz）
                              // 若降频后 USB CDC 日志异常，把它改回 0（UART0/CH340 不受影响）
