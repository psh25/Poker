/**
 * BLE 通信实现（Nordic UART Service，参考讲义 2.3.2）
 * - RX 特征（6E400002）：小程序/手机写入 → 字节放入 xBluetoothRxQueue，
 *   由 vBluetoothTask 用与板间同一套帧解析器解析成主机命令。
 * - TX 特征（6E400003）：host_send 组装好帧后 notify 给小程序。
 */
#include <Arduino.h>
#include <string.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

#include "ble_comms.h"
#include "itc.h"

static BLEServer *s_server = NULL;
static BLECharacteristic *s_txChar = NULL;
static bool s_connected = false;

class BleServerCallbacks : public BLEServerCallbacks {
    void onConnect(BLEServer *server) override {
        s_connected = true;
        Serial.println("[BLE] connected");
    }
    void onDisconnect(BLEServer *server) override {
        s_connected = false;
        Serial.println("[BLE] disconnected, advertising again");
        server->startAdvertising();
    }
};

class BleRxCallbacks : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *ch) override {
        std::string value = ch->getValue();
        if (value.empty()) return;
        Serial.printf("[BLE] RX %u bytes\n", (unsigned)value.size());

        // 放入主机 RX 队列，由 vBluetoothTask 解析（不阻塞 BLE 回调）
        const uint8_t *p = (const uint8_t *)value.data();
        size_t left = value.size();
        while (left > 0) {
            bt_msg_t m;
            size_t n = (left < sizeof(m.data)) ? left : sizeof(m.data);
            memcpy(m.data, p, n);
            m.len = (uint8_t)n;
            if (xQueueSend(xBluetoothRxQueue, &m, 0) != pdPASS) break;  // 队列满丢弃
            p += n;
            left -= n;
        }
    }
};

void ble_init(void) {
    BLEDevice::init(BLE_DEVICE_NAME);
    s_server = BLEDevice::createServer();
    s_server->setCallbacks(new BleServerCallbacks());

    BLEService *svc = s_server->createService(BLE_SERVICE_UUID);
    s_txChar = svc->createCharacteristic(BLE_CHAR_TX_UUID,
                                         BLECharacteristic::PROPERTY_NOTIFY);
    s_txChar->addDescriptor(new BLE2902());
    BLECharacteristic *rxChar = svc->createCharacteristic(BLE_CHAR_RX_UUID,
                                                          BLECharacteristic::PROPERTY_WRITE);
    rxChar->setCallbacks(new BleRxCallbacks());

    svc->start();
    s_server->getAdvertising()->start();
    Serial.printf("[BLE] init '%s' service=%s\n", BLE_DEVICE_NAME, BLE_SERVICE_UUID);
}

bool ble_connected(void) {
    return s_connected;
}

bool ble_send(const uint8_t *data, size_t len) {
    if (!s_connected || !s_txChar || len == 0 || len > 512) return false;
    s_txChar->setValue((uint8_t *)data, (uint16_t)len);
    s_txChar->notify();
    return true;
}
