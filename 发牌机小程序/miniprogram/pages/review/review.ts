import { store, GameRecord } from '../../utils/store'
import { cardName, cardIsRed } from '../../utils/cards'
import { cloudReady, getGameFromCloud, saveGameToCloud } from '../../utils/cloud'

Page({
  data: {
    title: '',
    time: '',
    phase: '',
    deck: [] as { idx: number; pile: number; name: string; red: boolean }[],
    piles: [] as { pile: number; cards: string[] }[],
    played: [] as { p: number; name: string; red: boolean }[],
    cloudOk: false,
    gameId: '',
    fromCloud: false,
  },

  game: null as GameRecord | null,

  onLoad(options: any) {
    this.setData({ cloudOk: cloudReady(), gameId: options.id || '', fromCloud: options.cloud === '1' })
    this.loadGame(options.id || '', options.cloud === '1')
  },

  async loadGame(id: string, fromCloud: boolean) {
    let g: GameRecord | null = null
    if (fromCloud) {
      const res = await getGameFromCloud(id)
      if (res.ok && res.data) g = res.data as GameRecord
    } else {
      g = store.history().find((x) => x.id === id) || null
    }
    if (!g) { wx.showToast({ title: '未找到该牌局', icon: 'none' }); return }
    this.game = g
    const deck = g.deck.map((c) => ({ idx: c.idx, pile: c.pile, name: cardName({ s: c.s, r: c.r }), red: cardIsRed({ s: c.s, r: c.r }) }))
    const piles = g.piles.map((cards, i) => ({ pile: i, cards: cards.map((c) => cardName(c)) }))
    const played = g.played.map((p) => ({ p: p.p, name: cardName({ s: p.s, r: p.r }), red: cardIsRed({ s: p.s, r: p.r }) }))
    this.setData({
      title: g.modeName,
      time: new Date(g.startTime).toLocaleString(),
      phase: g.phase,
      deck, piles, played,
    })
  },

  async syncToCloud() {
    if (!cloudReady()) { wx.showToast({ title: '云环境未配置', icon: 'none' }); return }
    if (!this.game) return
    const res = await saveGameToCloud(this.game)
    if (res.ok) wx.showToast({ title: '已同步到云', icon: 'success' })
    else wx.showToast({ title: '同步失败', icon: 'none' })
  },
})