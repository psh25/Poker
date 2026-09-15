import { bleManager, BleConnState } from '../../utils/ble'
import { MODES } from '../../config/index'
import { HOST_CMD, STATE_NAMES, decodeEvent } from '../../utils/protocol'
import { cardIsRed, cardName } from '../../utils/cards'
import { store, requestSegment, markMoveApplied } from '../../utils/store'
import { cloudReady, pushMoveToCloud } from '../../utils/cloud'
import { emit } from '../../utils/bus'

declare function setTimeout(handler: () => void, timeout?: number): number

interface DealtCardView {
  idx: number
  pile: number
  name: string
  red: boolean
  unknown: boolean
  simulated: boolean
}

function delay(ms: number): Promise<void> {
  return new Promise((resolve) => setTimeout(resolve, ms))
}

function parseCustomSequence(raw: string): number[] {
  const parts = raw.split(/[\s,，]+/).filter((s) => s.length > 0)
  if (!parts.length || parts.length > 64) throw new Error('自定义顺序需要 1~64 个牌堆号')
  const sequence = parts.map((s) => Number(s))
  for (const deck of sequence) {
    if (!Number.isInteger(deck) || deck < 0 || deck >= 4) {
      throw new Error('牌堆号只能是 0、1、2、3')
    }
  }
  return sequence
}

function selectedSequence(modeId: number, customText: string): number[] {
  const mode = MODES.find((m) => m.id === modeId)
  if (!mode) throw new Error('未知发牌模式')
  return mode.id === 3 ? parseCustomSequence(customText) : mode.sequence.slice()
}

Page({
  data: {
    bleState: 'idle' as BleConnState,
    bleStateText: '未连接',
    scanning: false,
    devices: [] as { deviceId: string; name: string; rssi: number; matched: boolean }[],
    showDeviceList: false,
    selectedMode: 0,
    modes: MODES,
    customSequenceText: '0,1,2,3',
    cloudOk: false,
    boardState: 0,
    boardStateText: '',
    dealing: false,
    dealDone: false,
    dealCount: 0,
    dealTotal: 0,
    dealPercent: 0,
    dealtCards: [] as DealtCardView[],
    lastCardText: '',
  },

  onLoad() {
    this.setData({ cloudOk: cloudReady() })
    bleManager.onStateChange = (st) => {
      const text = st === 'connected' ? '已连接' : st === 'scanning' ? '扫描中…' : st === 'connecting' ? '连接中…' : st === 'error' ? '出错' : '未连接'
      this.setData({ bleState: st, bleStateText: text })
      if (st === 'connected') this.setData({ showDeviceList: false, scanning: false })
    }
    // 全局帧分发：底板二进制事件 -> 存储 -> 事件总线
    bleManager.onFrame = (type, data) => {
      const ev = decodeEvent(type, data)
      if (ev.type === 'card') {
        const card = { s: ev.s, r: ev.r }
        const name = cardName(card)
        const unknown = ev.s === 0xff || ev.r === 0xff
        const view: DealtCardView = {
          idx: ev.idx + 1,
          pile: ev.pile,
          name,
          red: cardIsRed(card),
          unknown,
          simulated: ev.simulated,
        }
        store.onCard({ s: ev.s, r: ev.r, pile: ev.pile, idx: ev.idx })
        const dealtCards = this.data.dealtCards.concat([view])
        const total = this.data.dealTotal || (store.current ? store.current.deck.length : dealtCards.length)
        const dealPercent = total > 0 ? Math.min(100, Math.round(dealtCards.length * 100 / total)) : 0
        this.setData({
          dealtCards,
          dealCount: dealtCards.length,
          lastCardText: name,
          dealTotal: total,
          dealPercent,
        })
        emit('deal', ev)
      } else if (ev.type === 'played') {
        if (ev.s >= 0 && ev.s <= 4 && ev.r >= 3 && ev.r <= 17) {
          store.onPlayed({ s: ev.s, r: ev.r, p: ev.p, ts: Date.now() })
          emit('played', ev)
          // 现场按编码器记的牌 → 同步云端（线上玩家同页可见）
          if (cloudReady() && store.current) {
            const move = {
              type: 'played',
              id: 'p' + Date.now().toString(36) + Math.random().toString(36).slice(2, 6),
              p: ev.p,
              cards: [{ s: ev.s, r: ev.r }],
              ts: Date.now(),
            }
            markMoveApplied(store.current.id, move.id)
            pushMoveToCloud(store.current.id, move).catch(() => {})
          }
        } else {
          console.log('[BLE] played 牌面未知(s=' + ev.s + ' r=' + ev.r + ')，仅记玩家 p=' + ev.p)
        }
      } else if (ev.type === 'deal_done') {
        store.finishDeal()
        const total = ev.total || (store.current ? store.current.deck.length : this.data.dealCount)
        const received = this.data.dealtCards.length
        this.setData({
          dealing: false,
          dealDone: true,
          dealTotal: total,
          dealCount: received,
          dealPercent: total > 0 ? Math.min(100, Math.round(received * 100 / total)) : 100,
        })
        wx.showToast({ title: '整副发完（' + total + ' 张）', icon: 'success' })
        emit('deal_done', ev)
      } else if (ev.type === 'state') {
        this.setData({
          boardState: ev.state,
          boardStateText: STATE_NAMES[ev.state] || ('状态' + ev.state),
        })
        emit('state', ev)
      } else if (ev.type === 'ack') {
        // 每条命令的回执（0x90）；需要时可在此打印 ev.origType
      }
    }
  },

  onUnload() {
    bleManager.onFrame = undefined
    bleManager.onStateChange = undefined
  },

  async startScan() {
    // Android 真机扫描 BLE 需要定位权限
    await new Promise<void>((resolve) => {
      wx.authorize({ scope: 'scope.userLocation', success: () => resolve(), fail: () => resolve() })
    })
    this.setData({ scanning: true, showDeviceList: true })
    const list = await bleManager.scan()
    const devices = list.slice(0, 50)
    this.setData({ devices, scanning: false })
    if (!devices.some((d) => d.matched)) {
      wx.showToast({ title: '暂未发现 DealerBot，请确认底板已通电并重新扫描', icon: 'none', duration: 3000 })
    }
  },

  onPickDevice(e: any) {
    const id = e.currentTarget.dataset.id
    const dev = this.data.devices.find((d) => d.deviceId === id)
    if (!dev) return
    wx.showLoading({ title: '连接中…' })
    bleManager.connect(dev.deviceId)
      .then(() => { wx.hideLoading() })
      .catch((err: any) => {
        wx.hideLoading()
        const msg = (err && err.errMsg) ? String(err.errMsg)
          : (err && err.message) ? String(err.message) : '连接失败'
        console.error('[BLE] 连接失败:', err)
        wx.showModal({ title: '连接失败', content: msg, showCancel: false })
      })
  },

  disconnect() {
    bleManager.disconnect()
  },

  onSelectMode(e: any) {
    const selectedMode = Number(e.currentTarget.dataset.id)
    const mode = MODES.find((m) => m.id === selectedMode)
    this.setData({ selectedMode, dealTotal: mode ? mode.totalCards : 0 })
  },

  onCustomSequenceInput(e: any) {
    this.setData({ customSequenceText: String(e.detail.value || '') })
  },

  // 分帧上传完整发牌计划：0x14 开始 → 0x15 数据块 → 0x16 提交。
  async sendDealPlan(scheme: number, sequence: number[]) {
    const total = sequence.length
    await bleManager.writeFrame(HOST_CMD.PLAN_BEGIN, [scheme, total & 0xff, (total >> 8) & 0xff, 4])
    const chunkSize = 30
    for (let offset = 0; offset < total; offset += chunkSize) {
      await bleManager.writeFrame(HOST_CMD.PLAN_CHUNK, [offset].concat(sequence.slice(offset, offset + chunkSize)))
    }
    await bleManager.writeFrame(HOST_CMD.PLAN_COMMIT)
  },

  // 发送“完整计划 + 选方案 + 确认”
  async sendMode() {
    if (bleManager.state !== 'connected') { wx.showToast({ title: '先连接发牌机', icon: 'none' }); return }
    const m = this.data.selectedMode
    let sentCount = 0
    try {
      const sequence = selectedSequence(m, this.data.customSequenceText)
      sentCount = sequence.length
      await bleManager.writeFrame(HOST_CMD.SELECT_SCHEME, [m])
      await this.sendDealPlan(m, sequence)
      await bleManager.writeFrame(HOST_CMD.CONFIRM)
      this.setData({ dealTotal: sentCount })
    } catch (e) {
      const msg = e instanceof Error ? e.message : '发牌计划无效'
      wx.showToast({ title: msg, icon: 'none' })
      return
    }
    wx.showToast({ title: '方案和 ' + sentCount + ' 张计划已发送', icon: 'success' })
  },

  // 开始发牌：复位 → 选方案 → 上传完整计划 → 确认 → 发牌。
  async startDeal() {
    if (bleManager.state !== 'connected') { wx.showToast({ title: '先连接发牌机', icon: 'none' }); return }
    if (this.data.dealing) return

    const m = this.data.selectedMode
    let sequence: number[]
    try {
      sequence = selectedSequence(m, this.data.customSequenceText)
    } catch (e) {
      const msg = e instanceof Error ? e.message : '发牌计划无效'
      wx.showToast({ title: msg, icon: 'none' })
      return
    }

    store.startGame(m)
    this.setData({
      dealing: true,
      dealDone: false,
      dealCount: 0,
      dealTotal: sequence.length,
      dealPercent: 0,
      dealtCards: [],
      lastCardText: '',
    })
    wx.showLoading({ title: '发送发牌计划…' })
    try {
      await bleManager.writeFrame(HOST_CMD.RESET)
      await delay(600)
      await bleManager.writeFrame(HOST_CMD.SELECT_SCHEME, [m])
      await this.sendDealPlan(m, sequence)
      await bleManager.writeFrame(HOST_CMD.CONFIRM)
      await bleManager.writeFrame(HOST_CMD.DEAL)
    } catch (e) {
      wx.hideLoading()
      this.setData({ dealing: false })
      wx.showToast({ title: '发送失败', icon: 'none' })
      return
    }
    wx.hideLoading()
    wx.showToast({ title: '发牌指令已发送', icon: 'success' })
  },

  // 发完后跳到“牌局”页并停到“记牌”页签（牌局页是 tabBar 页，必须用 switchTab）
  goCount() {
    requestSegment('count')
    wx.switchTab({ url: '/pages/game/game' })
  },

  stopBoard() {
    if (bleManager.state !== 'connected') return
    bleManager.writeFrame(HOST_CMD.STOP).catch(() => {})
    wx.showToast({ title: '已发送停机', icon: 'none' })
  },

  resetBoard() {
    if (bleManager.state !== 'connected') return
    bleManager.writeFrame(HOST_CMD.RESET).catch(() => {})
    this.setData({
      dealing: false,
      dealDone: false,
      dealCount: 0,
      dealTotal: 0,
      dealPercent: 0,
      dealtCards: [],
      lastCardText: '',
    })
    wx.showToast({ title: '已发送复位', icon: 'none' })
  },
})