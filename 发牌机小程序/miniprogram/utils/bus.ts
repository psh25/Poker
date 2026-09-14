// 极简事件总线，用于 BLE 消息 -> 页面 的通知
type Handler = (data?: any) => void
const map: { [k: string]: Handler[] } = {}

export function on(evt: string, fn: Handler): void {
  (map[evt] = map[evt] || []).push(fn)
}
export function off(evt: string, fn: Handler): void {
  const a = map[evt]
  if (!a) return
  const i = a.indexOf(fn)
  if (i >= 0) a.splice(i, 1)
}
export function emit(evt: string, data?: any): void {
  const a = map[evt] || []
  a.forEach((fn) => { try { fn(data) } catch (e) { /* ignore */ } })
}