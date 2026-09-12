# OpenMV 扑克牌识别通信协议

适用程序：`main_standalone.py`

## 1. 硬件连接

| 功能 | OpenMV H7 Plus 引脚 | 信号方向 |
|---|---|---|
| 识别触发 | P6 | 外部控制器 → OpenMV |
| UART3 发送 | P4 / TX | OpenMV → 外部控制器 RX |
| UART3 接收 | P5 / RX | 外部控制器 TX → OpenMV，用于校准命令 |
| 信号地 | GND | 两块电路板共地 |

建议外部控制器使用 3.3 V 逻辑电平。

UART3 参数：

```text
波特率：115200
数据位：8
校验位：无
停止位：1
流控：无
```

## 2. P6 触发输入

P6 配置为带内部上拉的输入，采用下降沿触发：

```text
空闲状态：高电平
触发事件：高电平变为低电平
```

推荐触发波形：

```text
P6  ─────────┐      ┌────────────────
             └──────┘
              ≥5 ms
```

触发规则：

- P6 必须先处于高电平，再产生下降沿。
- 开机时P6若已经处于低电平，不会产生识别触发。
- 下降沿被锁存后，程序等待2 ms再开始处理。
- 每次有效触发只执行一次摄像头拍照。
- 角度和二值化补偿均在同一张照片上执行，不会重新拍摄。
- P6恢复高电平并稳定至少5 ms后，程序才允许下一次触发。
- 识别期间到达的新触发会被忽略。
- 外部控制器应等待本次UART结果后，再发送下一次触发。

## 3. UART输入与摄像头校准

OpenMV在空闲主循环中持续接收UART3字节，使用64字节接收缓冲区，并按
CRLF结束的ASCII文本行解析命令。当前只接受：

```text
CALIBRATE\r\n
```

对应十六进制字节：

```text
43 41 4C 49 42 52 41 54 45 0D 0A
```

收到完整命令后，OpenMV执行以下操作：

1. 暂停接受新的P6触发；
2. 点亮红、绿、蓝LED；
3. 开启自动曝光、自动增益和自动白平衡；
4. 等待 `CAMERA_SETTLE_MS`，当前为2000 ms；
5. 读取并锁定新的曝光、增益和RGB增益；
6. 等待 `CAMERA_APPLY_SETTLE_MS`，当前为200 ms；
7. 将参数写入 `camera.json`；
8. 熄灭LED；
9. 通过UART和USB回复结果。

校准成功回复：

```text
RESULT:CALIBRATED\r\n
```

校准或命令解析失败回复：

```text
RESULT:ERROR\r\n
```

校准参数使用 `.new` 和 `.bak` 文件进行可恢复替换。启动时程序会检查并恢复
上次意外断电留下的中间文件。

超过32字节的命令行、非ASCII命令、空行和未知命令都会回复
`RESULT:ERROR\r\n`。不足一整行的数据会保留到后续接收周期继续拼接。

校准命令可能在识别过程中进入UART硬件缓冲区，但必须等识别完成后才会处理。
由于外部控制器当前把“收到任意一行”视为回应，发送 `CALIBRATE` 前必须确认：

- 没有正在进行的P6识别；
- 上一次识别结果已经完整接收；
- UART接收缓冲区中没有旧结果。

否则先到达的普通牌结果可能被误认为校准回应。建议外部控制器仍检查
`RESULT:CALIBRATED` 或 `RESULT:ERROR`，并为校准设置至少3秒超时。

## 4. UART输出协议

每次有效触发，UART3只发送一条结果记录：

```text
RESULT:<识别结果>\r\n
```

外部控制器应读取到换行符 `\n`，然后去除末尾的 `\r\n`。

### 4.1 普通牌

普通牌格式：

```text
RESULT:<花色>_<点数>\r\n
```

花色取值：

```text
heart
diamond
club
spade
```

点数取值：

```text
A
2
3
4
5
6
7
8
9
10
J
Q
K
```

示例：

```text
RESULT:heart_A\r\n
RESULT:club_10\r\n
RESULT:spade_K\r\n
```

普通牌结果可以用以下规则解析：

```text
(heart|diamond|club|spade)_(A|2|3|4|5|6|7|8|9|10|J|Q|K)
```

### 4.2 特殊结果

| UART记录 | 含义 |
|---|---|
| `RESULT:joker_big\r\n` | 红色Joker |
| `RESULT:joker_small\r\n` | 黑色Joker |
| `RESULT:back\r\n` | 牌背 |
| `RESULT:UNKNOWN\r\n` | 没有获得足够可靠的识别结果 |
| `RESULT:CALIBRATED\r\n` | UART摄像头校准成功 |
| `RESULT:ERROR\r\n` | 程序启动或识别过程发生异常 |

当前程序使用以下Joker映射：

```text
红色Joker → joker_big
黑色Joker → joker_small
```

程序没有“无牌”类别。`UNKNOWN`只表示无法可靠分类，不能解释为无牌。

## 5. USB输出

`print()`内容通过USB CDC/REPL输出到连接的电脑。UART和USB的主结果相同。

成功示例：

```text
RESULT:club_10
TIME_MS:320 CAPTURE_MS:75
```

失败示例：

```text
RESULT:UNKNOWN
REASON:RANK_MARGIN=0.031
TIME_MS:480 CAPTURE_MS:74
```

字段含义：

| 字段 | 含义 |
|---|---|
| `RESULT` | 最终识别结果 |
| `REASON` | `UNKNOWN`的简要原因，只通过USB输出 |
| `TIME_MS` | 从P6下降沿到结果输出阶段的总耗时 |
| `CAPTURE_MS` | 本次唯一一次摄像头拍照的耗时 |
| `UART_ERROR` | UART发送失败信息，只通过USB输出 |

UART不会发送 `REASON`、`TIME_MS` 或 `CAPTURE_MS`。

校准成功时USB输出：

```text
RESULT:CALIBRATED
CALIBRATION: {'exposure_us': ..., 'gain_db': ..., 'rgb_gain_db': ...}
```

校准失败时USB输出 `RESULT:ERROR` 和 `CALIBRATE_ERROR`。

## 6. 通信时序

```text
外部控制器                         OpenMV H7 Plus
    │                                   │
    │──── P6下降沿 ────────────────────>│
    │                                   │ 拍摄一次并识别
    │                                   │
    │<──── RESULT:club_10\r\n ──────────│
    │                                   │
    │──── 下一次P6下降沿 ──────────────>│
```

外部控制器建议设置1.2秒的响应超时。如果超时后仍未收到完整的
`RESULT:...\r\n`，应视为OpenMV通信或运行异常。

校准时序：

```text
外部控制器                         OpenMV H7 Plus
    │                                   │
    │──── CALIBRATE\r\n ───────────────>│
    │                                   │ 点亮LED并重新校准
    │                                   │ 锁定并保存参数
    │<──── RESULT:CALIBRATED\r\n ───────│
```

## 7. LED行为

程序使用板载红、绿、蓝LED，三色同时点亮：

- 上电进入程序后点亮1000 ms，然后熄灭；
- P6触发后，拍照前点亮并等待35 ms；
- 唯一一次 `snapshot()` 返回后立即熄灭，图像处理期间保持熄灭；
- 执行UART校准时全程点亮，完成或失败后熄灭；
- 拍照和校准代码使用异常清理，异常发生时也会尝试熄灭LED。

35 ms预亮时间用于避免摄像头取得LED尚未稳定照明的帧，该时间计入
`CAPTURE_MS` 和总识别耗时。

## 8. 启动行为

程序正常启动后，只通过USB输出：

```text
BANK_READY rank=13 suit=4 joker=2 back=1
READY:P6_ACTIVE_LOW UART3_115200 SD_TEMPLATES
CAMERA: {...}
```

当前版本不会通过UART发送启动完成通知。外部控制器应在OpenMV启动和模板加载完成后再发送首次触发。

如果启动失败，程序尝试通过UART发送：

```text
RESULT:ERROR\r\n
```

随后停留在错误状态，不再处理P6触发。

## 9. 当前协议限制

当前UART协议不包含：

- 启动完成通知；
- 忙碌通知；
- 请求或结果序号；
- 校验和；
- 外部控制器应答；
- 除 `CALIBRATE` 以外的UART命令。

正常通信必须遵循“一次触发、等待一条结果、再进行下一次触发”的顺序。

## 10. 部署文件

OpenMV内部Flash：

```text
/flash/main.py
/flash/cards_hybrid_core.py
/flash/cards_fast_config.py
/flash/cards_fast_v1/camera.json
```

SD卡：

```text
/sdcard/cards_fast_v1/templates/
├── rank/
├── suit/
├── joker/
└── back/
```

将项目中的 `main_standalone.py` 复制到OpenMV内部Flash并改名为 `main.py`。
