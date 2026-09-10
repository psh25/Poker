# OpenMV H7 Plus：无牌边、小模板扑克牌识别

本版本另存新文件，不修改原来的 `template_1.py`、
`recognize_cards_triggered.py` 或旧模板目录。不使用 CNN、不判断无牌。
目标平台为你当前脚本使用的 OpenMV 固件 5.x（`csi.CSI`）。
配置值、SSIM 阈值和时间预算需要在实际机器上验证，未进行板上测速。

## 1. 上传哪些文件

把下列 **4 个文件放在 OpenMV 的 `/flash` 同一层目录**：

- `cards_fast_config.py`：只保存存储、相机和模板格式等共用配置。
- `cards_fast_core.py`：采集/识别共用的原生图像处理。
- `capture_cards_fast.py`：在 IDE 中运行这个文件采集。
- `recognize_cards_fast.py`：在 IDE 中运行这个文件识别。

独立运行时把所需入口的内容复制为板上的 `main.py`，两个共用模块仍须保留。
PC 上的 `tests` 不需要上传，板上不需要 numpy、OpenCV 或 pytest。
更新共用模块或配置后，重置板子再运行，避免使用旧模块缓存。

在 `cards_fast_config.py` 中选择模板存储介质：

```python
USE_SD_CARD = True   # /sdcard/cards_fast_v2
USE_SD_CARD = False  # /flash/cards_fast_v2，无需 SD 卡
```

修改后重置 OpenMV，保证采集和识别加载同一个配置。使用 SD 时会确认挂载
和剩余空间；使用 Flash 时完全不依赖 SD。两种模式均不会自动切换介质，
避免模板被意外分散到两个目录。相机标定始终位于内部 Flash 的
`/flash/cards_fast_v1/camera.json`；若不存在，先运行一次相机标定。

本版本不保存 PPM、BMP、JPG 或拒绝样本 JSON。只有成功模板的小型 PGM
及配套 JSON 会写入 SD 卡；`SAVE_TEMPLATES=False` 只做实时验证，不写文件。

## 2. 成像与 ROI 标定

1. 固定相机、白光、牌面高度和镜头焦点。原始样本有明显模糊，先检查调焦。
2. 模板采集只调整 `capture_cards_fast.py` 中的 `CAPTURE_ROIS`。把牌放在
   标称位置，将点数、花色和 JOKER 框收紧，避免把中间花纹和牌外背景采进模板。
3. 实际识别只调整 `recognize_cards_fast.py` 中的 `RECOGNITION_ROIS`，窗口
   应覆盖完整机械位移范围。采集 ROI 与识别 ROI 不要求位置或尺寸相同。
4. 两个入口分别保存 `adaptive_size` 和 `adaptive_offset`。程序使用原生
   `mean(..., threshold=True, invert=True)`提取局部暗字，不再计算整块 ROI
   的 Otsu 阈值。当前 `size=5`、`offset=-8` 是实机调参起点。
5. 普通牌应点数在上、花色在下；如果安装方向不同，先统一图像方向再采集。
6. 看不到牌边时，使用字迹连通域定位。10 按同行区域合并；点数/花色
   相对位置用 `PAIR_*` 校验。不要用字符自身主方向当作准确倾角。
7. 标准模板在标称角度采集。识别时仅对已定位的小图执行有限角度校正。

蓝框是搜索范围，绿框是实际提取区域，右侧是将要保存的归一化模板。
最终模板为**白色字迹、黑色背景**，这让红黑形状共用处理，也避免旋转
补黑边被错误当作字符。识别大小王的颜色仍来自原彩色帧。

## 3. 首次相机标定

在入口 `capture_cards_fast.py` 设置：

```python
CALIBRATE_CAMERA = True
```

在固定光照下放一张普通牌，启动程序。相机自动稳定后，曝光、增益、
白平衡写入 `camera.json` 并锁定。以后都设为 `False`，两端读取同一文件。
初次未标定就运行识别会给出明确错误。

已有模板时不允许覆盖相机标定。重新标定应使用新的 ROOT 或自行归档
旧模板后重采，防止模板和识别图成像条件不同。代码不会替你删除旧数据。

## 4. 采集模板

P9 接瞬时按键到 GND。放稳牌后按一下保存，松开后才能再次保存。
标签由你填写，程序不使用待采标签去决定如何分割图像。

### 点数

```python
CAPTURE_KIND = "normal"
RANK_LABEL = "A"              # 改为 A、2…10、J、Q、K
SUIT_LABEL = "spade"          # 必须填写这张牌实际的花色
NORMAL_SAVE_GROUPS = ("rank",)
SAVE_TEMPLATES = True
CALIBRATE_CAMERA = False
```

点数采集时仍同时定位花色并检查布局，确保完整观察区适合识别。
每类先保留 1～2 张清楚、有代表性的标准姿态模板，建议覆盖红黑印刷。

### 花色

设置 `NORMAL_SAVE_GROUPS = ("suit",)`，分别采集 `heart`、`diamond`、
`club`、`spade`。如果两个类别都缺模板，可设为 `("rank", "suit")` 一次保存两者。
不要在采 13 个点数时无意重复保存 13 张同一种花色。

若绿框选到了干扰区域，先调本文件的 `CAPTURE_ROIS`、`adaptive_size`、
`adaptive_offset`，再考虑共用的 `MIN_BOX/MAX_BOX`；
`CANDIDATE_INDEX` 可选择预览中的其它候选，但不应用它掩盖系统性的错误定位。

### 大小王

```python
CAPTURE_KIND = "joker"
JOKER_COLOR = "red"           # 黑王改为 "black"
```

红黑王各至少采一次，形状模板均归到 `joker`。颜色若不符合填写标签，
程序不写模板。先调共享 LAB 阈值，不能通过降低颜色置信度
把白底误当作黑字。大小王和红黑对应关系在识别程序的
`red_joker_label` / `black_joker_label` 设置。

若不同机械位置露出的 JOKER 片段不同，优先选共同可见的可辨认片段；
必要时采少量实际片段模板。轮廓碰到搜索边界默认拒绝保存，应该调整
窗口或选择内部稳定片段；不能用残缺片段当作完整文本通用模板。

### 牌背

设置 `CAPTURE_KIND = "back"`。牌背采用独立纹理窗口，不作字迹分割。
可采 2～3 个代表性位置；识别同帧复核时会检查 `BACK_REFINE_OFFSETS`
中的少量位移。纹理的位移容忍度需要实拍验证，复杂变化可能需扩大
搜索或采集更多代表性样本，但不能无限增加模板。

### 保存结构及质量检查

有效小模板写入 `templates/<group>/*.pgm`，每个 PGM 有对应 JSON。
程序检查对比度、符号大小、密度、边界裁切、布局和颜色；这些检查
**不能保证图像已合焦，也不能自动确认你输入的点数标签是否正确**。
失败时只输出串口诊断，不写任何文件。

模板先写为临时 PGM/JSON，同步后重命名提交，PGM 最后成为提交标志。
启动加载时忽略残留的 `.tmp.*` 文件。不要在写入期间断电或热插拔 SD 卡。

每类默认最多 4 张模板，超限会明确报错，不静默截断或覆盖。
人工清理时只归档确认不需要的成对 PGM/JSON，保留有代表性的模板。

## 5. 验证样本

把 `SAVE_TEMPLATES = False`，可在 IDE 预览和终端中检查中心与极限偏移、
正负倾角、不同实体牌；该模式不写文件。需要留存验证图时，由电脑端保存
IDE Frame Buffer，避免 OpenMV 文件系统持续写大图。

## 6. 识别和串口

模板完整后运行 `recognize_cards_fast.py`。
默认检查 13 种点数、4 种花色、JOKER、牌背是否齐全。
调试阶段可临时把 `STRICT_TEMPLATE_COVERAGE=False`，此时缺失类别无法识别，
也不能用不完整候选集的分差验收准确率。

接线：P9 -> 按键 -> GND；UART3 P4=TX、P5=RX，115200、8N1，共地，
使用与板子兼容的 3.3 V 串口接口。

每次按下只发送一条最终报文，例如：

```text
RESULT:spade_A
RESULT:heart_10
RESULT:joker_big
RESULT:joker_small
RESULT:back
RESULT:UNKNOWN
```

线路终止符为 CRLF。`UNKNOWN` 代表证据不足/超时，不代表无牌。
运行异常输出 `RESULT:ERROR`；传输失败另在 USB 输出错误。
USB 还输出 `PROFILE` 和类别分数，UART 不输出这些调试数据。

流程：记录触发沿 -> 消抖 -> 丢弃潜在旧帧 -> 获取新帧 -> 字迹定位 ->
原生二值化/旋转/缩放 -> 原生 SSIM -> 按类别取最高分并检查类间分差 ->
同帧角度/阈值复核 -> 时间允许时重拍一次 -> UART 发完最后字节。
所有 4 种花色都参与评分，颜色明确冲突时拒绝；颜色不确定时不错误
排除形状候选。JOKER 必须获得明确红/黑证据才输出大小王。

## 7. 耗时及阈值

- `RESULT_BUDGET_MS=900`，其中 `OUTPUT_RESERVE_MS=35` 预留发送时间。
- `PROFILE.total_ms` 从触发沿到 UART 最后字节发完，包含消抖和取帧。
- `capture_ms` 统计取帧；`passes` 每项为 (第几帧, 阶段, 定位ms, 阶段总ms)。
- 若 `UART_ENABLED=False`，USB 输出无法精确代表物理 UART 发送结束时间。
- 超过处理预算的结果不会作为成功结果输出，而是 `UNKNOWN/TIMEOUT`。
- 截止时间是协作式检查，不能中断单次原生 C 函数或正在进行的曝光，
  所以本程序不构成严格硬实时 900 ms 保证。`over_budget` 会记录实际超预算。
- 串口发送后的 USB 调试打印和空闲 GC 不计入 UART 结果延迟，但会影响
  下一次可接收触发的时刻；释放并稳定后重新武装。正式应用可关闭 USB profile。
- 首轮 SSIM 阈值是起点，不是准确率承诺。检查错误类别、同类/异类分数、
  重拍比例、各类别最坏耗时，再调整 `ACCEPT_SCORE/MARGIN`。
- 不建议用降低阈值掩盖模糊、漏裁或错位。先看采集预览和实际图像。
- 改变采集二值化参数或归一化方式后应重新采集模板。识别 ROI、识别自适应
  阈值、决策阈值和搜索角度可独立调整，不需要修改共用 `config`。

## 8. 已完成的本地验证与限制

运行 `python -m pytest -q tests/test_cards_fast.py` 可检查连通域合并、平移
归一化、角度搜索、颜色判断、类别汇总、场景冲突、模板参数一致性、
采集/加载/识别往返、重拍、超时及 UART 部分写入处理。
图像测试通过 PC 上的小型 OpenCV 适配层进行；与 OpenMV 原生核并非
逐像素等价，也不代表 H7 Plus 的速度或实拍准确率。尚未连接 H7 Plus
完成板上运行验证。固件接口参照 OpenMV v5.0.0 的 `modules/py_image.c`。

本次自适应二值化升级使用新流水线版本和 `cards_fast_v2` 存储目录；原
`cards_fast_v1` 模板不会被加载。请按本流程重采，旧目录可自行归档。
