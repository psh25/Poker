// ============================================================
// 微信云开发封装。
// 说明：云环境（CLOUD.ENV_ID）配好之前，所有接口静默走本地；
//       配好后自动切换为云数据库同步（多手机共享、线上第三人用）。
// ============================================================
import { CLOUD } from '../config/index'

let inited = false
let ready = false

export function initCloud(): void {
  if (inited) return
  inited = true
  try {
    if (CLOUD.ENV_ID) {
      wx.cloud.init({ env: CLOUD.ENV_ID, traceUser: true })
      ready = true
    }
  } catch (e) {
    ready = false
  }
}

export function cloudReady(): boolean {
  return ready
}

export interface SyncResult { ok: boolean; data?: any; error?: string }

export function callSync(action: string, payload: any = {}): Promise<SyncResult> {
  if (!ready) return Promise.resolve({ ok: false, error: 'cloud-not-configured' })
  return wx.cloud.callFunction({ name: 'sync', data: { action, ...payload } })
    .then((res: any) => {
      const r = res.result || {}
      return { ok: !!r.ok, data: r.data, error: r.error }
    })
    .catch((err: any) => ({ ok: false, error: (err && err.errMsg) || 'cloud-error' }))
}

export function saveGameToCloud(game: any): Promise<SyncResult> {
  return callSync('save', { game })
}
export function getGameFromCloud(id: string): Promise<SyncResult> {
  return callSync('get', { id })
}
export function listGamesFromCloud(): Promise<SyncResult> {
  return callSync('list')
}
export function removeGameFromCloud(id: string): Promise<SyncResult> {
  return callSync('remove', { id })
}
export function pushMoveToCloud(gameId: string, move: any): Promise<SyncResult> {
  return callSync('pushMove', { gameId, move })
}
export function fetchMovesFromCloud(gameId: string): Promise<SyncResult> {
  return callSync('fetchMoves', { gameId })
}
// 按房间码找开放中的牌局（线上玩家加入用）
export function findByRoom(roomCode: string): Promise<SyncResult> {
  return callSync('findRoom', { code: roomCode })
}
// 线上玩家加入房间并认领牌堆号（牌堆号即身份，不能重复认领）
export function joinRoom(roomCode: string, pile: number): Promise<SyncResult> {
  return callSync('join', { code: roomCode, pile })
}

// 实时监听某局的线上出牌（moves 集合）；权限不允许时自动失败，
// 页面会回退到轮询（见 game.ts）。返回取消函数。
export function watchGameMoves(gameId: string, onChange: (list: any[]) => void): () => void {
  if (!ready) return () => {}
  try {
    const db = wx.cloud.database()
    const docId = 'g_' + gameId
    let closed = false
    const watcher = db.collection('moves').doc(docId).watch({
      onChange: (snapshot: any) => {
        if (closed) return
        const list = (snapshot && snapshot.docs && snapshot.docs.length && snapshot.docs[0].list) || []
        onChange(list)
      },
      onError: (err: any) => {
        console.error('[cloud] watch 失败，回退轮询:', err)
      },
    })
    return () => {
      closed = true
      try { watcher.close() } catch (e) { /* ignore */ }
    }
  } catch (e) {
    return () => {}
  }
}