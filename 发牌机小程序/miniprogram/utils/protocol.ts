// ============================================================
// 底板 ⇄ 小程序 BLE 协议（二进制帧版）
// 帧：0xA5 | type | len | data(0..31) | crc8(type+len+data) | 0xAA
// 与底板固件 BottomBoard 主机层（tasks.cpp host_*）及
// docs/board_protocol.md §10 一致。
//
// 手机 → 底板命令：
//   0x01 发牌(仅IDLE且已确认)  0x02 停机  0x03 查状态(回0x91)
//   0x04 子板自检(转发)        0x05 复位
//   0x10 选方案 data=[idx0..3] 0x11 确认方案
//   0x14 发牌计划开始 [scheme,totalLo,totalHi,pileCount]
//   0x15 发牌计划分块 [offset,deckIndex...]  0x16 提交计划
//   0x13 线上出牌文本 data=UTF-8(≤31B) → 底板屏幕
// 底板 → 手机事件：
//   0x90 回执 data=[原type,原data...]
//   0x91 状态 data=[state,scheme,confirmed]
//   0x92 发牌 data=[idxLo,idxHi,pile,suit,rank,simulated]
//   0x93 整轮发完 data=[totalLo,totalHi]
//   0x94 记牌 data=[p,suit,rank]
// ============================================================

export const HOST_CMD = {
  DEAL: 0x01,
  STOP: 0x02,
  STATUS: 0x03,
  SELF_TEST: 0x04,
  RESET: 0x05,
  SELECT_SCHEME: 0x10,
  CONFIRM: 0x11,
  PLAY_TEXT: 0x13,
  PLAN_BEGIN: 0x14,
  PLAN_CHUNK: 0x15,
  PLAN_COMMIT: 0x16,
} as const

export const HOST_EVT = {
  ACK: 0x90,
  STATE: 0x91,
  CARD: 0x92,
  DEAL_DONE: 0x93,
  PLAYED: 0x94,
} as const

// 底板状态机只有三态（BottomBoard/include/state_machine.h 的 system_state_t）：
//   0 = IDLE（待命）/ 1 = DEALING（发牌中）/ 2 = GAME_ACTIVE（牌局中）
// 以前这里多列了一个"牌局结束"，固件并没有这个状态，0x91 永远不会报 3。
// 遇到表外的值由调用方兜底显示"状态N"。
export const STATE_NAMES = ['待命', '发牌中', '牌局中']

// CRC-8（多项式 0x07，初值 0x00，与固件 proto_crc8 一致）
export function crc8(buf: number[]): number {
  let crc = 0
  for (let i = 0; i < buf.length; i++) {
    crc ^= buf[i]
    for (let b = 0; b < 8; b++) {
      crc = (crc & 0x80) ? (((crc << 1) ^ 0x07) & 0xff) : ((crc << 1) & 0xff)
    }
  }
  return crc
}

// 组装一帧（含帧头/CRC/帧尾）
export function encodeFrame(type: number, data: number[] = []): number[] {
  const head = [0xA5, type & 0xff, data.length & 0xff]
  const frame = head.concat(data)
  frame.push(crc8(frame.slice(1)))
  frame.push(0xAA)
  return frame
}

export function frameHex(frame: number[]): string {
  let s = ''
  for (const b of frame) s += (b < 16 ? '0' : '') + b.toString(16).toUpperCase()
  return s
}

// UTF-8 编码（微信 JS 环境不保证 TextEncoder，手写）
export function utf8Bytes(s: string): number[] {
  const out: number[] = []
  for (let i = 0; i < s.length; i++) {
    let code = s.charCodeAt(i)
    if (code < 0x80) {
      out.push(code)
    } else if (code < 0x800) {
      out.push(0xc0 | (code >> 6), 0x80 | (code & 0x3f))
    } else if (code >= 0xd800 && code <= 0xdbff && i + 1 < s.length) {
      const lo = s.charCodeAt(i + 1)
      if (lo >= 0xdc00 && lo <= 0xdfff) {
        code = 0x10000 + ((code - 0xd800) << 10) + (lo - 0xdc00)
        out.push(0xf0 | (code >> 18), 0x80 | ((code >> 12) & 0x3f),
                 0x80 | ((code >> 6) & 0x3f), 0x80 | (code & 0x3f))
        i++
      } else {
        out.push(0xef, 0xbf, 0xbd)   // 孤立高位代理 → U+FFFD
      }
    } else if (code < 0x10000) {
      out.push(0xe0 | (code >> 12), 0x80 | ((code >> 6) & 0x3f), 0x80 | (code & 0x3f))
    }
  }
  return out
}

// 文本 → payload（截断到 ≤ maxBytes 个 UTF-8 字节，不截半个字符）
export function textPayload(s: string, maxBytes = 31): number[] {
  if (utf8Bytes(s).length <= maxBytes) return utf8Bytes(s)
  let out = ''
  for (let i = 0; i < s.length; i++) {
    const t = out + s[i]
    if (utf8Bytes(t).length > maxBytes) break
    out = t
  }
  return utf8Bytes(out)
}

// ---- 事件解码 ----
export interface HostCard { type: 'card'; idx: number; pile: number; s: number; r: number; simulated: boolean }
export interface HostPlayed { type: 'played'; p: number; s: number; r: number }
export interface HostState { type: 'state'; state: number; scheme: number; confirmed: boolean }
export interface HostDealDone { type: 'deal_done'; total: number }
export interface HostAck { type: 'ack'; origType: number }
export interface HostOther { type: 'other'; evtType: number; data: number[] }

export type HostEvent = HostCard | HostPlayed | HostState | HostDealDone | HostAck | HostOther

export function decodeEvent(evtType: number, data: number[]): HostEvent {
  switch (evtType) {
    case HOST_EVT.CARD: {
      // 新固件: [idxLo, idxHi, pile, suit, rank, simulated]；兼容旧 4/5 字节格式。
      if (data.length >= 5) {
        return {
          type: 'card', idx: data[0] + data[1] * 256, pile: data[2], s: data[3], r: data[4],
          simulated: data.length >= 6 && data[5] === 1,
        }
      }
      return {
        type: 'card',
        idx: data.length > 0 ? data[0] : 0,
        pile: data.length > 1 ? data[1] : 0,
        s: data.length > 2 ? data[2] : 0,
        r: data.length > 3 ? data[3] : 0,
        simulated: false,
      }
    }
    case HOST_EVT.DEAL_DONE: {
      const total = data.length >= 2 ? data[0] + data[1] * 256 : (data.length > 0 ? data[0] : 0)
      return { type: 'deal_done', total }
    }
    case HOST_EVT.PLAYED:
      return {
        type: 'played',
        p: data.length > 0 ? data[0] : 0,
        s: data.length > 1 ? data[1] : 0xff,
        r: data.length > 2 ? data[2] : 0xff,
      }
    case HOST_EVT.STATE:
      return {
        type: 'state',
        state: data.length > 0 ? data[0] : 0,
        scheme: data.length > 1 ? data[1] : 0,
        confirmed: data.length > 2 ? !!data[2] : false,
      }
    case HOST_EVT.ACK:
      return { type: 'ack', origType: data.length > 0 ? data[0] : 0 }
    default:
      return { type: 'other', evtType, data }
  }
}
