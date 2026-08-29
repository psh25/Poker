# 底板 ↔ 子板通信协议（2 线 UART）

> 版本：v1.0  
> 关联代码：`BottomBoard/src/protocol.*`、`SubBoard/src/protocol.*`  
> 架构依据：[architecture_v2.md](architecture_v2.md) 第七章 / [subboard_architecture.md](subboard_architecture.md) 第五章  
> 状态：草案（类型与帧结构已定，个别事件的数据字段待定稿）

## 1. 物理层

- **2 线 UART**：TX/RX 交叉连接 + 共地（底板 IO42→子板 RX，底板 IO41←子板 TX）。
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

## 4. 事件（子板 → 底板）

| type | 名称 | data | 说明 |
|------|------|------|------|
| 0x81 | `EVT_READY` | 无 | 上电自检完成 |
| 0x82 | `EVT_CARD_OUT` | 无 | 光敏检测到一张牌发出 |
| 0x83 | `EVT_CARD_VALUE` | 牌序号 / 花色 / 点数（格式待定稿） | 牌面识别结果 |
| 0x84 | `EVT_DEAL_DONE` | 无 | 单张发牌流程完成 |
| 0x85 | `EVT_ERROR_CARD_JAM` | 无 | 光敏超时 / 卡牌 |
| 0x86 | `EVT_ERROR_MOTOR_STALL` | 无 | 发牌电机堵转 |
| 0x87 | `EVT_ERROR_CAM_FAIL` | 无 | 摄像头识别失败 |
| 0x88 | `EVT_ACK` | 原 type + 原 data | 命令确认回执（调试用） |

## 5. 确认机制（ACK）

- 子板每收到一帧**校验通过**的命令：串口打印 `[SUB] RX: cmd=0x.. len=.. data:..`，回发 `EVT_ACK`（数据 = 原 type + 原 data），再打印 `[SUB] TX: ACK cmd=0x..`。
- 底板收到 `EVT_ACK`：串口打印 `[BOT] SUB-ACK: cmd=0x..`，**仅调试，不进业务队列**。
- 示例：底板发 `A5 03 00 3F AA`（查询状态）→ 子板回 `A5 88 01 03 .. AA`（ACK，数据 0x03 表示“确认的是 0x03 命令”）。回显 deal 命令的 ACK 帧为 `A5 88 01 01 48 AA`。

## 6. 调试 CLI（电脑串口 115200）

**底板串口（USB）**输入，回车执行：

```
help                                # 帮助
state                               # 打印底板当前状态机状态
sub <cmd> [hex data...]             # 底板 → 子板（自动组帧 + 自动算 CRC）
sim <type> [hex data...]            # 模拟子板 → 底板事件（本地喂给协议分发）
menu | select <n> | dealing <pct>   # 底板屏幕测试
```

- `cmd` / `type` 支持文本别名或 hex：
  - 命令别名：`deal` / `stop` / `status` / `selftest` / `reset`（0x01~0x05）；
  - 事件别名：`ready` / `cardout` / `cardvalue` / `dealdone` / `jam` / `motorstall` / `camfail` / `ack`（0x81~0x88）；
  - 也可直接写 hex，如 `sub 0x03`。
- 可选 `hex data...`：空格分隔的十六进制数据，如 `sub 0x01 03 04`。
- 别名规则：枚举名去掉 `CMD_` / `EVT_` 前缀后的小写（如 `EVT_DEAL_DONE` → `dealdone`，`EVT_ERROR_MOTOR_STALL` → `motorstall`）。
- `sub` 走 `proto_send()` 自动组帧（含 CRC），**无需手算 len / CRC**；
- `sim` 直接调用 `proto_on_event()`，等价于子板真的发来一帧（且已通过 CRC 校验）。

示例：

```
sub status      → 底板: [CLI] -> SUB type=0x03 ... sent
                  子板: [SUB] RX: cmd=0x03 ... / [SUB] TX: ACK cmd=0x03
                  底板: [BOT] SUB-ACK: cmd=0x03
sim cardout     → 底板模拟收到光敏事件，触发协议分发
select 3        → 屏幕高亮方案 3
```

**子板串口（USB）**同样支持直接注入命令（模拟底板发来）：

```
help | deal | stop | status | selftest | reset | <hex type> [hex data...]
```

输入后子板打印 `[CLI] inject ...`、`[SUB] RX:...`，并正常回发 ACK。

## 7. 新增命令（扩展指南）

- **类型空间**：命令 `0x01~0x0F`（已用 0x01~0x05）、事件 `0x81~0x8F`（已用 0x81~0x88）；`0x10~0x7F`、`0x90~0xFF` 预留。
- **载荷**：每帧最多 32 字节（`PROTO_MAX_DATA`），CRC 自动计算。

新增一条命令只需 4 步：

1. 在**两端** `protocol.h` 的枚举里加新 type（命令加进 `proto_cmd_t`，事件加进 `proto_evt_t`）；
2. 在子板 `state_machine.cpp` 的 `sub_state_handle_command()` 加对应 `case`；
3. （可选）在底板 `tasks.cpp` 的调试 CLI 里加文本映射；
4. 如需带数据，约定 data 字段含义并写入本表。

> 载荷超过 32 字节时：两端同步增大 `PROTO_MAX_DATA`（`protocol.h`），或拆成多帧由业务层组合。

## 9. 命名约定与缩写

- **CMD** = Command（命令）：底板 → 子板，枚举 `CMD_*`，取值 0x01~0x0F。
- **EVT** = Event（事件）：子板 → 底板，枚举统一 `EVT_*`（错误事件为 `EVT_ERROR_*`），取值 0x81~0x8F。
- **ACK** = Acknowledgment（确认 / 应答）：`EVT_ACK`，子板收到合法命令后回发的回执。
- **CLI 别名** = 枚举名去掉前缀后的小写（如 `CMD_STATUS_QUERY` → `status`，`EVT_ERROR_MOTOR_STALL` → `motorstall`）。
- 串口打印前缀统一：底板 `[BOT]`，子板 `[SUB]`，调试命令行 `[CLI]`。

## 8. 常见问题

- **收不到 ACK**：检查波特率（115200）、接线（TX↔RX 交叉、共地）、两端 CRC 是否一致。
- **乱码 / 丢帧**：杜邦线尽量短、远离电机线；确认无干扰源后仍乱码可降低波特率。
