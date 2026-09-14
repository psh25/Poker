// 发牌机牌局云同步函数
// 动作：
//   save       保存/覆盖一局（games 集合）
//   get        取一局
//   list       列最近 50 局
//   findRoom   按房间码找开放中的一局
//   join       加入房间并认领牌堆号（不可重复；把 join 事件写入 moves）
//   remove     删除一局
//   pushMove   追加一条线上事件（type: play 线上出牌 / played 现场记牌；moves 集合按 gameId 分组）
//   fetchMoves 取某局所有线上事件
const cloud = require('wx-server-sdk')
cloud.init({ env: cloud.DYNAMIC_CURRENT_ENV })
const db = cloud.database()
const games = db.collection('games')
const moves = db.collection('moves')

async function appendMove(gameId, move) {
  const docId = 'g_' + String(gameId)
  const prev = await moves.doc(docId).get().catch(() => null)
  const arr = (prev && prev.data && prev.data.list) ? prev.data.list : []
  arr.push(move)
  await moves.doc(docId).set({ data: { list: arr, updatedAt: Date.now() } })
  return arr.length - 1
}

exports.main = async (event) => {
  const { action } = event
  try {
    if (action === 'save') {
      const game = event.game
      if (!game || !game.id) return { ok: false, error: 'no-game' }
      await games.doc(String(game.id)).set({ data: { ...game, updatedAt: Date.now() } })
      return { ok: true }
    }
    if (action === 'get') {
      const res = await games.doc(String(event.id)).get()
      return { ok: true, data: res.data }
    }
    if (action === 'findRoom') {
      const code = String(event.code || '').trim()
      if (!code) return { ok: false, error: 'no-code' }
      const res = await games.where({ roomCode: code, open: true }).limit(1).get()
      if (!res.data.length) return { ok: false, error: 'room-not-found' }
      return { ok: true, data: res.data[0] }
    }
    if (action === 'join') {
      const code = String(event.code || '').trim()
      const pile = Number(event.pile)
      if (!code) return { ok: false, error: 'no-code' }
      if (!Number.isInteger(pile) || pile < 0 || pile > 20) return { ok: false, error: 'bad-pile' }
      const res = await games.where({ roomCode: code, open: true }).limit(1).get()
      if (!res.data.length) return { ok: false, error: 'room-not-found' }
      const g = res.data[0]
      const members = Array.isArray(g.members) ? g.members.slice() : []
      if (members.indexOf(pile) >= 0) return { ok: false, error: 'pile-taken' }
      members.push(pile)
      await games.doc(String(g.id)).update({ data: { members } })
      const move = { type: 'join', id: 'j' + Date.now().toString(36) + Math.floor(Math.random() * 1e6).toString(36), p: pile, ts: Date.now() }
      await appendMove(g.id, move)
      return { ok: true, data: g, members }
    }
    if (action === 'list') {
      const res = await games.orderBy('startTime', 'desc').limit(50).get()
      return { ok: true, data: res.data }
    }
    if (action === 'remove') {
      await games.doc(String(event.id)).remove()
      return { ok: true }
    }
    if (action === 'pushMove') {
      const { gameId, move } = event
      if (!gameId || !move) return { ok: false, error: 'no-move' }
      if (!move.type) move.type = 'play'
      if (!move.id) move.id = Date.now().toString(36) + Math.floor(Math.random() * 1e6).toString(36)
      const idx = await appendMove(gameId, move)
      return { ok: true, idx }
    }
    if (action === 'fetchMoves') {
      const docId = 'g_' + String(event.gameId)
      const res = await moves.doc(docId).get().catch(() => null)
      return { ok: true, data: (res && res.data && res.data.list) ? res.data.list : [] }
    }
    return { ok: false, error: 'unknown-action' }
  } catch (e) {
    return { ok: false, error: String((e && e.errMsg) || e) }
  }
}
