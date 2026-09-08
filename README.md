# 多功能发牌机（Dealer Machine）

本项目是一个基于 **ESP32-S3 底板 + 上层子板** 的多功能发牌机：通过旋转编码器选择发牌方案（德州扑克、斗地主、桥牌等），自动旋转底座、逐张发牌，用摄像头记录牌面、光敏传感器校验发牌结果，并通过蓝牙与小程序联动，支持远程参与牌局。

## 仓库结构

```
DealerMachine/
├── README.md                  # 本文件：项目说明与开发导航
├── docs/                      # 架构设计文档（必读）
│   ├── architecture_v2.md     # 整体架构 v2（底板 RTOS + 子板裸机）
│   ├── architecture_v2_diff.md# v1 → v2 区别总结与待审核项
│   ├── subboard_architecture.md  # 子板裸机架构
│   └── board_protocol.md      # 底板 ↔ 子板 UART 通信协议
├── BottomBoard/               # 底板 PlatformIO 项目（ESP32-S3，Arduino + FreeRTOS）
└── SubBoard/                  # 子板 PlatformIO 项目（ESP32-S3，裸机超级循环 + 中断）
```

## 硬件概览

| 部件 | 所属板 | 说明 |
|------|--------|------|
| 底座转盘步进电机 | 底板 | TMC2209 驱动（STEP/DIR/ENN，电流由驱动板 VREF 电位器设定；不再使用单线 UART） |
| 屏幕 + SD 卡 | 底板 | ST7735 128×160（1.8"，当前测试板），与 SD 共用 SPI 总线（独立片选） |
| 旋转编码器 | 底板 | EC11：旋转选择/取消方案、短按确认方案/CONFIRM 发牌、GAME_ACTIVE 长按结束回 IDLE |
| 霍尔零点传感器 | 底板 | A3144，两段式上电归零、运行中失步校准 |
| 蓝牙 | 底板 | ESP32-S3 内置 BLE，与小程序通信 |
| 发牌电机 | 子板 | 6V 直流电机，由子板控制 |
| 光敏传感器 | 子板 | 检测牌是否发出、计数 |
| 摄像头 | 子板 | 识别牌面（花色、点数） |
| 板间通信 | 滑环 | **2 线 UART**（POS/NEG 各一线：底板 POS=IO45 TX、NEG=IO48 RX；子板 POS=IO38 RX、NEG=IO39 TX），CRC 校验 |

> ⚠️ IO 映射以两板的 `include/pins_config.h` 为最新标准，**开发/接线前必须与各自原理图逐项核对**；《底板软件开发交接指南》中的旧版引脚表已不再使用。

## 架构要点（v2）

设计文档：[整体架构 v2](docs/architecture_v2.md) · [v1→v2 差异](docs/architecture_v2_diff.md) · [子板架构](docs/subboard_architecture.md)

### 核心决策

- **底板：FreeRTOS 多任务**；**子板：裸机（超级循环 + 中断）**，子板是“带反馈的执行器”。
- **通信原则**：子板 → 底板逐张实时上报（光敏/识别/异常），底板 → 小程序整局统一上传。
- **板间协议**：`0xA5 | type | len | data | crc8 | 0xAA`，掉线超时保护（详见 [board_protocol.md](docs/board_protocol.md)）。
- **命令/事件命名**：命令统一 `CMD_*`，事件统一 `EVT_*`（错误事件为 `EVT_ERROR_*`）；CLI 别名 = 枚举名去前缀的小写（`status`、`dealdone`、`motorstall`）。
- **状态机**：`IDLE → DEALING → GAME_ACTIVE`（选方案并入 IDLE：旋转切换/取消、短按确认方案、再按 CONFIRM 发牌；GAME_ACTIVE 长按编码器确认结束并直接回 IDLE），仅状态管理任务负责切换（单写者）。
- **屏幕显示**：事件驱动（队列触发），非轮询，只展示不决策。

### 底板任务与优先级

| 任务 | 优先级 | 核心 | 触发方式 | 关键时序 |
|------|--------|------|----------|----------|
| 蓝牙通信 | 高 (3) | 任意 | BLE 队列 + 帧解析 | NUS 服务，收主机命令帧/发状态帧 |
| 子板通信 | 中 (2) | 固定核心 0 | 队列 + 串口 | 20ms 轮询，掉线阈值 3s |
| 发牌控制 | 中高 (2) | 固定核心 1 | 信号量触发 | 光敏 500ms、摄像头 2s 超时 |
| 编码器处理 | 中 (2) | 任意 | 1ms 轮询 A/B + SW | 10ms 消抖、4 步/格 |
| 屏幕显示 | 低 (1) | 任意 | 队列触发 | 事件驱动，队列容量 5 |
| 状态管理 | 低 (1) | 任意 | 事件组阻塞 | 永久等待（协调器） |
| 系统监控 | 低 (1) | 任意 | 定时器 | 500ms 巡检电机状态/心跳/告警 |

## 底板代码框架说明

`BottomBoard/` 已具备可用的**基础交互**：屏幕方案选择（两段式确认）、编码器旋转/按键、串口 CLI 调试（`sub` / `sim` / `setstate` 等）均已实现；电机、霍尔、SD、蓝牙等硬件功能仍以空函数（`busy_*`）占位，并在代码注释中标注了对应的架构章节与时序。

| 文件 | 内容 |
|------|------|
| `include/pins_config.h` | IO 映射 |
| `include/app_config.h` | 优先级、栈大小、队列容量、时序常量 |
| `include/itc.h` / `src/itc.cpp` | 队列、信号量、互斥量、事件组创建 |
| `src/tasks.cpp` | 7 个任务骨架 |
| `src/display.cpp` / `include/display.h` | 屏幕显示（IDLE 屏：未选择/已选方案 + CONFIRM 按钮） |
| `src/state_machine.cpp` | 状态机与转移逻辑 |
| `src/protocol.cpp` | 子板 UART 协议（帧解析/发送/事件分发） |
| `src/hardware.cpp` | 硬件初始化、中断、步进驱动（AccelStepper）/归零/自检占位 |
| `src/main.cpp` | 启动顺序 |

启动顺序：硬件初始化 → 创建内核对象 → 挂接中断 → 步进驱动初始化（AccelStepper）→ 霍尔两段式归零 → 外设自检 → 创建任务（低→高）→ IDLE。

## 开发环境

- VSCode + PlatformIO 插件
- 打开 `BottomBoard/` 或 `SubBoard/` 目录即可构建上传；板卡与烧录参数统一在各自 `platformio.ini` 中（当前均为 `esp32-s3-devkitc-1`；底板默认 8MB Flash、无 PSRAM，16MB/PSRAM 配置已注释，待按模组型号恢复）
- 调试串口（CH340 / UART0）115200 波特率，日志从 `Serial` 输出
- 屏幕（TFT_eSPI）与步进（AccelStepper）已通过 `lib_deps` 声明；SD 卡驱动（SdFat）待加入，无需依赖本机 Arduino 库目录

## 当前状态与 TODO

- [x] 架构 v2 文档（整体 / 差异 / 子板）
- [x] 底板代码框架（任务、ITC、状态机、协议、硬件骨架）
- [ ] 底板 IO 映射与原理图核对
- [x] 底座步进驱动（STEP/DIR/ENN，梯形加减速平滑启停，电流由驱动板 VREF 电位器设定）
- [ ] （可选）恢复 TMC 单线 UART 电流控制（若启用需重新评估库与平台兼容性）
- [ ] 霍尔两段式归零与失步校准
- [x] 屏幕显示（ST7735 128×160：方案列表 + 两段式确认）
- [ ] SD 卡驱动（SdFat，与屏幕共用 SPI 的片选互斥）
- [x] 底板 CLI 调试（sub / sim / setstate / 屏幕测试）
- [x] 底板 BLE 基础（NUS 收命令帧/发状态帧，帧格式与板间一致）
- [ ] 小程序端联调（连接、帧收发 UI）
- [x] 子板 `SubBoard/` PlatformIO 项目（裸机状态机 + 串口协议）
- [x] 子板串口链路调试（串口日志回显；RGB LED 已随最新硬件配置移除）
- [x] 板间协议基础联调（ACK 回传、CLI 注入）
- [ ] 板间全流程联调（待子板硬件定型）

## 注意事项

- Micro USB 调试口仅 4.6~4.7V，**禁止**给视觉模组和满载电机供电。
- 电机启停会产生强电磁干扰：传感器采样需消抖/多次采样，串口必须带校验。
- 串口接线：滑环 POS 连 POS、NEG 连 NEG（底板 POS=IO45 TX → 子板 POS=IO38 RX；底板 NEG=IO48 RX ← 子板 NEG=IO39 TX），且**必须共地**；不要占用 UART0（GPIO43/44，CH340 调试口）；给某块板烧录前先断开两板间的 RX/TX 线。
