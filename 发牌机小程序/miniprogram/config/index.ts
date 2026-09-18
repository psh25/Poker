// ============================================================
// 全局配置
// BLE 采用课程讲义《网络通信蓝牙》的 Nordic UART Service 标准：
//   Service 6E400001-B5A3-F393-E0A9-E50E24DCCA9E
//   RX 写特征 6E400002-...（手机 -> 底板）
//   TX 通知特征 6E400003-...（底板 -> 手机）
// 数据格式：二进制帧，具体见 utils/protocol.ts（可用 BLE 调试助手直接收发调试）
// ============================================================

export const BLE = {
  SERVICE_UUID: '6E400001-B5A3-F393-E0A9-E50E24DCCA9E',
  RX_UUID: '6E400002-B5A3-F393-E0A9-E50E24DCCA9E',   // 手机 -> 底板（写）
  TX_UUID: '6E400003-B5A3-F393-E0A9-E50E24DCCA9E',   // 底板 -> 手机（通知）
}

// 微信云开发：环境 ID。云环境配好之后填到这里。
// 没配置时小程序自动进入“本地模式”：只记录在本机，不报错。
export const CLOUD = {
  ENV_ID: 'cloud1-d7gr5mffo24846ba1',   // 云开发环境 ID
  COLLECTION: 'games',  // 牌局集合
}

// 实体牌堆位数量：与底板 BottomBoard/include/app_config.h 的 DECK_COUNT(=8) 一致。
// 上传计划时的 pileCount 要报这个数，否则底板会拒绝 4~7 号牌堆（计划校验要求 deck < pileCount）。
export const MAX_PILES = 8
// 单局最多张数：与底板 deal_config.h 的 DEAL_TOTAL_CARDS_MAX(=160) 一致。
export const MAX_CARDS_PER_GAME = 160

// 发牌模式：sequence 中每个数字是"这一张发到哪个牌堆"（0-based）。
//
// ⚠️ 模式的 id **必须与底板方案表一一对应**
//    （BottomBoard/src/deal_config.cpp 的 kPresetNames：
//     0 Doudizhu / 1 Guandan / 2 Shengji / 3 Texas6 / 4 Bridge /
//     5 SortJoker / 6 SortFace / 7 Custom / 8 Test / 9 RotateTest），
//    因为 0x10 选的就是底板方案索引，底板屏幕上的方案名也按它显示。
//
// boardPlan = false：小程序下发完整计划（0x14 开始 → 0x15 分块 → 0x16 提交）
// boardPlan = true ：计划由**底板自己生成**，手机只发 选方案 + 确认 + 发牌
//                    （分拣要靠摄像头按牌面决定落点、旋转测试只转不发，手机都无法表达）
export interface ModeDef {
  id: number
  name: string
  desc: string
  totalCards: number
  sequence: number[]
  boardPlan: boolean
  custom?: boolean
}

function repeatPiles(pattern: number[], cards: number): number[] {
  const out: number[] = []
  while (out.length < cards) out.push(pattern[out.length % pattern.length])
  return out
}

const fillPile = (pile: number, n: number): number[] => new Array(n).fill(pile)

// 斗地主：3 家各 17 张（轮转发）+ 3 张底牌（堆 3）
const DOUDIZHU_SEQUENCE = repeatPiles([0, 1, 2], 51).concat(fillPile(3, 3))
// 掼蛋：4 家各 27 张（两副共 108）
const GUANDAN_SEQUENCE = repeatPiles([0, 1, 2, 3], 108)
// 升级：4 家各 25 张 + 8 张底牌（堆 4）
const SHENGJI_SEQUENCE = repeatPiles([0, 1, 2, 3], 100).concat(fillPile(4, 8))
// 德州 6 人：6 家各 2 张 + 5 张公共牌（堆 6），其余 35 张进弃牌堆（堆 7）
// （与底板参数 6×2 + 公共 5、总牌数 52 一致：余牌也要发完，牌源才算清空）
const TEXAS_SEQUENCE = repeatPiles([0, 1, 2, 3, 4, 5], 12).concat(fillPile(6, 5), fillPile(7, 35))
// 桥牌：4 家各 13 张
const BRIDGE_SEQUENCE = repeatPiles([0, 1, 2, 3], 52)
// 发牌测试：4 堆各 1 张（最短一发，用来验证转盘/推牌链路）
const TEST_SEQUENCE = [0, 1, 2, 3]

export const MODES: ModeDef[] = [
  { id: 0, name: '斗地主', desc: '3 家各 17 张 + 3 张底牌', totalCards: 54, sequence: DOUDIZHU_SEQUENCE, boardPlan: false },
  { id: 1, name: '掼蛋', desc: '4 家各 27 张（两副共 108）', totalCards: 108, sequence: GUANDAN_SEQUENCE, boardPlan: false },
  { id: 2, name: '升级', desc: '4 家各 25 张 + 8 张底牌', totalCards: 108, sequence: SHENGJI_SEQUENCE, boardPlan: false },
  { id: 3, name: '德州6人', desc: '6 家各 2 张 + 公共 5 张（余牌进弃牌堆）', totalCards: 52, sequence: TEXAS_SEQUENCE, boardPlan: false },
  { id: 4, name: '桥牌', desc: '4 家各 13 张', totalCards: 52, sequence: BRIDGE_SEQUENCE, boardPlan: false },
  { id: 5, name: '分拣·大小王', desc: '按牌面把大小王分到另一堆（需摄像头）', totalCards: 0, sequence: [], boardPlan: true },
  { id: 6, name: '分拣·正反面', desc: '按牌面把牌背分到另一堆（需摄像头）', totalCards: 0, sequence: [], boardPlan: true },
  { id: 7, name: '自定义', desc: '手动填写逐张目标牌堆（0~7）', totalCards: 4, sequence: TEST_SEQUENCE, boardPlan: false, custom: true },
  { id: 8, name: '发牌测试', desc: '4 堆各 1 张（最短一发）', totalCards: 4, sequence: TEST_SEQUENCE, boardPlan: false },
  { id: 9, name: '旋转测试', desc: '只转转盘、不发牌（底盘自检）', totalCards: 0, sequence: [], boardPlan: true },
]
