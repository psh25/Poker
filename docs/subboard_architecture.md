# 子板架构 v1（裸机：超级循环 + 中断）

> 版本：v1.0  
> 日期：2026-08-23  
> 状态：草稿。子板主控与摄像头模组硬件选型尚未最终确定，本文先固定**架构形态、职责边界与通信协议**；引脚映射、驱动库等实现细节待硬件定型后补充。

## 一、职责定位

子板是**“带反馈的执行器”**，不是决策者：

- 只听底板命令做事：启动/停止发牌电机、采集光敏信号、驱动摄像头识别牌面、打包数据实时回传。
- 只做**物理层异常检测**（光敏超时、卡牌、电机堵转电流），检测到后**立即上报**，不做重试/停止等业务决策。
- **业务层决策**（是否漏发/多发、是否重发、是否急停、整副牌数据组装与上传小程序）全部归底板。

## 二、架构选型：裸机，不使用 RTOS

### 2.1 工作负载分析

子板处理的是**串行流水线**：

```
收到底板指令 → 启动发牌电机 → 等待光敏触发 → 触发摄像头拍照并识别 → 打包数据回传
```

特征：

- 没有两个需要同时竞争 CPU 的重型任务；电机启动后不再占用 CPU，传感器等待走中断。
- 实时性要求集中在“及时响应底板命令”和“光敏触发捕获”，两者用中断即可满足。
- 摄像头识别虽耗时，但主要是等待硬件就绪（SPI/DMA），CPU 大部分时间空闲，用状态机等待标志位即可。

### 2.2 裸机 vs RTOS 对比

| 对比维度 | 裸机（推荐） | RTOS |
|----------|--------------|------|
| 资源占用 | 极省，内存留给图像缓冲 | 额外消耗内核 ROM/RAM 与任务栈 |
| 开发周期 | 快，状态机几小时可写完 | 需设计任务、优先级、栈、同步 |
| 可维护性 | 逻辑线性，`state` 变量一目了然 | 异步事件，排查需脑内模拟调度 |
| 稳定性 | 无栈溢出/优先级反转类内核风险 | 栈大小、优先级、内存碎片均有隐患 |
| 逻辑匹配度 | 与“启动→等待→完成”流水线天然匹配 | 需要为简单逻辑强行拆任务 |

> 结论：一个发牌电机 + 光敏 + 摄像头不满足上 RTOS 的条件（多个不同相位电机、独立网络栈、多个精确软件定时器等），子板坚决采用裸机。

## 三、状态机设计

### 3.1 状态定义

| 状态 | 含义 | 可执行操作 |
|------|------|------------|
| IDLE | 空闲，等待底板命令 | 解析串口命令、查询状态、自检 |
| MOTOR_ON | 发牌电机启动中 | 启动电机，启动 500ms 超时计时 |
| WAIT_CARD | 等待光敏检测到牌 | 等待光敏中断标志；超时进入 ERROR |
| CAM_CAPTURE | 摄像头拍照 + 识别 | 触发拍照、读取图像、识别牌面（含 2s 超时） |
| SEND_BACK | 打包回传底板 | 组装事件帧发送，完成后回到 IDLE |
| ERROR | 物理层异常 | 立即上报错误事件，停止电机，等待底板指令（重试/复位） |

### 3.2 状态转移图

```mermaid
stateDiagram-v2
    [*] --> IDLE : 上电自检完成
    IDLE --> MOTOR_ON : 收到 CMD_DEAL_START
    MOTOR_ON --> WAIT_CARD : 电机启动完成
    WAIT_CARD --> CAM_CAPTURE : 光敏中断触发
    WAIT_CARD --> ERROR : 500ms 超时（卡牌/漏发）
    CAM_CAPTURE --> SEND_BACK : 识别完成（成功或标记未知牌）
    SEND_BACK --> IDLE : EVT_DEAL_DONE 发送完成
    IDLE --> ERROR : 自检失败（上电或底板触发）
    ERROR --> IDLE : 收到底板复位/重试指令
    MOTOR_ON --> ERROR : 堵转电流检测（如有）
```

### 3.3 转移条件与动作表

| 转移 | 触发条件 | 动作 |
|------|----------|------|
| IDLE → MOTOR_ON | 收到 `CMD_DEAL_START` | 启动发牌电机；启动 500ms 超时定时器 |
| MOTOR_ON → WAIT_CARD | 电机启动完成 | 等待光敏触发 |
| WAIT_CARD → CAM_CAPTURE | 光敏中断标志置位 | 停止电机；触发摄像头拍照 |
| WAIT_CARD → ERROR | 500ms 未检测到牌 | 停止电机；上报 `EVT_ERROR_CARD_JAM` |
| CAM_CAPTURE → SEND_BACK | 识别完成 | 组装 `EVT_CARD_VALUE`（含未知牌标志） |
| SEND_BACK → IDLE | 事件帧发送完成 | 清空单卡缓冲，回到待命 |
| ERROR → IDLE | 收到底板复位/重试指令 | 复位状态机，重新待命 |

## 四、中断与定时器设计

| 中断/定时器 | 用途 | 说明 |
|-------------|------|------|
| 串口 RX 中断 | 接收底板命令 | 中断内只写入环形缓冲区，主循环解析，避免丢帧 |
| 光敏 GPIO 中断 | 检测牌通过 | 置位标志 + 记录计数；配合消抖/多次采样防电机干扰误触发 |
| 软件超时定时器 | 光敏 500ms 超时 | 用于卡牌/漏发检测 |
| 串口 TX | 事件上报 | 逐张实时上报，不缓存整副牌 |
| 电流检测（如有） | 发牌电机堵转 | 模拟输入或驱动芯片报警脚，异常立即上报 |

> 设计原则：中断函数内**不做耗时操作**（不识别图像、不解析协议），只置标志位/写缓冲；全部业务在 `loop()` 状态机中处理。

## 五、与底板的通信协议

### 5.1 物理层

- 通道：滑环串口（底板 IO42/IO41 对端；**当前按 2 线普通 UART 处理**，若板上有差分收发器则对本层透明）。
- 可靠性：**必须带 CRC 校验**；数据包分小段发送；校验失败丢弃并触发重传；长时间无通信按掉线处理。

### 5.2 帧格式（草案）

```
帧头(0xA5) | 类型(1B) | 长度(1B) | 数据(nB) | CRC(1B) | 帧尾(0xAA)
```

### 5.3 命令（底板 → 子板）

| 命令 | 数据 | 说明 |
|------|------|------|
| `CMD_DEAL_START` | 目标牌堆/序号 | 启动发牌电机发一张牌 |
| `CMD_STOP` | — | 停止电机 / 急停 |
| `CMD_STATUS_QUERY` | — | 查询当前状态与计数 |
| `CMD_SELF_TEST` | — | 触发自检（光敏、电机驱动、摄像头） |
| `CMD_RESET` | — | 复位状态机（从 ERROR 恢复） |

### 5.4 事件（子板 → 底板）

| 事件 | 数据 | 说明 |
|------|------|------|
| `EVT_READY` | 自检结果 | 上电自检完成，进入待命 |
| `EVT_CARD_OUT` | 成功标志 | 光敏检测到一张牌发出 |
| `EVT_CARD_VALUE` | 牌序号、花色、点数、未知牌标志 | 牌面识别结果 |
| `EVT_DEAL_DONE` | — | 单张发牌流程完成 |
| `EVT_ERROR_CARD_JAM` | 超时值 | 光敏超时/卡牌，**立即上报** |
| `EVT_ERROR_MOTOR_STALL` | 电流/时间 | 发牌电机堵转（如支持检测） |
| `EVT_ERROR_CAM_FAIL` | 错误码 | 摄像头识别失败（该张标记为未知牌） |

### 5.5 实时上报原则（重点）

1. **发一张、传一张**：子板内存中只需保留当前这一张牌的信息，不缓存整副牌。
2. 光敏触发后立即发 `EVT_CARD_OUT`；识别完成后立即发 `EVT_CARD_VALUE`，顺序天然一致，无需复杂流水号。
3. 异常（卡牌超时/堵转）**第一时间上报**，等待底板决策，而不是攒到最后。
4. 整副牌数据的拼装与统一上传小程序是底板职责。

## 六、数据与内存设计

- **单卡数据缓冲**：只保留当前一张牌的信息（牌序号、花色、点数、成功/失败标志），无历史数组。
- **静态分配**：图像缓冲使用静态大数组复用，避免频繁 `malloc/free` 造成堆碎片。
- **摄像头帧缓冲**：若摄像头驱动需要较大缓冲，裸机架构下可独占大部分 RAM（这是不用 RTOS 的额外收益）。

## 七、异常处理

| 检查类型 | 谁检测 | 上报时机 | 底板决策 |
|----------|--------|----------|----------|
| 光敏超时/卡牌 | 子板（500ms 定时器） | 立即发 `EVT_ERROR_CARD_JAM` | 标记疑似漏发，决定重试一次或暂停报警 |
| 发牌电机堵转 | 子板（电流检测/超时） | 立即发 `EVT_ERROR_MOTOR_STALL` | 停止或急停，进入错误状态 |
| 摄像头识别失败 | 子板（2s 超时/识别置信度） | 该张标记“未知牌”，发 `EVT_CARD_VALUE` + 标志 | 记录异常，整局结束后向小程序报告 |
| 串口掉线 | 子板/底板双方 | 本地超时处理 | 底板暂停发牌流程，等待链路恢复 |

## 八、启动与自检流程

1. 硬件初始化：GPIO、PWM、串口、摄像头接口。
2. 自检：光敏电平正常、电机驱动就绪、摄像头可初始化。
3. 上报 `EVT_READY`（含自检结果）给底板。
4. 进入 IDLE，等待底板命令。

## 九、主循环伪代码框架

```cpp
enum { STATE_IDLE, STATE_MOTOR_ON, STATE_WAIT_CARD,
       STATE_CAM_CAPTURE, STATE_SEND_BACK, STATE_ERROR } state;

void loop() {
    parse_uart_command();          // 串口环形缓冲解析，设置命令标志
    switch (state) {
    case STATE_IDLE:
        if (cmd == CMD_DEAL_START) { start_motor(); state = STATE_MOTOR_ON; }
        if (cmd == CMD_SELF_TEST)   { run_self_test(); }
        break;
    case STATE_MOTOR_ON:
        if (motor_started) { state = STATE_WAIT_CARD; arm_timeout(500); }
        break;
    case STATE_WAIT_CARD:
        if (photo_isr_flag) {
            stop_motor();
            send_event(EVT_CARD_OUT);
            trigger_camera();
            state = STATE_CAM_CAPTURE;
        } else if (timeout_expired) {
            stop_motor();
            send_event(EVT_ERROR_CARD_JAM);
            state = STATE_ERROR;
        }
        break;
    case STATE_CAM_CAPTURE:
        if (camera_ready) {
            recognize_card();       // 成功或标记未知牌
            state = STATE_SEND_BACK;
        } else if (timeout_expired) {
            mark_unknown_card();
            state = STATE_SEND_BACK;
        }
        break;
    case STATE_SEND_BACK:
        send_event(EVT_CARD_VALUE);
        send_event(EVT_DEAL_DONE);
        state = STATE_IDLE;
        break;
    case STATE_ERROR:
        if (cmd == CMD_RESET || cmd == CMD_STOP) { reset_machine(); state = STATE_IDLE; }
        break;
    }
}
```

## 十、与仓库结构的关系及落地清单

后续将在 `DealerMachine/` 下创建第二个 PlatformIO 项目 `SubBoard/`，结构参照现有 `BottomBoard/`（`src/`、`include/`、`platformio.ini`、`test/`）。

### 落地前需确认

1. **子板主控选型**（决定引脚映射、摄像头接口、电机驱动 IO）。
2. **摄像头模组**：视觉模组（如 OpenMV，自带识别，串口/SPI 回传结果）或摄像头传感器 + 主控识别，两者对状态机 `CAM_CAPTURE` 阶段实现不同。
3. **发牌电机驱动**：是否有电流检测/堵转报警脚。
4. **协议帧格式定稿**：与底板 `architecture_v2.md` 第七章对齐后冻结。
5. **光敏传感器安装与触发电平**：确认有效沿与消抖参数。
