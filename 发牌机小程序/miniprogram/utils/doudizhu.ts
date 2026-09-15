// ============================================================
// 斗地主 AI 模块（精简版 · 纯逻辑 · 不依赖 wx API）
//
// 功能：
//   analyze()         识别一组牌是什么牌型（非法返回 null）
//   findAllPlays()    枚举一手牌里所有能出的牌型（用于"提示"）
//   leadHand()        主动出牌：选一手最合适的
//   respondHand()     跟牌：出能压过上一手的最合适的一手（压不过返回 null）
//   canBeat()         判断 combo 能否压过 last
//   comboName()       牌型的中文描述（用于界面展示）
//
// 思路参考经典权重/规则类斗地主 AI（ZhouWeikuan/DouDiZhu 等），
// 纯本地计算，一手牌枚举在毫秒级，适合跑在小程序里。
// 后续可接入 store.remainMap() 的记牌信息，让 AI 更聪明。
// ============================================================
import { Card, RANK_NAMES } from './cards'

export const RANK_3 = 3
export const RANK_2 = 15
export const RANK_SJOKER = 16
export const RANK_BJOKER = 17

export type ComboType =
  | 'single' | 'pair' | 'triple' | 'triple_single' | 'triple_pair'
  | 'straight' | 'double_straight' | 'airplane' | 'airplane_single' | 'airplane_pair'
  | 'four_two' | 'four_two_pair' | 'bomb' | 'rocket'

export interface Combo {
  type: ComboType
  main: number[]      // 主牌点数（升序）
  kickers: number[]   // 带的点数（升序，无则空数组）
  cards: Card[]       // 组成这副牌的具体牌
  weight: number      // 比大小用（主牌最大点；王炸=99）
  count: number       // 总张数
}

export const COMBO_NAMES: { [k in ComboType]: string } = {
  single: '单张',
  pair: '对子',
  triple: '三张',
  triple_single: '三带一',
  triple_pair: '三带二',
  straight: '顺子',
  double_straight: '连对',
  airplane: '飞机',
  airplane_single: '飞机带单',
  airplane_pair: '飞机带对',
  four_two: '四带二',
  four_two_pair: '四带两对',
  bomb: '炸弹',
  rocket: '王炸',
}

// ---------------- 内部工具 ----------------

function groupRanks(cards: Card[]): Map<number, Card[]> {
  const m = new Map<number, Card[]>()
  for (const c of cards) {
    const a = m.get(c.r)
    if (a) a.push(c)
    else m.set(c.r, [c])
  }
  return m
}

function sortedRanks(m: Map<number, Card[]>): number[] {
  return Array.from(m.keys()).sort((a, b) => a - b)
}

function makeCombo(type: ComboType, main: number[], kickers: number[], cards: Card[]): Combo {
  const weight = main.length ? main[main.length - 1] : 0
  return { type, main: main.slice(), kickers: kickers.slice(), cards: cards.slice(), weight, count: cards.length }
}

function isConsecutive(ranks: number[]): boolean {
  for (let i = 1; i < ranks.length; i++) {
    if (ranks[i] !== ranks[i - 1] + 1) return false
  }
  return true
}

function hasJoker(ranks: number[]): boolean {
  return ranks.some((r) => r >= RANK_SJOKER)
}

function takeN(m: Map<number, Card[]>, r: number, n: number): Card[] {
  const a = m.get(r)
  return a ? a.slice(0, n) : []
}

// 飞机带单 / 带对：找 k 组连续三张 + k 个单/对
function findAirplaneWings(cards: Card[]): Combo | null {
  const n = cards.length
  if (n < 8) return null
  const m = groupRanks(cards)
  const ranks = sortedRanks(m)
  if (hasJoker(ranks)) return null
  const cnt = (r: number) => (m.get(r) || []).length

  // 带单：4k 张，k>=2
  if (n % 4 === 0) {
    const k = n / 4
    const triples = ranks.filter((r) => cnt(r) === 3)
    for (let i = 0; i + k <= triples.length; i++) {
      const main = triples.slice(i, i + k)
      if (!isConsecutive(main)) continue
      const singles = ranks.filter((r) => main.indexOf(r) < 0 && cnt(r) >= 1)
      if (singles.length >= k) {
        const kickers = singles.slice(0, k)
        const cardsOut: Card[] = []
        for (const r of main) cardsOut.push(...takeN(m, r, 3))
        for (const r of kickers) cardsOut.push(...takeN(m, r, 1))
        return makeCombo('airplane_single', main, kickers, cardsOut)
      }
    }
  }

  // 带对：5k 张，k>=2
  if (n % 5 === 0) {
    const k = n / 5
    const triples = ranks.filter((r) => cnt(r) === 3)
    for (let i = 0; i + k <= triples.length; i++) {
      const main = triples.slice(i, i + k)
      if (!isConsecutive(main)) continue
      const pairs = ranks.filter((r) => main.indexOf(r) < 0 && cnt(r) >= 2)
      if (pairs.length >= k) {
        const kickers = pairs.slice(0, k)
        const cardsOut: Card[] = []
        for (const r of main) cardsOut.push(...takeN(m, r, 3))
        for (const r of kickers) cardsOut.push(...takeN(m, r, 2))
        return makeCombo('airplane_pair', main, kickers, cardsOut)
      }
    }
  }
  return null
}

// ---------------- 牌型识别 ----------------

export function analyze(cards: Card[]): Combo | null {
  const n = cards.length
  if (!n) return null
  const m = groupRanks(cards)
  const ranks = sortedRanks(m)
  const counts = ranks.map((r) => (m.get(r) || []).length)

  // 王炸
  if (n === 2 && ranks[0] === RANK_SJOKER && ranks[1] === RANK_BJOKER) {
    return makeCombo('rocket', ranks, [], cards)
  }
  // 炸弹
  if (n === 4 && ranks.length === 1) return makeCombo('bomb', ranks, [], cards)

  if (n === 1) return makeCombo('single', ranks, [], cards)
  if (n === 2 && ranks.length === 1) return makeCombo('pair', ranks, [], cards)
  if (n === 3 && ranks.length === 1) return makeCombo('triple', ranks, [], cards)

  // 三带一
  if (n === 4 && ranks.length === 2 && counts.indexOf(3) >= 0 && counts.indexOf(1) >= 0) {
    const r3 = ranks[counts.indexOf(3)]
    const r1 = ranks[counts.indexOf(1)]
    return makeCombo('triple_single', [r3], [r1], cards)
  }
  // 三带二
  if (n === 5 && ranks.length === 2 && counts.indexOf(3) >= 0 && counts.indexOf(2) >= 0) {
    const r3 = ranks[counts.indexOf(3)]
    const r2 = ranks[counts.indexOf(2)]
    return makeCombo('triple_pair', [r3], [r2], cards)
  }
  // 四带二（两单）
  if (n === 6 && ranks.length === 3 && counts.indexOf(4) >= 0) {
    const r4 = ranks[counts.indexOf(4)]
    const kickers = ranks.filter((_, i) => counts[i] === 1)
    if (kickers.length === 2) return makeCombo('four_two', [r4], kickers, cards)
  }
  // 四带两对
  if (n === 8 && ranks.length === 3 && counts.indexOf(4) >= 0 && counts.filter((c) => c === 2).length === 2) {
    const r4 = ranks[counts.indexOf(4)]
    const kickers = ranks.filter((_, i) => counts[i] === 2)
    return makeCombo('four_two_pair', [r4], kickers, cards)
  }
  // 顺子（5 张起，不含 2 和王）
  if (n >= 5 && !hasJoker(ranks) && counts.every((c) => c === 1) && isConsecutive(ranks)) {
    return makeCombo('straight', ranks, [], cards)
  }
  // 连对（3 连起）
  if (n >= 6 && n % 2 === 0 && !hasJoker(ranks) && counts.every((c) => c === 2) && isConsecutive(ranks)) {
    return makeCombo('double_straight', ranks, [], cards)
  }
  // 飞机不带
  if (n >= 6 && n % 3 === 0 && !hasJoker(ranks) && counts.every((c) => c === 3) && isConsecutive(ranks)) {
    return makeCombo('airplane', ranks, [], cards)
  }
  // 飞机带单 / 带对
  const wings = findAirplaneWings(cards)
  if (wings) return wings

  return null
}

// ---------------- 手牌枚举 ----------------

export function findAllPlays(hand: Card[]): Combo[] {
  const m = groupRanks(hand)
  const ranks = sortedRanks(m)
  const cnt = (r: number) => (m.get(r) || []).length
  const res: Combo[] = []

  // 单张 / 对子 / 三张 / 炸弹
  for (const r of ranks) {
    const c = cnt(r)
    res.push(makeCombo('single', [r], [], takeN(m, r, 1)))
    if (c >= 2) res.push(makeCombo('pair', [r], [], takeN(m, r, 2)))
    if (c >= 3) res.push(makeCombo('triple', [r], [], takeN(m, r, 3)))
    if (c >= 4) res.push(makeCombo('bomb', [r], [], takeN(m, r, 4)))
  }

  // 王炸
  if (cnt(RANK_SJOKER) >= 1 && cnt(RANK_BJOKER) >= 1) {
    const cs: Card[] = []
    cs.push(...takeN(m, RANK_SJOKER, 1), ...takeN(m, RANK_BJOKER, 1))
    res.push(makeCombo('rocket', [RANK_SJOKER, RANK_BJOKER], [], cs))
  }

  // 三带一 / 三带二
  for (const r of ranks) {
    if (cnt(r) < 3) continue
    const singles = ranks.filter((x) => x !== r && cnt(x) >= 1)
    if (singles.length) {
      const k = singles[0]
      res.push(makeCombo('triple_single', [r], [k], takeN(m, r, 3).concat(takeN(m, k, 1))))
    }
    const pairs = ranks.filter((x) => x !== r && cnt(x) >= 2)
    if (pairs.length) {
      const k = pairs[0]
      res.push(makeCombo('triple_pair', [r], [k], takeN(m, r, 3).concat(takeN(m, k, 2))))
    }
  }

  // 四带二（两单）/ 四带两对
  for (const r of ranks) {
    if (cnt(r) !== 4) continue
    const others = ranks.filter((x) => x !== r)
    const singles = others.filter((x) => cnt(x) >= 1)
    if (singles.length >= 2) {
      const k = singles.slice(0, 2)
      res.push(makeCombo('four_two', [r], k, takeN(m, r, 4).concat(takeN(m, k[0], 1), takeN(m, k[1], 1))))
    }
    const pairs = others.filter((x) => cnt(x) >= 2)
    if (pairs.length >= 2) {
      const k = pairs.slice(0, 2)
      res.push(makeCombo('four_two_pair', [r], k, takeN(m, r, 4).concat(takeN(m, k[0], 2), takeN(m, k[1], 2))))
    }
  }

  // 顺子
  for (let start = RANK_3; start <= RANK_2 - 4; start++) {
    for (let len = 5; start + len - 1 <= RANK_2; len++) {
      const seq: number[] = []
      for (let i = 0; i < len; i++) seq.push(start + i)
      if (seq.some((r) => cnt(r) < 1)) break
      const cs: Card[] = []
      for (const r of seq) cs.push(takeN(m, r, 1)[0])
      res.push(makeCombo('straight', seq, [], cs))
    }
  }

  // 连对
  for (let start = RANK_3; start <= RANK_2 - 2; start++) {
    for (let len = 3; start + len - 1 <= RANK_2; len++) {
      const seq: number[] = []
      for (let i = 0; i < len; i++) seq.push(start + i)
      if (seq.some((r) => cnt(r) < 2)) break
      const cs: Card[] = []
      for (const r of seq) cs.push(...takeN(m, r, 2))
      res.push(makeCombo('double_straight', seq, [], cs))
    }
  }

  // 飞机（不带 / 带单 / 带对）
  const tripleRanks = ranks.filter((r) => cnt(r) >= 3)
  for (let len = 2; len <= tripleRanks.length; len++) {
    for (let i = 0; i + len <= tripleRanks.length; i++) {
      const main = tripleRanks.slice(i, i + len)
      if (!isConsecutive(main)) continue
      const mainCards: Card[] = []
      for (const r of main) mainCards.push(...takeN(m, r, 3))
      res.push(makeCombo('airplane', main, [], mainCards))
      // 带单
      const singles = ranks.filter((r) => main.indexOf(r) < 0 && cnt(r) >= 1)
      if (singles.length >= len) {
        const k = singles.slice(0, len)
        const cs = mainCards.slice()
        for (const r of k) cs.push(takeN(m, r, 1)[0])
        res.push(makeCombo('airplane_single', main, k, cs))
      }
      // 带对
      const pairs = ranks.filter((r) => main.indexOf(r) < 0 && cnt(r) >= 2)
      if (pairs.length >= len) {
        const k = pairs.slice(0, len)
        const cs = mainCards.slice()
        for (const r of k) cs.push(...takeN(m, r, 2))
        res.push(makeCombo('airplane_pair', main, k, cs))
      }
    }
  }

  // 提示/跟牌用：张数少在前、权重小在前
  res.sort((a, b) => a.count - b.count || a.weight - b.weight)
  return res
}

// ---------------- 大小比较 ----------------

const FIXED_LENGTH_TYPES: ComboType[] = [
  'single', 'pair', 'triple', 'triple_single', 'triple_pair', 'four_two', 'four_two_pair',
]

export function canBeat(combo: Combo, last: Combo): boolean {
  if (last.type === 'rocket') return false
  if (combo.type === 'rocket') return true
  if (combo.type === 'bomb') {
    return last.type !== 'bomb' || combo.weight > last.weight
  }
  if (combo.type !== last.type) return false
  if (FIXED_LENGTH_TYPES.indexOf(combo.type) >= 0) return combo.weight > last.weight
  return combo.count === last.count && combo.weight > last.weight
}

// ---------------- 出牌策略 ----------------

// 主动出牌：张数多、权重小；尽量不打炸弹/王炸（除非没别的可出）
export function leadHand(hand: Card[]): Combo | null {
  if (!hand.length) return null
  // 能一把出完就直接出
  if (hand.length <= 5) {
    const win = analyze(hand)
    if (win) return win
  }
  const all = findAllPlays(hand)
  const normal = all.filter((c) => c.type !== 'bomb' && c.type !== 'rocket')
  const pool = normal.length ? normal : all
  let best: Combo | null = null
  let bestScore = -Infinity
  for (const c of pool) {
    let score = c.count * 10 - c.weight
    if (c.type === 'single') score -= 2 // 尽量不单出
    if (c.count === hand.length) score += 1000
    if (score > bestScore) {
      bestScore = score
      best = c
    }
  }
  return best
}

// 跟牌：选最省的一手压过；只剩炸弹可压且手牌还多时选择"过"
export function respondHand(hand: Card[], last: Combo): Combo | null {
  const m = groupRanks(hand)
  const cnt = (r: number) => (m.get(r) || []).length
  const cands = findAllPlays(hand).filter((c) => canBeat(c, last))
  if (!cands.length) return null

  let best: Combo | null = null
  let bestScore = Infinity
  for (const c of cands) {
    // 拆牌惩罚：用掉某点数后，该点数剩余张数越多说明拆得越狠（拆对/拆三）
    const used = new Map<number, number>()
    for (const card of c.cards) used.set(card.r, (used.get(card.r) || 0) + 1)
    let breakPen = 0
    for (const [r, u] of used) {
      const remain = cnt(r) - u
      breakPen += remain * 2
    }
    let score = c.weight + breakPen
    if (c.type === 'bomb') score += 60
    if (c.type === 'rocket') score += 500
    if (c.count === hand.length) score -= 1000 // 能一把出完，必出
    if (score < bestScore) {
      bestScore = score
      best = c
    }
  }
  if (!best) return null
  // 只有炸弹/王炸能压，且还不是决胜手 -> 过，保留炸弹
  const onlyHeavy = cands.every((c) => c.type === 'bomb' || c.type === 'rocket')
  if (onlyHeavy && hand.length > 6 && best.count < hand.length) return null
  return best
}

// ---------------- 展示 ----------------

export function comboName(c: Combo): string {
  const rn = (r: number) => RANK_NAMES[r] || String(r)
  const mains = c.main.map(rn).join(' ')
  const ks = c.kickers.map(rn).join(' ')
  switch (c.type) {
    case 'single': return '单张 ' + rn(c.weight)
    case 'pair': return '对子 ' + rn(c.weight)
    case 'triple': return '三张 ' + rn(c.weight)
    case 'triple_single': return '三带一 ' + rn(c.weight) + ' 带 ' + ks
    case 'triple_pair': return '三带二 ' + rn(c.weight) + ' 带对 ' + ks
    case 'straight': return '顺子 ' + mains
    case 'double_straight': return '连对 ' + mains
    case 'airplane': return '飞机 ' + mains
    case 'airplane_single': return '飞机带单 ' + mains + ' 带 ' + ks
    case 'airplane_pair': return '飞机带对 ' + mains + ' 带对 ' + ks
    case 'four_two': return '四带二 ' + rn(c.weight) + ' 带 ' + ks
    case 'four_two_pair': return '四带两对 ' + rn(c.weight)
    case 'bomb': return '炸弹 ' + rn(c.weight)
    case 'rocket': return '王炸'
  }
  return ''
}
