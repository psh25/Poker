// ============================================================
// 全局配置
// BLE 采用课程讲义《网络通信蓝牙》的 Nordic UART Service 标准：
//   Service 6E400001-B5A3-F393-E0A9-E50E24DCCA9E
//   RX 写特征 6E400002-...（手机 -> 底板）
//   TX 通知特征 6E400003-...（底板 -> 手机）
// 数据格式：二进制帧，具体见 utils/protocol.ts（可用 BLE 调试助手直接收发调试）
// ============================================================

export const BLE = {
  SERVICE_UUID: '6E400001-B5A3-F393-E0A9-E50E24DCCA9E',
  RX_UUID: '6E400002-B5A3-F393-E0A9-E50E24DCCA9E',   // 手机 -> 底板（写）
  TX_UUID: '6E400003-B5A3-F393-E0A9-E50E24DCCA9E',   // 底板 -> 手机（通知）
}

// 微信云开发：环境 ID。云环境配好之后填到这里。
// 没配置时小程序自动进入“本地模式”：只记录在本机，不报错。
export const CLOUD = {
  ENV_ID: 'cloud1-d7gr5mffo24846ba1',   // 云开发环境 ID
  COLLECTION: 'games',  // 牌局集合
}

// 发牌模式：完整发牌计划由小程序下发，sequence 中每个数字是目标牌堆（0-based）。
export interface ModeDef {
  id: number
  name: string
  desc: string
  totalCards: number
  sequence: number[]
}

function repeatPiles(pattern: number[], cards: number): number[] {
  const out: number[] = []
  while (out.length < cards) out.push(pattern[out.length % pattern.length])
  return out
}

const DOUDIZHU_SEQUENCE = repeatPiles([0, 1, 2], 51).concat([3, 3, 3])
const TEXAS_SEQUENCE = repeatPiles([0, 1, 2, 3], 8)
const BRIDGE_SEQUENCE = repeatPiles([0, 1, 2, 3], 52)

export const MODES: ModeDef[] = [
  { id: 0, name: '斗地主', desc: '3 家各 17 张 + 3 张底牌', totalCards: 54, sequence: DOUDIZHU_SEQUENCE },
  { id: 1, name: '德州扑克', desc: '4 家各 2 张底牌', totalCards: 8, sequence: TEXAS_SEQUENCE },
  { id: 2, name: '桥牌', desc: '4 家各 13 张', totalCards: 52, sequence: BRIDGE_SEQUENCE },
  { id: 3, name: '自定义', desc: '默认每堆 1 张，可编辑顺序', totalCards: 4, sequence: [0, 1, 2, 3] },
]