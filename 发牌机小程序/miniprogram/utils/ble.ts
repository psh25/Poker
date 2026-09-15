// ============================================================
// BLE 管理器：封装微信小程序蓝牙 API，二进制帧协议
// 连接对象是底板 ESP32（Nordic UART Service，设备名 DealerBot）
// 收发都是 0xA5|type|len|data|crc8|0xAA 帧（见 utils/protocol.ts）
// ============================================================
import { BLE } from '../config/index'
import { HOST_CMD, crc8, encodeFrame, frameHex } from './protocol'

export type BleConnState = 'idle' | 'scanning' | 'connecting' | 'connected' | 'error'

interface FoundDevice {
  deviceId: string
  name: string
  rssi: number
  matched: boolean
}

declare function setTimeout(handler: () => void, timeout?: number): number

// 每次 BLE write 最大字节（Android 默认 MTU=23 → 20 字节；不做 MTU 协商）
const WRITE_CHUNK = 20

// 流式帧解析：0xA5 | type | len | data | crc8 | 0xAA（可跨多次 notify 重组）
class FrameParser {
  // 注意：开发者工具用 Babel 剥 TS 类型，类属性只写“纯类型声明 + 构造函数赋值”：
  // 不要写 “state = 0” 这种类字段初始化；也不要加 declare（工具的 Babel TS 插件默认不允许 declare 类字段）。
  onFrame: ((type: number, data: number[]) => void) | undefined
  private state: number        // 0 idle / 1 type / 2 len / 3 data / 4 crc / 5 tail
  private type: number
  private len: number
  private data: number[]
  private idx: number

  constructor() {
    this.onFrame = undefined
    this.state = 0
    this.type = 0
    this.len = 0
    this.data = []
    this.idx = 0
  }

  feed(bytes: Uint8Array) {
    for (let i = 0; i < bytes.length; i++) this.feedByte(bytes[i])
  }

  private feedByte(b: number) {
    switch (this.state) {
      case 0:
        if (b === 0xA5) this.state = 1
        break
      case 1:
        this.type = b
        this.state = 2
        break
      case 2:
        if (b > 63) { this.state = 0; break }   // 超长帧丢弃
        this.len = b
        this.idx = 0
        this.data = []
        this.state = this.len ? 3 : 4
        break
      case 3:
        this.data[this.idx++] = b
        if (this.idx >= this.len) this.state = 4
        break
      case 4: {
        const crc = crc8([this.type, this.len].concat(this.data))
        this.state = (crc === b) ? 5 : 0
        break
      }
      case 5:
        if (b === 0xAA && this.onFrame) this.onFrame(this.type, this.data)
        this.state = 0
        break
      default:
        this.state = 0
        break
    }
  }
}

class BleManager {
  state: BleConnState
  deviceId: string
  serviceId: string
  txCharId: string      // 底板 -> 手机（通知）
  rxCharId: string      // 手机 -> 底板（写）
  foundDevices: FoundDevice[]
  onFrame: ((type: number, data: number[]) => void) | undefined
  onStateChange: ((st: BleConnState) => void) | undefined
  private parser: FrameParser

  constructor() {
    this.state = 'idle'
    this.deviceId = ''
    this.serviceId = ''
    this.txCharId = ''
    this.rxCharId = ''
    this.foundDevices = []
    this.onFrame = undefined
    this.onStateChange = undefined
    this.parser = new FrameParser()
  }

  private setState(st: BleConnState) {
    this.state = st
    if (this.onStateChange) this.onStateChange(st)
  }

  ensureAdapter(): Promise<void> {
    return new Promise((resolve) => {
      wx.openBluetoothAdapter({
        success: () => resolve(),
        fail: () => resolve(),   // 可能已打开
      })
    })
  }

  scan(timeoutMs = 15000): Promise<FoundDevice[]> {
    this.foundDevices = []
    return new Promise((resolve) => {
      this.ensureAdapter().then(() => {
        // 连续点扫描时先移除旧监听，避免一个设备被重复注册多次。
        try { wx.offBluetoothDeviceFound() } catch (e) { /* ignore */ }
        wx.onBluetoothDeviceFound((res) => {
          for (const d of res.devices) {
            if (!d.deviceId) continue
            const localName = d.name || (d as any).localName || ''
            const name = localName.toUpperCase()
            const serviceMatched = ((d.advertisServiceUUIDs || []) as string[]).some((u) =>
              u.toUpperCase().indexOf(BLE.SERVICE_UUID.replace(/-/g, '').slice(0, 8)) >= 0)
            const matched = name.indexOf('DEALERBOT') >= 0 || name.indexOf('ESP32') >= 0 || serviceMatched
            const idx = this.foundDevices.findIndex((f) => f.deviceId === d.deviceId)
            const item: FoundDevice = {
              deviceId: d.deviceId,
              name: localName || '(无名称设备)',
              rssi: d.RSSI || -100,
              matched,
            }
            if (idx >= 0) this.foundDevices[idx] = item
            else this.foundDevices.push(item)
          }
        })
        wx.startBluetoothDevicesDiscovery({
          allowDuplicatesKey: true,
          success: () => this.setState('scanning'),
          fail: (err) => {
            console.error('[BLE] startBluetoothDevicesDiscovery 失败:', err)
            this.setState('error')
          },
        })
        setTimeout(() => {
          wx.stopBluetoothDevicesDiscovery({})
          this.foundDevices.sort((a, b) =>
            Number(b.matched) - Number(a.matched) || b.rssi - a.rssi)
          resolve(this.foundDevices)
        }, timeoutMs)
      }).catch(() => this.setState('error'))
    })
  }

  connect(deviceId: string): Promise<void> {
    return new Promise((resolve, reject) => {
      this.deviceId = deviceId
      this.setState('connecting')
      // 部分安卓机在“扫描进行中”直接连接会失败：先停止扫描
      wx.stopBluetoothDevicesDiscovery({ complete: () => {} })
      wx.createBLEConnection({
        deviceId,
        timeout: 10000,
        success: () => this.discover(deviceId).then(() => resolve()).catch(reject),
        fail: (err) => {
          console.error('[BLE] createBLEConnection 失败:', err)
          this.setState('error')
          reject(err)
        },
      })
    })
  }

  private discover(deviceId: string): Promise<void> {
    return new Promise((resolve, reject) => {
      wx.getBLEDeviceServices({
        deviceId,
        success: (res) => {
          const svc = res.services.find((s) => s.uuid.toUpperCase() === BLE.SERVICE_UUID.toUpperCase())
          if (!svc) {
            const list = (res.services || []).map((x) => x.uuid).join(',')
            console.error('[BLE] 未找到 NUS Service，实际服务:', list)
            this.setState('error')
            reject(new Error('未找到 NUS Service（' + list + '）'))
            return
          }
          this.serviceId = svc.uuid
          wx.getBLEDeviceCharacteristics({
            deviceId,
            serviceId: svc.uuid,
            success: (r) => {
              const tx = r.characteristics.find((c) => c.uuid.toUpperCase() === BLE.TX_UUID.toUpperCase())
              const rx = r.characteristics.find((c) => c.uuid.toUpperCase() === BLE.RX_UUID.toUpperCase())
              if (!tx || !rx) {
                const list = (r.characteristics || []).map((x) => x.uuid).join(',')
                console.error('[BLE] 未找到 RX/TX 特征，实际特征:', list)
                this.setState('error')
                reject(new Error('未找到 RX/TX 特征（' + list + '）'))
                return
              }
              this.txCharId = tx.uuid
              this.rxCharId = rx.uuid
              this.parser = new FrameParser()
              this.parser.onFrame = (t, d) => {
                if (this.onFrame) {
                  console.log('[BLE] RX frame type=0x' + t.toString(16) + ' ' + frameHex(encodeFrame(t, d)))
                  this.onFrame(t, d)
                }
              }
              this.subscribeNotify(deviceId, this.serviceId, tx.uuid).then(() => {
                this.setState('connected')
                // 连接后握手：查询一次底板状态（回 0x91）
                this.writeFrame(HOST_CMD.STATUS).catch(() => {})
                resolve()
              }).catch(reject)
            },
            fail: (err) => { console.error('[BLE] getBLEDeviceCharacteristics 失败:', err); reject(err) },
          })
        },
        fail: (err) => { console.error('[BLE] getBLEDeviceServices 失败:', err); reject(err) },
      })
    })
  }

  private subscribeNotify(deviceId: string, serviceId: string, charId: string): Promise<void> {
    return new Promise((resolve, reject) => {
      wx.notifyBLECharacteristicValueChange({
        deviceId, serviceId, characteristicId: charId, state: true,
        success: () => {
          wx.onBLECharacteristicValueChange((res) => {
            if (res.characteristicId.toUpperCase() === charId.toUpperCase()) {
              this.parser.feed(new Uint8Array(res.value))
            }
          })
          resolve()
        },
        fail: (err) => { console.error('[BLE] notifyBLECharacteristicValueChange 失败:', err); reject(err) },
      })
    })
  }

  // 发送一帧：整帧可能 >20B，按 20B 分片依次写入（底板解析器可跨片重组）
  writeFrame(type: number, data: number[] = []): Promise<void> {
    if (this.state !== 'connected') return Promise.reject(new Error('未连接'))
    const frame = encodeFrame(type, data)
    console.log('[BLE] TX ' + frameHex(frame))
    return this.writeChunks(frame, 0)
  }

  private writeChunks(frame: number[], offset: number): Promise<void> {
    if (offset >= frame.length) return Promise.resolve()
    const end = Math.min(offset + WRITE_CHUNK, frame.length)
    const chunk = frame.slice(offset, end)
    return new Promise<void>((resolve, reject) => {
      wx.writeBLECharacteristicValue({
        deviceId: this.deviceId,
        serviceId: this.serviceId,
        characteristicId: this.rxCharId,
        value: this.bytesToAb(chunk),
        success: () => this.writeChunks(frame, end).then(resolve).catch(reject),
        fail: reject,
      })
    })
  }

  private bytesToAb(bytes: number[]): ArrayBuffer {
    const buf = new ArrayBuffer(bytes.length)
    const view = new Uint8Array(buf)
    for (let i = 0; i < bytes.length; i++) view[i] = bytes[i]
    return buf
  }

  disconnect(): void {
    if (this.deviceId) {
      wx.closeBLEConnection({ deviceId: this.deviceId, fail: () => {} })
    }
    this.deviceId = ''
    this.serviceId = ''
    this.txCharId = ''
    this.rxCharId = ''
    this.setState('idle')
  }
}

export const bleManager = new BleManager()
