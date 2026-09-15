import { store, CardDealt, takePendingSegment, isMoveApplied, markMoveApplied } from '../../utils/store'
import { Card, cardName, cardIsRed, sortDesc, RANK_NAMES, SUIT_SYMBOLS, fullDeck } from '../../utils/cards'
import { analyze, canBeat, leadHand, respondHand, comboName, Combo } from '../../utils/doudizhu'
import { bleManager } from '../../utils/ble'
import { HOST_CMD, textPayload } from '../../utils/protocol'
import { on, off } from '../../utils/bus'
import { cloudReady, saveGameToCloud, pushMoveToCloud, fetchMovesFromCloud, joinRoom, getGameFromCloud, watchGameMoves } from '../../utils/cloud'

declare function setInterval(handler: () => void, timeout?: number): number
declare function clearInterval(handle: number): void
declare function setTimeout(handler: () => void, timeout?: number): number
declare function clearTimeout(handle: number): void

// AI 陪打对局状态（本地演示，纯前端，不需要蓝牙）
interface AiDemoState {
  hands: Card[][]           // 0=你（地主）, 1=AI甲, 2=AI乙
  bottom: Card[]            // 3 张底牌（归地主）
  turn: number              // 当前轮到谁
  last: { player: number; combo: Combo } | null   // 上一手
  passCount: number
  winner: number            // -1 未结束
  log: string[]
}

function shuffle<T>(arr: T[]): T[] {
  const a = arr.slice()
  for (let i = a.length - 1; i > 0; i--) {
    const j = Math.floor(Math.random() * (i + 1))
    const t = a[i]; a[i] = a[j]; a[j] = t
  }
  return a
}

function playerName(p: number): string {
  return p === 0 ? '你' : (p === 1 ? 'AI甲' : 'AI乙')
}

// 线上出牌 → 底板屏幕的短文本（斗地主能识别牌型就显示牌型名，否则只报张数）
function playScreenText(cards: Card[]): string {
  if (!cards.length) return '出牌'
  const c = analyze(cards)
  if (c) return comboName(c)
  return '线上出牌 ' + cards.length + ' 张'
}

Page({
  data: {
    segment: 'record' as 'record' | 'count' | 'online' | 'ai',
    modeName: '',
    phase: '',
    dealCount: 0,
    lastCard: '',
    playedCount: 0,
    playedRecent: [] as { s: number; r: number; name: string; p: number }[],
    remainList: [] as { label: string; count: number }[],
    remainGroups: [] as { label: string; count: number }[][],
    remainTotal: 0,
    myPile: -1,
    myPileInput: '',
    myHand: [] as { s: number; r: number; name: string; red: boolean; selected: boolean; rankName?: string; suitSym?: string }[],
    selCount: 0,
    connected: false,
    cloudOk: false,
    syncTip: '',
    online: {
      role: '' as '' | 'host' | 'guest',
      roomCode: '',
      roomInput: '',
      pileInput: '',
      tip: '',
      members: [] as number[],
      feed: [] as { id: string; who: string; text: string }[],
    },
    ai: {
      running: false,
      winner: -1,
      turn: -1,
      hand: [] as { s: number; r: number; name: string; red: boolean; selected: boolean }[],
      aiCounts: [0, 0, 0] as number[],
      bottom: [] as { s: number; r: number; name: string; red: boolean }[],
      lastPlayName: '',
      lastPlayCards: [] as { s: number; r: number; name: string; red: boolean }[],
      selName: '',
      statusText: '',
      log: [] as string[],
    },
  },

  _onDeal: null as any,
  _onPlayed: null as any,
  _onDealDone: null as any,
  _movesTimer: 0 as any,
  _lastPlayedSave: 0,
  _watchClose: null as (() => void) | null,
  _ai: null as AiDemoState | null,
  _aiSel: null as string[] | null,
  _aiTimer: 0 as any,

  onLoad(options: any) {
    this.setData({ cloudOk: cloudReady() })
    bleManager.onStateChange = (st) => this.setData({ connected: st === 'connected' })
    // 支持从首页“去记牌”跳入并直接停到“记牌”页签
    const seg = options && options.seg
    if (seg === 'record' || seg === 'count' || seg === 'online' || seg === 'ai') {
      this.setData({ segment: seg })
    }
  },

  onShow() {
    // 首页“去记牌”→ switchTab 到这里：消费信号并切到对应页签
    const seg = takePendingSegment()
    if (seg) this.setData({ segment: seg })
    this._onDeal = this.handleDeal.bind(this)
    this._onPlayed = this.handlePlayed.bind(this)
    this._onDealDone = this.handleDealDone.bind(this)
    on('deal', this._onDeal)
    on('played', this._onPlayed)
    on('deal_done', this._onDealDone)
    this.refresh()
    if (cloudReady()) this.startMovePoll()
    if (this.data.online.role === 'host' || this.data.online.role === 'guest') this.startOnlineWatch()
    this.resumeAi()
  },

  onHide() {
    if (this._onDeal) off('deal', this._onDeal)
    if (this._onPlayed) off('played', this._onPlayed)
    if (this._onDealDone) off('deal_done', this._onDealDone)
    this.stopMovePoll()
    this.stopOnlineWatch()
    if (this._aiTimer) { clearTimeout(this._aiTimer); this._aiTimer = 0 }
  },

  // ---- 云：拉取线上出牌并转发给底板（现场手机） ----
  startMovePoll() {
    this.stopMovePoll()
    this._movesTimer = setInterval(() => { this.pollMoves() }, 2000)
    this.pollMoves()
  },

  stopMovePoll() {
    if (this._movesTimer) { clearInterval(this._movesTimer); this._movesTimer = 0 }
  },

  async pollMoves() {
    const g = store.current
    if (!g || !this.data.cloudOk) return
    const role = this.data.online.role
    if (role !== 'host' && role !== 'guest') return   // 现场收线上出牌；线上玩家收现场记牌/成员
    const res = await fetchMovesFromCloud(g.id)
    if (!res.ok || !res.data) return
    await this.applyCloudMoves(g, res.data as any[])
  },

  // 处理云端事件（按 move.id 去重）：
  //   join    有人加入（host 更新成员列表）
  //   play    线上玩家出牌 → 记牌 + （host 已连接）送底板屏幕
  //   played  现场编码器记牌 → 记牌（线上玩家可见）
  async applyCloudMoves(g: any, list: any[]): Promise<void> {
    const role = this.data.online.role
    for (let i = 0; i < list.length; i++) {
      const mv = list[i]
      if (!mv || typeof mv.id !== 'string') continue
      if (isMoveApplied(g.id, mv.id)) continue
      if (mv.type === 'join') {
        if (role === 'host' && typeof mv.p === 'number' && this.data.online.members.indexOf(mv.p) < 0) {
          this.setData({ 'online.members': this.data.online.members.concat(mv.p) })
          this.pushFeed(mv.id, '堆' + mv.p, '加入房间')
        }
        markMoveApplied(g.id, mv.id)
        continue
      }
      const cards = Array.isArray(mv.cards) ? (mv.cards as Card[]) : []
      const p = typeof mv.p === 'number' ? mv.p : -1
      const who = '堆' + p
      if (!cards.length) { markMoveApplied(g.id, mv.id); continue }
      for (const c of cards) {
        if (c && typeof c.s === 'number' && typeof c.r === 'number') {
          store.onPlayed({ s: c.s, r: c.r, p, ts: mv.ts || Date.now() })
        }
      }
      this.refreshPlayed()
      this.refreshRemain()
      this.refreshMyHand()
      const text = (mv.type === 'play') ? playScreenText(cards) : ('打回 ' + cards.map((c) => cardName(c)).join(' '))
      this.pushFeed(mv.id, who, text)
      // 只有现场手机把“线上出牌”显示到底板屏幕（现场记牌不上屏）
      if (mv.type === 'play' && role === 'host' && this.data.connected) {
        await bleManager.writeFrame(HOST_CMD.PLAY_TEXT, textPayload(text)).catch(() => {})
      }
      markMoveApplied(g.id, mv.id)
    }
  },

  // 出牌动态（线上页同页展示：谁出了什么）
  pushFeed(id: string, who: string, text: string) {
    const feed = this.data.online.feed.slice()
    feed.unshift({ id, who, text })
    if (feed.length > 12) feed.length = 12
    this.setData({ 'online.feed': feed })
  },

  // 实时 watch（moves 集合权限允许时即时；失败自动回退到 2s 轮询）
  startOnlineWatch() {
    this.stopOnlineWatch()
    const g = store.current
    if (!g || !cloudReady()) return
    this._watchClose = watchGameMoves(g.id, (list) => {
      this.applyCloudMoves(g, list).catch(() => {})
    })
  },

  stopOnlineWatch() {
    if (this._watchClose) {
      try { this._watchClose() } catch (e) { /* ignore */ }
      this._watchClose = null
    }
  },

  // ---- 底板消息处理 ----
  handleDeal(msg: CardDealt) {
    this.setData({
      dealCount: msg.idx + 1,
      lastCard: cardName({ s: msg.s, r: msg.r }),
      playedCount: store.current ? store.current.played.length : 0,
    })
    this.refreshRemain()
    this.refreshMyHand()
  },

  handlePlayed() {
    this.setData({ playedCount: store.current ? store.current.played.length : 0 })
    this.refreshPlayed()
    this.refreshRemain()
    this.refreshMyHand()
    // 节流同步整局到云（记牌多手机共享）
    if (this.data.cloudOk && store.current) {
      const now = Date.now()
      if (now - this._lastPlayedSave > 2000) {
        this._lastPlayedSave = now
        saveGameToCloud(store.current)
      }
    }
  },

  handleDealDone() {
    this.setData({ phase: 'playing' })
    if (this.data.cloudOk && store.current) {
      saveGameToCloud(store.current)
    }
    wx.showToast({ title: '整副牌发完', icon: 'success' })
    this.refresh()
  },

  switchSegment(e: any) {
    this.setData({ segment: e.currentTarget.dataset.seg })
    this.refresh()
  },

  refresh() {
    const g = store.current
    this.setData({
      modeName: g ? g.modeName : '',
      phase: g ? g.phase : '',
      dealCount: g ? g.deck.length : 0,
      playedCount: g ? g.played.length : 0,
      lastCard: g && g.deck.length ? cardName({ s: g.deck[g.deck.length - 1].s, r: g.deck[g.deck.length - 1].r }) : '',
    })
    this.refreshPlayed()
    this.refreshRemain()
    this.refreshMyHand()
  },

  refreshPlayed() {
    const g = store.current
    if (!g) { this.setData({ playedRecent: [] }); return }
    const recent = g.played.slice(-12).map((p) => ({
      s: p.s, r: p.r, name: cardName({ s: p.s, r: p.r }), p: p.p,
    })).reverse()
    this.setData({ playedRecent: recent })
  },

  // 记牌：本玩家(myPile)看“外面还剩什么” =
  //   整副牌 − 我自己这一堆(piles[myPile]) − 别人已打出的牌(played.p !== myPile)
  // 自己打出的牌属于自己那堆，不再重复减，所以模拟“自己出牌”不会改变“外面剩余”。
  refreshRemain() {
    const g = store.current
    const myPile = this.data.myPile
    const cnt: { [key: number]: number } = {}
    for (let r = 3; r <= 15; r++) cnt[r] = 4
    cnt[16] = 1   // 小王
    cnt[17] = 1   // 大王
    if (g) {
      if (myPile >= 0 && g.piles[myPile]) {
        for (const c of g.piles[myPile]) {
          if (cnt[c.r] !== undefined && cnt[c.r] > 0) cnt[c.r]--
        }
      }
      for (const pl of g.played) {
        if (myPile >= 0 && pl.p === myPile) continue   // 自己打的牌不减（已经减过自己这堆）
        if (cnt[pl.r] !== undefined && cnt[pl.r] > 0) cnt[pl.r]--
      }
    }
    const list: { label: string; count: number }[] = []
    for (let r = 17; r >= 3; r--) {
      list.push({ label: RANK_NAMES[r] || String(r), count: cnt[r] || 0 })
    }
    const groups: { label: string; count: number }[][] = []
    for (let i = 0; i < list.length; i += 6) groups.push(list.slice(i, i + 6))
    let total = 0
    for (const it of list) total += it.count
    this.setData({ remainList: list, remainGroups: groups, remainTotal: total })
  },

  refreshMyHand() {
    const g = store.current
    if (this.data.myPile < 0 || !g || !g.piles[this.data.myPile]) {
      this.setData({ myHand: [] })
      return
    }
    const hand = store.playerHand(this.data.myPile)
    this.setData({
      myHand: sortDesc(hand).map((c) => {
        let rankName = RANK_NAMES[c.r] || String(c.r)
        let suitSym = ''
        if (c.r === 17) { rankName = '大'; suitSym = '王' }       // 大王竖排两字
        else if (c.r === 16) { rankName = '小'; suitSym = '王' }  // 小王竖排两字
        else { suitSym = SUIT_SYMBOLS[c.s] || '' }
        return {
          s: c.s, r: c.r, name: cardName(c), red: cardIsRed(c), selected: false,
          rankName, suitSym,
        }
      }),
      selCount: 0,
    })
  },

  onMyPileInput(e: any) {
    this.setData({ myPileInput: e.detail.value })
  },

  applyMyPile() {
    const v = Number(this.data.myPileInput)
    if (isNaN(v) || v < 0) { wx.showToast({ title: '堆号需 >=0', icon: 'none' }); return }
    this.setData({ myPile: v })
    this.refresh()   // 重刷手牌 + 剩余（外面剩余按我的堆号重算）
    wx.showToast({ title: '已设置我的堆 ' + v, icon: 'none' })
  },

  // 无板调试：生成一整副斗地主（54 张：3 堆各 17 + 底牌3），用于练习记牌
  demoDoudizhu() {
    if (this.data.online.role !== '') {
      wx.showToast({ title: '请先在“线上”退出房间再生成测试牌局', icon: 'none' })
      return
    }
    store.startDemoDoudizhu()
    this.setData({ myPile: -1, myPileInput: '', myHand: [], playedRecent: [] })
    this.refresh()
    wx.showToast({ title: '已生成斗地主测试牌局（54 张）', icon: 'success' })
  },

  // 无板调试：模拟“我（堆 X）出牌”——按当前已选的多张一起打出（等价于一次真实出牌）
  simPlayCard() {
    const p = this.data.myPile
    if (p < 0) { wx.showToast({ title: '先设置我的堆号', icon: 'none' }); return }
    const selected = this.data.myHand.filter((c) => c.selected)
    if (!selected.length) { wx.showToast({ title: '先点选要打出的牌（叠牌或下方列表多选）', icon: 'none' }); return }
    for (const c of selected) {
      store.onPlayed({ s: c.s, r: c.r, p, ts: Date.now() })
    }
    this.refreshPlayed()
    this.refreshRemain()
    this.refreshMyHand()
    this.setData({ playedCount: store.current ? store.current.played.length : 0, selCount: 0 })
    wx.showToast({ title: '打出 ' + selected.length + ' 张（堆' + p + '）', icon: 'success' })
  },

  toggleCard(e: any) {
    const idx = Number(e.currentTarget.dataset.idx)
    const hand = this.data.myHand.slice()
    if (hand[idx]) hand[idx].selected = !hand[idx].selected
    this.setData({ myHand: hand })
  },

  // 记牌页多选：叠牌或下方列表点选/取消
  toggleMyCard(e: any) {
    const idx = Number(e.currentTarget.dataset.idx)
    const hand = this.data.myHand.slice()
    if (hand[idx]) hand[idx].selected = !hand[idx].selected
    const selCount = hand.filter((c) => c.selected).length
    this.setData({ myHand: hand, selCount })
  },

  // ---- 线上陪打：身份 / 房间 / 出牌 ----
  onRoomInput(e: any) {
    this.setData({ 'online.roomInput': e.detail.value })
  },

  onOnlinePileInput(e: any) {
    this.setData({ 'online.pileInput': e.detail.value })
  },

  // 选择身份：现场(host) / 线上玩家(guest)
  onlineChooseRole(e: any) {
    const role = e.currentTarget.dataset.role
    if (role === 'host') {
      if (bleManager.state !== 'connected') { wx.showToast({ title: '现场手机请先连接发牌机', icon: 'none' }); return }
      if (!store.current) { wx.showToast({ title: '请先完成一局发牌', icon: 'none' }); return }
      this.setData({
        online: { role: 'host', roomCode: store.current.roomCode || '', roomInput: '', pileInput: '', tip: '发完牌后点“生成房间码”开始联机', members: [], feed: [] },
      })
    } else {
      this.setData({
        online: {
          role: 'guest', roomCode: '', roomInput: '', pileInput: '',
          tip: this.data.cloudOk ? '输入现场的 6 位房间码加入' : '未配置云：本地演示模式，直接设堆号出牌（送底板屏幕）',
          members: [], feed: [],
        },
      })
    }
  },

  // 现场：生成/打开房间（现场手机先认领自己的堆号，另两人才能加入）
  async onlineHostRoom() {
    const g = store.current
    if (!g) { wx.showToast({ title: '先完成一局发牌', icon: 'none' }); return }
    if (g.phase === 'dealing') { wx.showToast({ title: '发牌还没结束', icon: 'none' }); return }
    const hostPile = this.data.myPile
    if (hostPile < 0) {
      wx.showToast({ title: '先在上方填“我的堆号（现场这台）”', icon: 'none' })
      return
    }
    const code = g.roomCode || String(Math.floor(100000 + Math.random() * 900000))
    store.setRoom(code, true)
    store.setMembers([hostPile])   // 现场认领自己的堆，别人不能抢
    let ok = true
    if (this.data.cloudOk) {
      const res = await saveGameToCloud(store.current)
      ok = res.ok
    }
    if (!ok) { wx.showToast({ title: '保存到云端失败', icon: 'none' }); return }
    // 读一次云端成员（含现场自己 + 重开时已加入的人）
    let members: number[] = [hostPile]
    if (this.data.cloudOk) {
      const rg = await getGameFromCloud(g.id)
      members = (rg.ok && rg.data && Array.isArray(rg.data.members) && rg.data.members.length) ? rg.data.members : [hostPile]
    }
    this.setData({ 'online.roomCode': code, 'online.members': members, 'online.feed': [], 'online.tip': '房间已开：' + code + '，让另两台手机加入（各选一个堆号）' })
    if (this.data.cloudOk) {
      this.startMovePoll()
      this.startOnlineWatch()
    }
    wx.showToast({ title: '房间码 ' + code, icon: 'success' })
  },

  // 现场：关闭房间
  async onlineCloseRoom() {
    const g = store.current
    if (g) { store.setRoom(g.roomCode || '', false) }
    if (this.data.cloudOk && g) { await saveGameToCloud(g).catch(() => {}) }
    this.stopOnlineWatch()
    this.setData({ 'online.roomCode': '', 'online.tip': '房间已关闭' })
  },

  // 返回重选身份：host 会先关房；guest 退出（不清云端）
  async onlineBack() {
    const role = this.data.online.role
    if (role === 'host') {
      const g = store.current
      if (g && g.roomCode) { store.setRoom(g.roomCode, false) }
      if (this.data.cloudOk && g) { await saveGameToCloud(g).catch(() => {}) }
      this.stopOnlineWatch()
    }
    this.setData({
      online: { role: '', roomCode: '', roomInput: '', pileInput: '', tip: '', members: [], feed: [] },
      myPile: -1, myHand: [],
    })
    this.refresh()
    wx.showToast({ title: '已返回，可重新选择', icon: 'none' })
  },

  // 线上玩家：退出当前房间（仍是 guest，可再输码或返回重选）
  onlineLeaveRoom() {
    this.setData({
      'online.roomCode': '',
      'online.roomInput': '',
      'online.pileInput': '',
      'online.tip': '已退出房间，可输入新房间码或返回重选',
      myPile: -1,
      myHand: [],
    })
    this.refresh()
  },

  // 线上玩家：加入房间（房间码 + 我的牌堆号；牌堆号即身份）
  async onlineJoinRoom() {
    const code = String(this.data.online.roomInput || '').trim()
    const pile = Number(this.data.online.pileInput)
    if (!code || code.length < 4 || code.length > 8 || isNaN(Number(code))) {
      wx.showToast({ title: '请输入 6 位房间码', icon: 'none' })
      return
    }
    if (!Number.isInteger(pile) || pile < 0 || pile > 20) {
      wx.showToast({ title: '请填写你的牌堆号（数字）', icon: 'none' })
      return
    }
    this.setData({ 'online.tip': '正在加入房间 ' + code + '（我是堆 ' + pile + '）…' })
    const res = await joinRoom(code, pile)
    if (!res.ok || !res.data) {
      const reason = res.error === 'pile-taken'
        ? '这个牌堆号已被别人加入，请换一个'
        : res.error === 'room-not-found' ? '房间不存在或已关闭' : '加入失败：' + (res.error || '未知')
      this.setData({ 'online.tip': reason })
      wx.showToast({ title: '加入失败', icon: 'none' })
      return
    }
    store.adoptGame(res.data)
    this.setData({
      'online.roomCode': code,
      'online.pileInput': String(pile),
      'online.tip': '已加入房间 ' + code + '，你是堆 ' + pile,
      myPile: pile,
      myPileInput: String(pile),
      myHand: [],
    })
    this.refresh()
    if (cloudReady()) {
      this.startMovePoll()
      this.startOnlineWatch()
    }
    wx.showToast({ title: '加入成功（堆 ' + pile + '）', icon: 'success' })
  },

  // 出牌：guest 推云（host 收云→送屏幕+记牌）；本地/演示直接送屏幕
  async playCards() {
    const selected = this.data.myHand.filter((c) => c.selected).map((c) => ({ s: c.s, r: c.r }))
    if (!selected.length) { wx.showToast({ title: '请先点选要出的牌', icon: 'none' }); return }
    const g = store.current
    const role = this.data.online.role
    const p = this.data.myPile
    if (role === 'guest' && this.data.cloudOk && p < 0) { wx.showToast({ title: '先设置你的堆号', icon: 'none' }); return }
    // 校验所选牌都在自己手牌里
    const owned = new Set<string>()
    for (const c of store.playerHand(p < 0 ? 0 : p)) owned.add(c.s + '-' + c.r)
    for (const c of selected) {
      if (!owned.has(c.s + '-' + c.r)) { wx.showToast({ title: '含不在你手牌里的牌', icon: 'none' }); return }
    }
    const move = { type: 'play', id: Date.now().toString(36) + Math.random().toString(36).slice(2, 6), p: p < 0 ? 0 : p, cards: selected, ts: Date.now() }
    if (role === 'guest' && this.data.cloudOk && g) {
      const res = await pushMoveToCloud(g.id, move)
      if (!res.ok) {
        wx.showToast({ title: '同步失败：' + (res.error || '云错误'), icon: 'none' })
        return
      }
      this.markPlayedLocally(move)
      markMoveApplied(g.id, move.id)
      this.pushFeed(move.id, '堆' + move.p, playScreenText(selected as Card[]))
      this.setData({ 'online.tip': '已出牌 ' + selected.length + ' 张（已同步现场）' })
      wx.showToast({ title: '已出牌，现场已收到', icon: 'success' })
    } else {
      // 本地演示 / 现场手机直接测试
      this.markPlayedLocally(move)
      if (this.data.connected) {
        await bleManager.writeFrame(HOST_CMD.PLAY_TEXT, textPayload(playScreenText(selected as Card[]))).catch(() => {})
      }
      this.pushFeed(move.id, '堆' + move.p, playScreenText(selected as Card[]))
      this.setData({ 'online.tip': '已出牌（本地演示，送底板屏幕）' })
      wx.showToast({ title: '已出牌', icon: 'success' })
    }
  },

  // 本机立即记录“线上出牌 = 已打出”
  markPlayedLocally(move: any) {
    for (const c of (move.cards || [])) {
      if (c && typeof c.s === 'number' && typeof c.r === 'number') {
        store.onPlayed({ s: c.s, r: c.r, p: move.p, ts: move.ts || Date.now() })
      }
    }
    this.refreshPlayed()
    this.refreshRemain()
    this.refreshMyHand()
  },

  // ================= AI 陪打（斗地主本地演示） =================

  aiStart() {
    if (this._aiTimer) { clearTimeout(this._aiTimer); this._aiTimer = 0 }
    const deck = shuffle(fullDeck())
    const hands: Card[][] = [[], [], []]
    for (let i = 0; i < 51; i++) hands[i % 3].push(deck[i])
    const bottom = deck.slice(51)
    hands[0] = hands[0].concat(bottom) // 你固定当地主（20 张）
    this._ai = {
      hands,
      bottom,
      turn: 0,
      last: null,
      passCount: 0,
      winner: -1,
      log: ['—— 新一局：你是地主，先出 ——'],
    }
    this._aiSel = []
    this.setData({ 'ai.running': true })
    this.aiRender()
  },

  aiRestart() {
    this.aiStart()
  },

  // 从其它页面回来时，如果正轮到 AI 就继续自动出牌
  resumeAi() {
    const a = this._ai
    if (a && a.winner < 0 && a.turn !== 0) this.scheduleAi()
  },

  scheduleAi() {
    if (this._aiTimer) { clearTimeout(this._aiTimer); this._aiTimer = 0 }
    const a = this._ai
    if (!a || a.winner >= 0 || a.turn === 0) return
    this._aiTimer = setTimeout(() => {
      this._aiTimer = 0
      const b = this._ai
      if (!b || b.winner >= 0 || b.turn === 0) return
      const p = b.turn
      let combo: Combo | null = null
      if (b.last && b.last.player !== p) combo = respondHand(b.hands[p], b.last.combo)
      else combo = leadHand(b.hands[p])
      if (combo) this.applyAiPlay(p, combo)
      else this.applyAiPass(p)
    }, 700)
  },

  applyAiPlay(player: number, combo: Combo) {
    const a = this._ai
    if (!a) return
    const keys = new Set<string>()
    for (const c of combo.cards) keys.add(c.s + '-' + c.r)
    a.hands[player] = a.hands[player].filter((c) => !keys.has(c.s + '-' + c.r))
    a.last = { player, combo }
    a.passCount = 0
    a.log.push(playerName(player) + ' 出：' + comboName(combo))
    if (!a.hands[player].length) {
      a.winner = player
      a.log.push(playerName(player) + ' 出完了，获胜！')
    } else {
      a.turn = (player + 1) % 3
    }
    this._aiSel = []
    this.aiRender()
    if (a.winner < 0 && a.turn !== 0) this.scheduleAi()
  },

  applyAiPass(player: number) {
    const a = this._ai
    if (!a) return
    a.passCount++
    a.log.push(playerName(player) + ' 要不起')
    if (a.passCount >= 2) {
      // 其余两家都不要，回到上一手出牌人重新出
      a.passCount = 0
      const lp = a.last ? a.last.player : 0
      a.last = null
      a.turn = lp
      a.log.push('—— 新一轮：' + playerName(lp) + ' 出牌 ——')
    } else {
      a.turn = (player + 1) % 3
    }
    this.aiRender()
    if (a.winner < 0 && a.turn !== 0) this.scheduleAi()
  },

  aiToggleCard(e: any) {
    const a = this._ai
    if (!a || a.winner >= 0 || a.turn !== 0) return
    const c = this.data.ai.hand[Number(e.currentTarget.dataset.idx)]
    if (!c) return
    const key = c.s + '-' + c.r
    const sel = (this._aiSel || []).slice()
    const i = sel.indexOf(key)
    if (i >= 0) sel.splice(i, 1)
    else sel.push(key)
    this._aiSel = sel
    this.aiRender()
    this.aiPreview()
  },

  aiSelectedCards(): Card[] {
    const a = this._ai
    const sel = this._aiSel || []
    if (!a || !sel.length) return []
    return a.hands[0].filter((c) => sel.indexOf(c.s + '-' + c.r) >= 0)
  },

  aiPreview() {
    const cards = this.aiSelectedCards()
    if (!cards.length) { this.setData({ 'ai.selName': '' }); return }
    const c = analyze(cards)
    this.setData({ 'ai.selName': c ? comboName(c) + '（' + c.count + ' 张）' : '不是合法牌型' })
  },

  aiPlaySelected() {
    const a = this._ai
    if (!a || a.winner >= 0 || a.turn !== 0) return
    const cards = this.aiSelectedCards()
    if (!cards.length) { wx.showToast({ title: '请先选牌', icon: 'none' }); return }
    const combo = analyze(cards)
    if (!combo) { wx.showToast({ title: '不是合法牌型', icon: 'none' }); return }
    if (a.last && a.last.player !== 0 && !canBeat(combo, a.last.combo)) {
      wx.showToast({ title: '压不过上一手', icon: 'none' })
      return
    }
    this.applyAiPlay(0, combo)
  },

  aiPass() {
    const a = this._ai
    if (!a || a.winner >= 0 || a.turn !== 0 || !a.last) return
    this.applyAiPass(0)
  },

  aiHint() {
    const a = this._ai
    if (!a || a.winner >= 0 || a.turn !== 0) return
    const hand = a.hands[0]
    let combo: Combo | null = null
    if (a.last && a.last.player !== 0) combo = respondHand(hand, a.last.combo)
    else combo = leadHand(hand)
    if (!combo) {
      this._aiSel = []
      this.aiRender()
      this.setData({ 'ai.selName': '' })
      wx.showToast({ title: '建议：要不起', icon: 'none' })
      return
    }
    this._aiSel = combo.cards.map((c) => c.s + '-' + c.r)
    this.aiRender()
    this.aiPreview()
    wx.showToast({ title: '提示：' + comboName(combo), icon: 'none' })
  },

  aiRender() {
    const a = this._ai
    if (!a) return
    const sel = this._aiSel || []
    const toView = (cards: Card[]) => cards.map((c) => ({
      s: c.s, r: c.r, name: cardName(c), red: cardIsRed(c),
    }))
    const handView = sortDesc(a.hands[0]).map((c) => ({
      s: c.s, r: c.r, name: cardName(c), red: cardIsRed(c),
      selected: sel.indexOf(c.s + '-' + c.r) >= 0,
    }))
    const lastCombo = a.last ? a.last.combo : null
    let statusText = ''
    if (a.winner >= 0) {
      statusText = a.winner === 0 ? '🎉 你赢了！' : playerName(a.winner) + ' 获胜，再来一局？'
    } else if (a.turn === 0) {
      statusText = a.last ? '轮到你出牌（要压过上一手）' : '你先出牌（地主）'
    } else {
      statusText = playerName(a.turn) + ' 思考中…'
    }
    this.setData({
      'ai.hand': handView,
      'ai.aiCounts': [a.hands[0].length, a.hands[1].length, a.hands[2].length],
      'ai.bottom': toView(a.bottom),
      'ai.lastPlayCards': lastCombo ? toView(lastCombo.cards) : [],
      'ai.lastPlayName': lastCombo ? comboName(lastCombo) : '',
      'ai.statusText': statusText,
      'ai.log': a.log.slice(-14),
      'ai.turn': a.turn,
      'ai.winner': a.winner,
    })
  },
})
