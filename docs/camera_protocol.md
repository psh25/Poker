# 摄像头（OpenMV）→ 子板：识别结果接口

> 摄像头端用的是 OpenMV 现有程序（`main_standalone.py`）的原生格式：**ASCII 文本行**，不需要改摄像头代码。
> 子板负责把它翻译成本项目内部的牌面编码，再用**板间二进制帧**（`0xA5 | type | len | data | crc8 | 0xAA`）发给底板。
>
> 两段链路各用各的格式：
>
> | 链路 | 格式 | 说明 |
> |---|---|---|
> | 摄像头 → 子板 | `RESULT:<结果>\r\n` 文本行 | 简单、无校验、无序号；靠“一次触发一条结果”保证顺序 |
> | 子板 → 底板 | 板间二进制帧 | 带 CRC8，见 [board_protocol.md](board_protocol.md) |

## 1. 硬件连接

| 功能 | OpenMV H7 Plus | 子板 | 方向 |
|---|---|---|---|
| 识别触发 | P6 | `PIN_CAM_TRIG`（GPIO20） | 子板 → 摄像头 |
| UART 发送 | P4 / TX | `PIN_CAM_RX`（GPIO46） | 摄像头 → 子板 |
| UART 接收 | P5 / RX | `PIN_CAM_TX`（GPIO10） | 子板 → 摄像头（**只用来下发指令**，如开机自检的校准，见第 7 节） |
| 地 | GND | GND | **必须共地** |

UART 参数：**115200，8N1，无流控**。逻辑电平 3.3V。

## 2. 触发时序（注意是下降沿）

OpenMV 的 P6 是**下降沿触发**、内部上拉：

```
P6  ────────┐           ┌────────────
            └───────────┘
              ≥5 ms 低电平
   空闲=高              空闲=高（稳定 ≥5ms 后才允许下一次触发）
```

| 项 | 值 |
|---|---|
| 空闲电平 | **高** |
| 触发 | 高 → 低（下降沿） |
| 低电平宽度 | ≥5ms（子板用 `CAM_TRIG_PULSE_MS`，当前 200ms） |
| 两次触发间隔 | 摄像头识别期间的新触发会被忽略；子板是“触发 → 等结果 → 再触发”，天然满足 |
| 上电要求 | P6 必须先处于高电平，再产生下降沿才会触发 |

子板实现：`hardware.cpp` 的 `sub_camera_trigger()`（拉低）与 `sub_camera_service()`（`CAM_TRIG_PULSE_MS` 后恢复高）。

## 3. 摄像头回传格式（每行一条结果）

```
RESULT:<结果>\r\n
```

| UART 记录 | 含义 |
|---|---|
| `RESULT:heart_A` | 普通牌：`<花色>_<点数>` |
| `RESULT:club_10` | 同上（点数为 10 时是两位数） |
| `RESULT:spade_K` | 同上 |
| `RESULT:joker_big` | 红色 Joker（大王） |
| `RESULT:joker_small` | 黑色 Joker（小王） |
| `RESULT:back` | 牌背 |
| `RESULT:UNKNOWN` | 识别置信度不足（**不是“无牌”**） |
| `RESULT:ERROR` | 摄像头程序启动或识别过程异常 |

花色取值：`spade` / `heart` / `club` / `diamond`
点数取值：`A` `2` … `10` `J` `Q` `K`

> 摄像头还会通过 USB 额外输出 `REASON` / `TIME_MS` / `CAPTURE_MS`（只在电脑上看），UART 上只有 `RESULT:` 这一行。
> 响应超时建议 **1.2s**（子板用 `CAM_RESULT_TIMEOUT_MS = 1200`）。

## 4. 文本 → 内部牌面编码

子板的 `cam_text_to_card()` 按下面的规则翻译（编码定义见 [protocol.h](../SubBoard/include/protocol.h)）：

| OpenMV 文本 | card 编码 | 含义 |
|---|---|---|
| `spade_A` … `spade_K` | 0 ~ 12 | 黑桃 A…K |
| `heart_A` … `heart_K` | 13 ~ 25 | 红桃 A…K |
| `club_A` … `club_K` | 26 ~ 38 | 梅花 A…K |
| `diamond_A` … `diamond_K` | 39 ~ 51 | 方块 A…K |
| `joker_small` | 52 | 小王 |
| `joker_big` | 53 | 大王 |
| `back` | 54 | 牌背 |
| `UNKNOWN` | 55（`src` = 低置信度） | 没认出来 |
| `ERROR` | — | 不走 `EVT_CARD_VALUE`，直接上报 `EVT_ERROR_CAM_FAIL` |

点数编号：`A=0, 2=1, 3=2, …, 10=9, J=10, Q=11, K=12`；`code = 花色编号 × 13 + 点数`
（花色编号：黑桃 0、红桃 1、梅花 2、方块 3）。

## 5. 子板 → 底板（回顾）

子板翻译完成后，用板间协议上报，`data = [card, src]`：

```
A5 83 02 <card> <src> <crc8> AA
```

| src | 含义 |
|---|---|
| 0 | 摄像头正常识别 |
| 1 | 超时没收到结果（子板兜底，默认按“未知牌”继续） |
| 2 | 低置信度（摄像头回 `UNKNOWN`） |
| 3 | 调试/模拟 |

示例（黑桃A）：`A5 83 02 00 00 DD AA`；大王：`A5 83 02 35 00 65 AA`。

## 6. 子板行为小结

> **串口日志约定**：子板把摄像头**发来的整行原文**直接回显（前缀统一 `[CAM] `），
> 不再打印解码后的 `card/src`——排查时能一眼看到摄像头到底发了什么。
> 解码结果只进板间协议帧（`EVT_CARD_VALUE` 的 `[card, src]`），不占串口日志。

| 情况 | 子板动作 | 串口打印 |
|---|---|---|
| 收到合法 `RESULT:<普通牌>` | 发 `EVT_CARD_VALUE [card, 0]` | `[CAM] RESULT:spade_A`（按收到的原文回显） |
| 收到 `RESULT:UNKNOWN` | 发 `EVT_CARD_VALUE [55, 2]` | `[CAM] RESULT:UNKNOWN` |
| 收到 `RESULT:ERROR` | 发 `EVT_ERROR_CAM_FAIL`（底板停机报警） | `[CAM] RESULT:ERROR -> report CAM fail` |
| 1.2s 内没收到结果 | 由 `CAM_EMPTY_ON_TIMEOUT` 决定：默认发 `[55, 1]` 继续；改为 0 则报 `EVT_ERROR_CAM_FAIL` | `[CAM] timeout -> UNKNOWN src=TIMEOUT (debug)` |
| 收到不认识的文本 | 丢弃并继续等 | `[CAM] cannot parse: xxx` / `[CAM] ignore line: xxx` |

## 7. 子板 → 摄像头：校准指令（开机自检用）

反方向（子板 `PIN_CAM_TX` → 摄像头 RX / P5）用**同一根 UART、同一套格式风格**：ASCII 文本行 + `\r\n`。

```
CALIBRATE\r\n
```

| 项 | 值 |
|---|---|
| 编码 | 纯 ASCII 大写，**CRLF（`\r\n`）结尾** —— 与 `RESULT:...\r\n` 完全同风格 |
| 串口参数 | 115200，8N1，无流控（与回传一致） |
| 何时发 | ① **开机自检时发一次**（子板 `sub_camera_calibrate()`）；② 子板串口敲 **`camcalib`** 手动发一次；③ 底板 `sub selftest` / 子板 `selftest` 复检时也会发 |
| 之后 | 子板等 `CAM_CALIB_WAIT_MS`(1.5s) 收回应；收到任意一行即视为“已回应” |
| 摄像头建议回 | 成功回一行 `CAL:OK\r\n`（内容不限）；校准失败可回 `RESULT:ERROR\r\n`，子板按“校准失败”处理 |

> ⚠️ **摄像头端目前不读 UART**：`重要信息/STANDALONE_IO_PROTOCOL(1).md` 第 7 节写明当前
> `main_standalone.py` 的 UART 只有输出、没有输入。所以本指令**要在摄像头端加接收处理后才生效**；
> 在那之前子板自检只会打印 `[CAM] calibrate: no reply`，并让 `SUB_ST_CAM_CALIB` 位保持 0（不算失败）。

### 7.1 为什么“先校准、再转电机”

自检要转一下发牌电机（确认驱动链路），但**电机一转牌就会错位**，摄像头按当前画面做的校准就白做了。
所以子板自检的顺序固定为：

```
读光电门电平 → 发 CALIBRATE（等 CAM_CALIB_WAIT_MS）→ 电机微动 → 再看一次光电门 → 查与底板串口
```

其中电机微动的正转时长用 `SELFTEST_MOTOR_FWD_MS`(150ms) 而**不是**出牌时长 `MOTOR_FWD_MS`(390ms)，
确保只抖一下、不会真的把牌发出去；反转用 `SELFTEST_MOTOR_REV_MS`(300ms) 把可能被推出来的牌退回。

要改指令内容，只改子板 `SubBoard/include/app_config.h` 的 `CAM_CMD_CALIBRATE`
（保持“纯 ASCII + `\r\n` 结尾”即可），并同步摄像头端。

### 7.2 联调用的手动命令（子板串口）

```
camcalib        # 向摄像头发一次 CALIBRATE，等 1.5s 并打印摄像头回的任何一行
```

输出示例（摄像头端已实现接收时）：

```
[CAM] TX cmd (11 bytes): 43 41 4C 49 42 52 41 54 45 0D 0A
[CAM] calibrate: send command, then wait for reply...
[CAM] calib rx: CAL:OK
[CAM] calibrate: reply received
[CLI] camcalib: camera replied
```

摄像头端没实现接收时，只会看到 `[CAM] calibrate: no reply ...` 与 `[CLI] camcalib: no reply / error`——
**这不是子板故障**，是摄像头端还没读 UART。

> 该命令是**阻塞**的（约 `CAM_CALIB_WAIT_MS` = 1.5s），只用于调试；
> 若当时正好有一个截图会话没结束，子板会先取消它再发校准（避免两种回应混在一起）。

## 8. 联调建议

1. **先单独测摄像头**：USB-TTL 接 OpenMV 的 P4(TX)，串口助手 115200 看有没有 `RESULT:...`；把 P6 用杜邦线碰一下 GND（制造下降沿）即可触发一次。
2. **再接子板**：观察子板串口是否打印 `[CAM] RESULT:...`（子板直接回显摄像头原文）；没有就查共地和 TX/RX 是否接反。
3. 子板打印 `cannot parse` → 检查摄像头输出的花色拼写（必须是 `spade/heart/club/diamond`）。
4. 子板打印 `timeout` → 确认 P6 的下降沿真的产生了（空闲必须是高电平），以及 OpenMV 模板是否加载成功。
5. 全链路：底板串口应能看到牌面事件，`dealinfo` 里能看到进度推进。
6. **校准指令**：不用等重启，子板串口敲 `camcalib` 即可手动验证；摄像头端加好接收处理后应打印
   `[CAM] TX cmd (11 bytes): 43 41 4C 49 42 52 41 54 45 0D 0A` 与 `[CAM] calib rx: ...`。
   开机自检路径下底板串口则打印 `SUB-READY: bits=0x....| cam-calib`。
