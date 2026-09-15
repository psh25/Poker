// ============================================================
// 牌面模型与编码
//   suit: 0=♠ 1=♥ 2=♣ 3=♦ 4=王
//   rank: 3..15 = 3,4,5,6,7,8,9,10,J,Q,K,A,2 ; 16=小王 ; 17=大王
// 与底板约定一致（子板摄像头识别后经底板蓝牙传出）。
// ============================================================
export interface Card {
  s: number
  r: number
}

export const SUIT_SYMBOLS = ['♠', '♥', '♣', '♦', '★']
export const SUIT_RED = [false, true, false, true, true]

export const RANK_NAMES: { [key: number]: string } = {
  3: '3', 4: '4', 5: '5', 6: '6', 7: '7', 8: '8', 9: '9', 10: '10',
  11: 'J', 12: 'Q', 13: 'K', 14: 'A', 15: '2', 16: '小王', 17: '大王',
}

export function cardName(c: Card): string {
  if (c.s === 0xff || c.r === 0xff) return '未知'
  if (c.r === 16) return '小王'
  if (c.r === 17) return '大王'
  return (SUIT_SYMBOLS[c.s] || '?') + (RANK_NAMES[c.r] || String(c.r))
}

export function cardIsRed(c: Card): boolean {
  if (c.s === 0xff || c.r === 0xff) return false
  return c.s === 1 || c.s === 3 || c.r >= 16
}

// 斗地主整副 54 张（按花色/点数生成）
export function fullDeck(): Card[] {
  const deck: Card[] = []
  for (let s = 0; s < 4; s++) {
    for (let r = 3; r <= 15; r++) deck.push({ s, r })
  }
  deck.push({ s: 4, r: 16 }, { s: 4, r: 17 })
  return deck
}

// 记牌用：一副牌里每张还有几张（默认 54 张）
export function newRemainMap(): { [key: string]: number } {
  const m: { [key: string]: number } = {}
  for (const c of fullDeck()) {
    const k = keyOf(c)
    m[k] = (m[k] || 0) + 1
  }
  return m
}

export function keyOf(c: Card): string {
  return c.s + '-' + c.r
}

// 排序：点数从大到小
export function sortDesc(cards: Card[]): Card[] {
  return cards.slice().sort((a, b) => b.r - a.r || a.s - b.s)
}