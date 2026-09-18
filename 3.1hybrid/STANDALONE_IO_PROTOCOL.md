# OpenMV 扑克牌识别通信协议

适用程序：`main_standalone.py`

## 1. 硬件连接

| 功能 | OpenMV H7 Plus 引脚 | ESP32-S3子板引脚 | 信号方向 |
|---|---|---|---|
| 识别触发 | P6 | GPIO20 / `PIN_CAM_TRIG` | 子板 → OpenMV |
| UART3 发送 | P4 / TX | GPIO46 / `PIN_CAM_RX` / Serial2 RX | OpenMV → 子板 |
| UART3 接收 | P5 / RX | GPIO10 / `PIN_CAM_TX` / Serial2 TX | 子板 → OpenMV，用于校准命令 |
| 信号地 | GND | GND | 两块电路板共地 |

建议外部控制器使用 3.3 V 逻辑电平。

TX必须接对方RX，不能TX接TX。ESP32-S3的 `Serial2` 与OpenMV的 `UART3`
是两端各自的硬件编号，不需要相同。

注意：子板当前GPIO20同时是ESP32-S3原生USB的D+。子板代码仅在首次截图时
将其配置为触发输出，配置后原生USB日志可能中断。相机UART不使用该USB链路；
联调截图应从子板的CH340/UART0观察日志，不要将USB断连误判为相机UART故障。

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

子板当前实际低脉冲为 `CAM_TRIG_PULSE_MS = 200` ms，而不是5 ms。
5 ms是OpenMV端的最低建议值；更改子板脉冲必须同步核对触发滤波和重新使能时序。

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

当前子板实现的计时起点是**200 ms低脉冲结束、恢复高电平并进入CAM_WAIT时**。
因此一次截图会话的无结果超时约为 `200 + 1200 = 1400` ms，加上主循环调度误差。
CAM_PULSE期间不解析相机结果，提前到达的结果保存在Serial2接收缓冲区，进入
CAM_WAIT后再处理。OpenMV的900 ms处理预算仍从P6下降沿开始，并预留35 ms用于输出。

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
READY:P6_ACTIVE_LOW UART3_115200 FLASH_TEMPLATES
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
/flash/cards_fast_v1/templates/
├── rank/
├── suit/
├── joker/
└── back/
```

将项目中的 `main_standalone.py` 复制到OpenMV内部Flash并改名为 `main.py`。
模板采集、识别、相机配置均使用内部Flash，无需插入SD卡。若旧模板只在SD卡上，
需先将完整的PGM/JSON文件对复制到上述Flash目录；程序不会自动迁移。

## 11. ESP32-S3子板集成信息

本节根据子板的实际源码核对，不能仅凭 `main.cpp` 中的调用或旧注释推断参数：

- `code/SubBoard/src/main.cpp`：启动时调用硬件初始化和自检；主循环调用
  `sub_camera_service()`；调试CLI提供 `camcalib`、`camcapture`。
- `code/SubBoard/include/pins_config.h`：GPIO10/TX、GPIO46/RX、GPIO20/TRIG。
- `code/SubBoard/include/app_config.h`：波特率、命令、脉冲和超时。
- `code/SubBoard/src/hardware.cpp`：Serial2初始化、校准收发、截图会话和结果解析。
- `code/SubBoard/include/protocol.h`：子板向底板转发的牌面编码。

### 11.1 当前参数对照

| 项目 | 子板配置或实现 | OpenMV配置或实现 |
|---|---|---|
| 相机UART | Serial2，115200，8N1 | UART3，115200，8N1 |
| 触发 | GPIO20，空闲高，低脉冲200 ms | P6，下降沿锁存，2 ms滤波 |
| 结果等待 | 脉冲结束后1200 ms | 下降沿起900 ms预算，输出预留35 ms |
| 校准指令 | `CALIBRATE\r\n`，11字节 | 按LF分行，移除末尾CR，识别CALIBRATE |
| 校准等待 | `CAM_CALIB_WAIT_MS = 3000` ms | 自动稳定2000 ms + 应用稳定200 ms + 文件写入/发送 |
| 接收行上限 | 40字节数组，最多39个正文字符 | 命令正文最多32字节，RX缓冲64字节 |
| 结果记录 | 解析 `RESULT:<label>` | CRLF结束，UART不发送调试信息 |

`UART_TIMEOUT_MS = 25`是OpenMV端UART读写相关超时，不是相机识别或校准时长。
最长正常结果 `RESULT:diamond_10\r\n` 为19字节，115200/8N1线上传输约1.65 ms；
实际调用耗时还包含缓冲、flush和调度，因此识别耗时主要不在UART。

### 11.2 校准与启动注意事项

子板 `sub_camera_calibrate()`会先取消当前截图会话、清空Serial2旧接收数据，
发送CALIBRATE，然后循环等待完整的3000 ms，**即使提前收到回应也不会提前返回**。
它将任意非空行（不以 `RESULT:ERROR` 开头）记为收到回应；当前OpenMV正常回复
`RESULT:CALIBRATED\r\n`，并不回复旧文档举例的 `CAL:OK`。

子板源码和 `code/docs/camera_protocol.md` 中“相机目前不读UART”“等待1.5秒”的
注释已落后于当前两端实现：本相机程序已经处理CALIBRATE，子板实际等待3秒。
此处仅记录差异，没有修改ESP32工程。

OpenMV启动需要LED提示、相机初始化和模板加载，而UART没有READY握手。两板同时
上电时，子板首次自检可能在OpenMV就绪前已经发出CALIBRATE，导致3秒等待仍失败。
应先让OpenMV启动就绪，再启动子板；或者相机就绪后从子板CLI手动执行 `camcalib`。
若未来要求同时上电可靠自检，需在两端共同增加READY/重试与命令类型校验，不能只
给OpenMV额外发送一行READY：当前子板会把任意一行误当校准成功。

### 11.3 识别结果如何转发给底板

相机UART只发送ASCII，**不发送A5/AA二进制帧、不发送CRC、不直接发送牌面编号**。
子板解析后转成 `EVT_CARD_VALUE = 0x83`，载荷为 `[card, src]`，再通过与底板的
另一条Serial1链路发送。这与相机Serial2不是同一串口。

| 相机结果 | 子板card | src | 后续行为 |
|---|---|---|---|
| spade_A～spade_K | 0～12 | 0 | EVT_CARD_VALUE |
| heart_A～heart_K | 13～25 | 0 | EVT_CARD_VALUE |
| club_A～club_K | 26～38 | 0 | EVT_CARD_VALUE |
| diamond_A～diamond_K | 39～51 | 0 | EVT_CARD_VALUE |
| joker_small | 52 | 0 | EVT_CARD_VALUE |
| joker_big | 53 | 0 | EVT_CARD_VALUE |
| back | 54 | 0 | EVT_CARD_VALUE |
| UNKNOWN | 55 | 2（低置信度） | EVT_CARD_VALUE，不表示无牌 |
| ERROR | 不发送牌面编号 | — | EVT_ERROR_CAM_FAIL = 0x87 |
| 超时，无UART结果 | 55 | 1（超时） | 当前CAM_EMPTY_ON_TIMEOUT=1，仍发EVT_CARD_VALUE |

普通牌编号为 `suit * 13 + rank`，suit顺序为黑桃、红桃、梅花、方块；
rank顺序为A、2～10、J、Q、K。不要依照相机模板目录顺序生成编号。

子板STOP/RESET只取消子板等待并丢弃当时的旧数据，没有向相机发送UART取消指令。
相机仍可能完成并发送旧结果。因为协议没有请求序号，取消后不要立即开始下一次
识别，应等待相机原会话结束后再触发，避免迟到结果混入新会话。

### 11.4 最小联调步骤

1. 连接P4→GPIO46、P5←GPIO10、P6←GPIO20和GND。保持P6空闲高。
2. 在OpenMV上运行当前 `main_standalone.py`，确认USB端出现READY；不要部署
   没有UART的 `recognize_cards_hybrid.py` 代替它。
3. 使用子板CH340调试串口，波特率115200，输入 `camcalib` 并以换行结束。
4. 子板应打印发送字节 `43 41 4C 49 42 52 41 54 45 0D 0A`，随后打印
   `[CAM] calib rx: RESULT:CALIBRATED` 和 `[CLI] camcalib: camera replied`。
5. 将牌放稳，输入 `camcapture`。该CLI命令在子板内部注入CMD_CAM_CAPTURE，
   然后由GPIO触发相机；**不是通过UART向相机发送字符串camcapture**。
6. 子板应出现 `[CAM] RESULT:heart_A` 等结果，底板收到0x83牌面事件。
7. 若只有 `[CAM] timeout -> UNKNOWN src=TIMEOUT (debug)`，先核对OpenMV USB
   是否产生同次RESULT，再查P6触发、共地、P4→GPIO46和两端波特率；不要将
   子板的超时兜底误认为相机识别成功。
