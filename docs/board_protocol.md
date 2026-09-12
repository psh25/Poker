# 底板 ↔ 子板通信协议（2 线 UART）

> 版本：v1.0  
> 关联代码：`BottomBoard/src/protocol.*`、`SubBoard/src/protocol.*`  
> 架构依据：[architecture_v2.md](architecture_v2.md) 第七章 / [subboard_architecture.md](subboard_architecture.md) 第五章  
> 状态：草案（类型与帧结构已定，个别事件的数据字段待定稿）

## 1. 物理层

- **2 线 UART**：滑环 POS/NEG 各一线 + 共地（底板 POS=IO45 TX → 子板 POS=IO38 RX；底板 NEG=IO48 RX ← 子板 NEG=IO39 TX）。
- 波特率 **115200**，8N1。
- 电机启停干扰大：帧带 CRC8，校验失败整帧丢弃；数据包建议分小段发送。

## 2. 帧格式（每个字节的含义）

| 偏移 | 字段 | 长度 | 含义 |
|------|------|------|------|
| 0 | 帧头 | 1 | 固定 `0xA5`，标志一帧开始 |
| 1 | type | 1 | 命令 / 事件类型（见第 3、4 节） |
| 2 | len | 1 | 数据长度，0~32 |
| 3 ~ 3+len-1 | data | len | 载荷（无数据时省略） |
| 3+len | crc8 | 1 | 对 `type + len + data` 的 CRC-8 校验 |
| 4+len | 帧尾 | 1 | 固定 `0xAA`，标志一帧结束 |

最小帧（无数据）共 5 字节：

```
A5 | type | 00 | crc8 | AA
```

CRC-8 算法（多项式 0x07，初值 0x00，与两端代码一致）：

```
crc = 0
for b in [type, len] + data:
    crc ^= b
    for i in 0..7:
        if crc 最高位为 1: crc = (crc << 1) ^ 0x07
        else:            crc = crc << 1
        只保留低 8 位
```

## 3. 命令（底板 → 子板）

| type | 名称 | data | 说明 | 完整帧示例 |
|------|------|------|------|------------|
| 0x01 | `CMD_DEAL_START` | 无 | 从牌堆发出一张牌 | `A5 01 00 15 AA` |
| 0x02 | `CMD_STOP` | 无 | 停止 / 急停 | `A5 02 00 2A AA` |
| 0x03 | `CMD_STATUS_QUERY` | 无 | 查询状态 | `A5 03 00 3F AA` |
| 0x04 | `CMD_SELF_TEST` | 无 | 触发自检 | `A5 04 00 54 AA` |
| 0x05 | `CMD_RESET` | 无 | 复位状态机（错误恢复） | `A5 05 00 41 AA` |
| 0x06 | `CMD_CAM_CAPTURE` | 无 | 触发摄像头截图（子板拉高 PIN_CAM_TRIG） | `A5 06 00 62 AA` |

## 4. 事件（子板 → 底板）

| type | 名称 | data | 说明 |
|------|------|------|------|
| 0x81 | `EVT_READY` | 无 | 上电自检完成 |
| 0x82 | `EVT_CARD_OUT` | 无 | 光敏检测到一张牌发出 |
| 0x83 | `EVT_CARD_VALUE` | `[card, src]`：card = 牌面编码 0~55（见 4.1），src = 来源 | 牌面识别结果 |
| 0x84 | `EVT_DEAL_DONE` | 无 | 单张发牌流程完成 |
| 0x85 | `EVT_ERROR_CARD_JAM` | 无 | 光敏超时 / 卡牌 |
| 0x86 | `EVT_ERROR_MOTOR_STALL` | 无 | 发牌电机堵转 |
| 0x87 | `EVT_ERROR_CAM_FAIL` | 无 | 摄像头识别失败 |
| 0x88 | `EVT_ACK` | 原 type + 原 data | 命令确认回执（调试用） |
| 0x89 | `EVT_STATUS` | [state, error, countLo, countHi] | 状态回执：心跳应答（`CMD_STATUS_QUERY` 的响应，不进业务队列） |

### 4.1 牌面编码（`EVT_CARD_VALUE` 的 data）

56 类 = 54 张牌 + 背面 + 未知；花色顺序 **黑桃 红桃 梅花 方块**，`code = 花色×13 + 点数`。

| 值 | 含义 |
|----|------|
| 0~12 | 黑桃 A,2,3,4,5,6,7,8,9,10,J,Q,K |
| 13~25 | 红桃 A…K |
| 26~38 | 梅花 A…K |
| 39~51 | 方块 A…K |
| 52 | 小王 |
| 53 | 大王 |
| 54 | 背面（识别到牌背） |
| 55 | 未知（没认出来 / 兜底） |

点数编号：`A=0, 2=1, 3=2, …, 10=9, J=10, Q=11, K=12`。

`src`（来源）：`0`=摄像头正常识别、`1`=识别超时（子板兜底）、`2`=低置信度、`3`=调试/模拟。

示例帧：黑桃A（card=0,src=0）= `A5 83 02 00 00 DD AA`；大王（card=53,src=0）= `A5 83 02 35 00 65 AA`；
未知+低置信度（card=55,src=2）= `A5 83 02 37 02 41 AA`。

> 摄像头 → 子板走的是**另一套更简单的格式**（OpenMV 原生 ASCII 文本行 `RESULT:...`，无校验无序号），
> 由子板翻译成上面的编码后再用板间帧上报；两段链路的分工见 [camera_protocol.md](camera_protocol.md)。

## 5. 确认机制（ACK）

- 子板每收到一帧**校验通过**的命令：串口打印 `[SUB] RX: cmd=0x.. len=.. data:..`，回发 `EVT_ACK`（数据 = 原 type + 原 data），再打印 `[SUB] TX: ACK cmd=0x..`。
- 底板收到 `EVT_ACK`：串口打印 `[BOT] SUB-ACK: cmd=0x..`，**仅调试，不进业务队列**。
- 子板侧用串口日志直观观察链路：收到字节与解析结果会打印 `[SUB] RX: ...`，回发成功打印 `[SUB] TX: ACK ...`。
- 示例：底板发 `A5 03 00 3F AA`（查询状态）→ 子板回 `A5 88 01 03 .. AA`（ACK，数据 0x03 表示“确认的是 0x03 命令”）。回显 deal 命令的 ACK 帧为 `A5 88 01 01 48 AA`。

## 6. 调试 CLI（电脑串口 115200）

**底板串口（USB）**输入，回车执行：

```
help                                # 帮助
state                               # 打印底板当前状态机状态
dealstart | stop | reset | confirm  # 启动发牌 / 停机 / 复位 / 确认方案
subboard                            # 打印子板在线状态与心跳时间
sub <cmd> [hex data...]             # 底板 → 子板（自动组帧 + 自动算 CRC）
sim <type> [hex data...]            # 模拟子板 → 底板事件（本地喂给协议分发）
idle | select <n> | dealing <pct>   # 底板屏幕测试
setstate <idle|dealing|active>      # 强制切换底板状态机（调试）
game list | game info               # 列出牌局参数 / 当前计划（按当前发牌方式生成）
game use <1-8|name>                 # 选择预置牌局（斗地主/掼蛋/升级/德州6人/桥牌/测试等）
game order [seq|rand]               # 查看/设置发牌方式（同 IDLE 长按编码器）
game custom players=N hand=N [public=N] [bottom=N] [total=N]   # 设置 Custom 参数
game random players=N hand=N [public=N] [bottom=N] [total=N]   # 同上并切到随机发牌
```

- `cmd` / `type` 支持文本别名或 hex：
  - 命令别名：`dealstart` / `stop` / `statusquery` / `selftest` / `reset` / `camcapture`（0x01~0x06）；
  - 事件别名：`ready` / `cardout` / `cardvalue` / `dealdone` / `errorcardjam` / `errormotorstall` / `errorcamfail` / `ack` / `status`（0x81~0x89）；
  - 也可直接写 hex，如 `sub 0x03`。
- 可选 `hex data...`：空格分隔的十六进制数据，如 `sub 0x01 03 04`。
- 别名规则（两端统一）：枚举名**去掉前缀**（`CMD_` / `EVT_`）→ **全部小写** → **去掉下划线**。
  例：`CMD_DEAL_START` → `dealstart`、`CMD_STATUS_QUERY` → `statusquery`、`EVT_ERROR_MOTOR_STALL` → `errormotorstall`。
- `sub` 走 `proto_send()` 自动组帧（含 CRC），**无需手算 len / CRC**；
- `sim` 直接调用 `proto_on_event()`，等价于子板真的发来一帧（且已通过 CRC 校验）。

示例：

```
sub status      → 底板: [CLI] -> SUB type=0x03 ... sent
                  子板: [SUB] RX: cmd=0x03 ... / [SUB] TX: ACK cmd=0x03
                  底板: [BOT] SUB-ACK: cmd=0x03
sim cardout     → 底板模拟收到光敏事件，触发协议分发
sub camcapture  → 子板拉高 PIN_CAM_TRIG 触发一次摄像头截图
select 3        → 屏幕高亮方案 3
game random players=3 hand=17 bottom=3 total=54  → 设 Custom 参数 + 切到随机，选中 Custom
game order rand → 只切换发牌方式（等效 IDLE 下长按编码器）
game info       → 打印当前计划的发牌组（牌堆/张数/标签）
```

**子板串口（USB）**同样支持直接注入命令（模拟底板发来）：

```
help | dealstart | stop | statusquery | selftest | reset | camcapture | auto [n|off] | mtest | <hex type> [hex data...]
```

输入后子板打印 `[CLI] inject ...`、`[SUB] RX:...`，并正常回发 ACK。

## 7. 新增命令（扩展指南）

- **类型空间**：命令 `0x01~0x0F`（已用 0x01~0x06）、事件 `0x81~0x8F`（已用 0x81~0x88）；`0x10~0x7F`、`0x90~0xFF` 预留。
- **载荷**：每帧最多 32 字节（`PROTO_MAX_DATA`），CRC 自动计算。

新增一条命令只需 4 步：

1. 在**两端** `protocol.h` 的枚举里加新 type（命令加进 `proto_cmd_t`，事件加进 `proto_evt_t`）；
2. 在子板 `state_machine.cpp` 的 `sub_state_handle_command()` 加对应 `case`；
3. （可选）在底板 `tasks.cpp` 的调试 CLI 里加文本映射；
4. 如需带数据，约定 data 字段含义并写入本表。

> 载荷超过 32 字节时：两端同步增大 `PROTO_MAX_DATA`（`protocol.h`），或拆成多帧由业务层组合。

## 8. 命名约定与缩写

- **CMD** = Command（命令）：底板 → 子板，枚举 `CMD_*`，取值 0x01~0x0F。
- **EVT** = Event（事件）：子板 → 底板，枚举统一 `EVT_*`（错误事件为 `EVT_ERROR_*`），取值 0x81~0x8F。
- **ACK** = Acknowledgment（确认 / 应答）：`EVT_ACK`，子板收到合法命令后回发的回执。
- **CLI 别名** = 枚举名去前缀 → 全小写 → 去下划线（如 `CMD_STATUS_QUERY` → `statusquery`，`EVT_ERROR_MOTOR_STALL` → `errormotorstall`）。
- 串口打印前缀统一：底板 `[BOT]`，子板 `[SUB]`，调试命令行 `[CLI]`。

## 9. 常见问题

- **收不到 ACK**：检查波特率（115200）、接线（POS/NEG 对应连接、共地）、两端 CRC 是否一致。
- **乱码 / 丢帧**：检查串口是否被其他线占用。
- **两板串口必须共地**：POS/NEG 之外还要连 GND，否则接收端会把噪声当数据（表现为持续乱码）。
- **不要占用 UART0**：板间通信用专用引脚（底板 IO45/48、子板 IO38/39），GPIO43/44 是 CH340 调试口；烧录前先断开两板间的 RX/TX 线。

## 10. 主机远程控制层（BLE 小程序 / 串口 CLI ↔ 底板）

> 目标：**同一功能在三种场景下一致**——小程序 BLE、电脑串口 CLI、底板↔子板 UART。
> 帧格式统一为 `0xA5 | type | len | data | crc8 | 0xAA`；BLE 与板间走二进制帧，
> CLI 文本命令是同一动作的可读别名（见下方对应表）。

### 10.1 BLE 通道（参考讲义《3-网络通信蓝牙》Nordic UART Service）

- Service：`6E400001-B5A3-F393-E0A9-E50E24DCCA9E`
- RX 特征（小程序写入，命令帧）：`6E400002-B5A3-F393-E0A9-E50E24DCCA9E`
- TX 特征（底板 notify，回执/状态帧）：`6E400003-B5A3-F393-E0A9-E50E24DCCA9E`
- 设备名：`DealerBot`；底板收到 RX 帧后用与板间同一套解析器处理。

### 10.2 主机 → 底板命令

| type | 含义 | data | 与 CLI 对应 |
|------|------|------|-------------|
| 0x01 | 启动发牌（复用板间 `CMD_DEAL_START` 语义） | 无 | `dealstart` |
| 0x02 | 停机：发子板 `CMD_STOP` + 底板回 IDLE | 无 | `stop` |
| 0x03 | 状态查询（回 0x91） | 无 | `sub statusquery` |
| 0x04 | 触发子板自检（转发） | 无 | `sub selftest` |
| 0x05 | 复位：发子板 `CMD_RESET` + 底板回 IDLE | 无 | `reset` |
| 0x10 | 选择方案（仅 IDLE 有效） | [0]=0~7 | `select N` / `game use N` |
| 0x11 | 确认方案（两段式第一步） | 无 | `confirm` |

> 心跳：底板监控任务每 `COMM_HEARTBEAT_MS`（1s）发一次板间 `CMD_STATUS_QUERY`（0x03），
> 子板回 `EVT_STATUS`（0x89）；`COMM_DEAD_TIMEOUT_MS`（3s）内没有任何子板帧即判定掉线（`[MON] sub board OFFLINE`）。

### 10.3 底板 → 主机事件

| type | 含义 | data |
|------|------|------|
| 0x90 | ACK 回执（镜像 EVT_ACK：原 type + 原 data） | 原 type + 原 data |
| 0x91 | 状态通知（状态切换时 notify） | [state, scheme, confirmed] |

### 10.4 使用示例（小程序）

1. 扫描并连接 `DealerBot`，订阅 TX 特征（notify）；
2. 远程选方案 4（TEST）：写 `0x10 03`；确认：写 `0x11`；
3. 远程发牌：写 `0x01`（随后底板向子板发 `CMD_DEAL_START`）；
4. 远程停机/重置：写 `0x02` / `0x05`；
5. 每次命令都会收到 `0x90` 回执；状态变化时收到 `0x91`。

> 自定义牌局参数目前通过串口 `game custom ...` 下发；后续小程序实现时复用同一参数模型（新增一条主机命令帧即可，不需要改发牌逻辑）。

> 注：0x01~0x05 与第 3 节“底板→子板命令”同号。区别只在于执行者：
> 主机发到**底板**，底板执行对应底板级动作并自行决定是否转发子板；板间帧是**底板**发给**子板**。
> 因此“发牌”这一功能在小程序、串口、板间三个场景下语义一致。
