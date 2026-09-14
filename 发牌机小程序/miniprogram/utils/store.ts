import { Card, keyOf, newRemainMap, fullDeck } from './cards'
import { MODES } from '../config/index'

export interface CardDealt { s: number; r: number; pile: number; idx: number }
export interface PlayedRecord { s: number; r: number; p: number; ts: number }
export interface GameRecord {
  id: string
  mode: number
  modeName: string
  startTime: number
  endTime?: number
  phase: 'dealing' | 'playing' | 'ended'
  deck: CardDealt[]        // 整副牌顺序（含堆号）
  piles: Card[][]          // 各堆的牌（按堆号）
  played: PlayedRecord[]   // 回收识别出的已打出的牌
  roomCode?: string        // 联机房间码（现场开房生成）
  open?: boolean           // 房间是否开放（线上玩家可加入）
  members?: number[]       // 房间成员（已认领的堆号，含现场自己的堆）
}

const STORAGE_KEY = 'dealer_games_v1'
const CURRENT_KEY = 'dealer_current_game'

function shuffleDeck(): Card[] {
  const a = fullDeck()
  for (let i = a.length - 1; i > 0; i--) {
    const j = Math.floor(Math.random() * (i + 1))
    const t = a[i]; a[i] = a[j]; a[j] = t
  }
  return a
}

function genId(): string {
  return Date.now().toString(36) + Math.random().toString(36).slice(2, 7)
}

class Store {
  // 开发者工具 TS 编译兼容：类属性只写纯类型声明（不加 declare、不初始化），赋值放构造函数
  current: GameRecord | null

  constructor() {
    this.current = null
  }

  load(): void {
    try {
      const g = wx.getStorageSync(CURRENT_KEY)
      if (g && g.id) this.current = g as GameRecord
    } catch (e) { /* ignore */ }
  }

  private persist(): void {
    try { wx.setStorageSync(CURRENT_KEY, this.current) } catch (e) { /* ignore */ }
  }

  startGame(modeId: number): GameRecord {
    const mode = MODES.find((m) => m.id === modeId) || MODES[0]
    this.current = {
      id: genId(),
      mode: mode.id,
      modeName: mode.name,
      startTime: Date.now(),
      phase: 'dealing',
      deck: [],
      piles: [],
      played: [],
    }
    this.persist()
    return this.current
  }

  onCard(c: CardDealt): void {
    if (!this.current || this.current.phase === 'ended') return
    this.current.deck.push(c)
    while (this.current.piles.length <= c.pile) this.current.piles.push([])
    this.current.piles[c.pile].push({ s: c.s, r: c.r })
    this.persist()
  }

  onPlayed(p: PlayedRecord): void {
    if (!this.current) return
    this.current.played.push(p)
    if (this.current.phase === 'dealing') this.current.phase = 'playing'
    this.persist()
  }

  finishDeal(): void {
    if (!this.current) return
    this.current.phase = 'playing'
    this.persist()
  }

  // 现场开房：设置并保存房间码
  setRoom(code: string, open: boolean): void {
    if (!this.current) return
    this.current.roomCode = code
    this.current.open = open
    this.persist()
  }

  // 设置房间成员（现场自己认领的堆号 + 之后加入的）
  setMembers(list: number[]): void {
    if (!this.current) return
    this.current.members = list
    this.persist()
  }

  // 线上玩家加入：把云端牌局复制为本机“当前牌局”（用于看自己的牌/出牌）
  adoptGame(game: any): void {
    this.current = {
      id: String(game.id),
      mode: typeof game.mode === 'number' ? game.mode : 0,
      modeName: game.modeName || '',
      startTime: game.startTime || Date.now(),
      phase: (game.phase === 'dealing' || game.phase === 'playing' || game.phase === 'ended') ? game.phase : 'playing',
      deck: Array.isArray(game.deck) ? game.deck : [],
      piles: Array.isArray(game.piles) ? game.piles : [],
      played: Array.isArray(game.played) ? game.played : [],
      roomCode: game.roomCode,
      open: !!game.open,
    }
    this.persist()
  }

  endGame(): GameRecord | null {
    const g = this.current
    if (g) {
      g.endTime = Date.now()
      g.phase = 'ended'
      this.persist()
      // 追加到历史
      const list: GameRecord[] = wx.getStorageSync(STORAGE_KEY) || []
      list.unshift(g)
      if (list.length > 50) list.length = 50
      wx.setStorageSync(STORAGE_KEY, list)
      this.current = null
      this.persist()
      return g
    }
    return null
  }

  history(): GameRecord[] {
    try { return wx.getStorageSync(STORAGE_KEY) || [] } catch (e) { return [] }
  }

  // 记牌：剩余 = 整副牌 54 张 - 已打出（发出的牌还在各玩家手里，不算“打过”）
  remainMap(): { [key: string]: number } {
    const m = newRemainMap()
    if (!this.current) return m
    for (const p of this.current.played) {
      const k = keyOf({ s: p.s, r: p.r })
      if (m[k] !== undefined && m[k] > 0) m[k]--
    }
    return m
  }

  // 无板调试：随机生成一整副斗地主（0/1/2 三堆各 17 张，堆 3 = 3 张底牌）
  startDemoDoudizhu(): GameRecord {
    const d = shuffleDeck()
    const piles: Card[][] = [[], [], []]
    const deck: CardDealt[] = []
    for (let i = 0; i < 54; i++) {
      const pile = i < 51 ? i % 3 : 3
      if (i < 51) piles[i % 3].push(d[i])
      deck.push({ s: d[i].s, r: d[i].r, pile, idx: i })
    }
    piles.push(d.slice(51))   // 底牌堆 = 3
    this.current = {
      id: genId(),
      mode: 0,
      modeName: '斗地主',
      startTime: Date.now(),
      phase: 'playing',
      deck,
      piles,
      played: [],
    }
    this.persist()
    return this.current
  }

  // 玩家 p 的手牌（记牌用：当前牌局中分到该堆的牌，减去已打出）
  playerHand(p: number): Card[] {
    if (!this.current || !this.current.piles[p]) return []
    const hand: Card[] = this.current.piles[p].map((c) => ({ s: c.s, r: c.r }))
    for (const pl of this.current.played) {
      if (pl.p !== p) continue
      const i = hand.findIndex((c) => c.s === pl.s && c.r === pl.r)
      if (i >= 0) hand.splice(i, 1)
    }
    return hand
  }
}

export const store = new Store()

// 跨 tab 页跳转的“页签”信号：首页点“去记牌”→ switchTab 到“牌局”页 → 该页 onShow 消费
let s_pendingSegment: 'record' | 'count' | 'online' | 'ai' | null = null
export function requestSegment(seg: 'record' | 'count' | 'online' | 'ai'): void {
  s_pendingSegment = seg
}
export function takePendingSegment(): 'record' | 'count' | 'online' | 'ai' | null {
  const seg = s_pendingSegment
  s_pendingSegment = null
  return seg
}

// 已应用的云端事件(move)id：轮询/实时 watch/本机推送 之间去重
function appliedMovesKey(gameId: string): string {
  return 'applied_moves_' + gameId
}
export function isMoveApplied(gameId: string, id: string): boolean {
  if (!id) return false
  try {
    const arr: string[] = wx.getStorageSync(appliedMovesKey(gameId)) || []
    return arr.indexOf(id) >= 0
  } catch (e) { return false }
}
export function markMoveApplied(gameId: string, id: string): void {
  if (!id) return
  try {
    const arr: string[] = wx.getStorageSync(appliedMovesKey(gameId)) || []
    if (arr.indexOf(id) < 0) {
      arr.push(id)
      if (arr.length > 300) arr.splice(0, arr.length - 300)
      wx.setStorageSync(appliedMovesKey(gameId), arr)
    }
  } catch (e) { /* ignore */ }
}
