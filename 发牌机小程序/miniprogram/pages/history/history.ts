import { store, GameRecord } from '../../utils/store'
import { cloudReady, listGamesFromCloud } from '../../utils/cloud'

Page({
  data: {
    list: [] as any[],
    cloudOk: false,
    pulling: false,
  },

  onShow() {
    this.setData({ cloudOk: cloudReady() })
    this.refresh()
  },

  refresh() {
    const list = store.history().map((g: GameRecord) => ({
      id: g.id,
      title: g.modeName,
      time: new Date(g.startTime).toLocaleString(),
      cards: g.deck.length,
      played: g.played.length,
      phase: g.phase,
    }))
    this.setData({ list })
  },

  onTap(e: any) {
    const id = e.currentTarget.dataset.id
    wx.navigateTo({ url: '/pages/review/review?id=' + id })
  },

  async pullCloud() {
    this.setData({ pulling: true })
    const res = await listGamesFromCloud()
    this.setData({ pulling: false })
    if (res.ok && res.data && res.data.length) {
      wx.showToast({ title: '拉到 ' + res.data.length + ' 局', icon: 'none' })
      // 云上的牌局仅用于评审查看，这里直接进入最近一局
      const first = res.data[0]
      wx.navigateTo({ url: '/pages/review/review?cloud=1&id=' + first._id })
    } else {
      wx.showToast({ title: '云端暂无数据', icon: 'none' })
    }
  },
})