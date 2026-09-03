#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/**
 * BLE 通信（参考重要信息讲义《3-网络通信蓝牙》：Nordic UART Service 结构）
 * 手机/小程序 ↔ 底板：RX 特征写入命令帧，TX 特征 notify 回执/状态帧；
 * 帧格式与板间通信一致（0xA5 | type | len | data | crc8 | 0xAA），见 board_protocol.md。
 */

#define BLE_DEVICE_NAME   "DealerBot"
#define BLE_SERVICE_UUID  "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define BLE_CHAR_RX_UUID  "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"  // 手机 → ESP32（write）
#define BLE_CHAR_TX_UUID  "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"  // ESP32 → 手机（notify）

void ble_init(void);                       // 创建 Server / Service / 特征并开始广播
bool ble_connected(void);
bool ble_send(const uint8_t *data, size_t len);   // 已连接才真正 notify
