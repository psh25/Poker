# OpenMV Cam H7 Plus 扑克牌局部识别：库与接口调研

> 调研日期：2026-09-02  
> 目标：摄像头与扑克牌距离相对固定，画面包含牌角，但牌可能有小幅旋转或透视偏差；在 OpenMV Cam H7 Plus 上完成牌角定位、方向校正、点数与花色识别。

## 1. 结论先行

建议先做传统视觉方案，必要时再上神经网络：

1. 用 `csi`（新固件）或 `sensor`（旧固件）完成采集；用固定 ROI、灰度图和较低分辨率控制耗时。
2. 相机启动后先让自动曝光、增益和白平衡稳定，再读取当前值并锁定，减少同一张牌在不同帧中的灰度/颜色漂移。
3. 用 `find_blobs()`、`find_lines()` / `find_line_segments()` 或 `find_rects()` 定位牌及估计角度。因为画面只包含牌的一角，完整矩形不一定存在，所以不能只依赖 `find_rects()`。
4. 用 `rotation_corr()` 做旋转/透视归一化，必要时先用 `lens_corr()` 校正镜头畸变；随后裁出固定尺寸的“点数区”和“花色区”。
5. 对归一化后的牌角优先尝试：
   - 点数：`find_template()` 或二值化后比较轮廓/像素特征；
   - 花色：`find_template()`，或用 `find_blobs()` 分离红/黑后再分类；
   - 方向/尺度变化仍较大：`find_keypoints()` + `image.match_descriptor()`；
   - 光照、背景和牌面版本变化很大：量化 TFLite 模型，使用 `ml.Model`。
6. 用标准文件 API、`Image.save()` 和 microSD 卡保存调试帧、模板、模型、标签和识别日志。长期频繁写入优先使用 `/sdcard`，并在断电前关闭文件、调用 `os.sync()`。

## 2. 硬件与固件注意事项

OpenMV Cam H7 Plus 使用 STM32H743（Cortex-M7，480 MHz）、32 MB 外部 SDRAM、32 MB QSPI Flash 和 OV5640 5 MP 滚动快门传感器，并带硬件 JPEG 编解码器。它的内存足以容纳较大的图像缓冲区，但分辨率越高，传统视觉算子的帧率仍会明显下降。

### 2.1 `csi` 与 `sensor` 的版本差异

OpenMV 固件 v5.0.0 / MicroPython 1.28 的现代接口是 `csi.CSI`。网上许多旧例程基于 v4.x，使用 `sensor`。两者表达的是同类功能，但命名风格不同：

| 功能 | 新接口（v5.0，优先） | 旧接口（v4.x 常见） |
|---|---|---|
| 创建/复位 | `cam = csi.CSI()`；`cam.reset()` | `sensor.reset()` |
| 像素格式 | `cam.pixformat(csi.GRAYSCALE)` | `sensor.set_pixformat(sensor.GRAYSCALE)` |
| 帧尺寸 | `cam.framesize(csi.QVGA)` | `sensor.set_framesize(sensor.QVGA)` |
| 窗口/ROI | `cam.window((x, y, w, h))` | `sensor.set_windowing((x, y, w, h))` |
| 拍照 | `cam.snapshot()` | `sensor.snapshot()` |
| 自动增益 | `cam.auto_gain(...)` | `sensor.set_auto_gain(...)` |
| 自动曝光 | `cam.auto_exposure(...)` | `sensor.set_auto_exposure(...)` |
| 自动白平衡 | `cam.auto_whitebal(...)` | `sensor.set_auto_whitebal(...)` |
| 镜像/翻转 | `cam.hmirror(...)` / `cam.vflip(...)` | `sensor.set_hmirror(...)` / `sensor.set_vflip(...)` |

不要在同一个脚本中混用两套接口。应先在 OpenMV IDE 中查看连接设备的固件版本，并以该版本 IDE 自带的 `File → Examples` 示例和对应版本文档为准。

## 3. 摄像头控制

### 3.1 `csi`：现代摄像头接口

典型初始化：

```python
import csi
import time

cam = csi.CSI()
cam.reset()
cam.pixformat(csi.GRAYSCALE)  # 模板、边缘、ORB 通常优先灰度
cam.framesize(csi.QVGA)      # 320x240，先以速度优先
cam.snapshot(time=1500)      # 让自动控制稳定；具体固件也可循环丢弃若干帧

clock = time.clock()
while True:
    clock.tick()
    img = cam.snapshot()
    print(clock.fps())
```

常用接口：

| 接口 | 作用 | 对本项目的建议 |
|---|---|---|
| `csi.CSI(cid=0)` | 创建摄像头对象 | H7 Plus 单主相机通常直接 `csi.CSI()` |
| `reset()` | 初始化/复位传感器 | 修改大类配置前调用 |
| `pixformat(format)` | `GRAYSCALE`、`RGB565`、`BAYER`、`YUV422`、部分传感器支持 `JPEG` | 算法处理用 `GRAYSCALE`/`RGB565`；高分辨率存图才考虑 `JPEG` |
| `framesize(size)` | 设置 QVGA、VGA 等预定义尺寸，也可给 `(w, h)` | 从 QVGA 开始；优先缩小图像而非盲目使用 5 MP |
| `window(roi)` | 设置采集窗口 | 距离固定时只采牌角可能出现的范围，显著降耗 |
| `snapshot()` | 拍一帧并返回 `image.Image` | 后续所有 `Image` 方法的入口 |
| `framerate(rate)` | 限制帧率 | 低帧率可换取更长曝光，但可能与自动曝光冲突 |
| `brightness()` / `contrast()` / `saturation()` | 传感器图像参数 | 只做小范围调节，不应代替稳定照明 |
| `quality(0..100)` | OV5640 等传感器 JPEG 质量 | 仅影响 JPEG 路径，不是识别精度旋钮 |
| `auto_gain(enable, gain_db=..., gain_db_ceiling=...)` | 自动/固定增益 | 自动稳定后锁定，降低帧间漂移 |
| `gain_db()` | 读取当前增益 | 用于记录稳定值并回填 |
| `auto_exposure(enable, exposure_us=...)` | 自动/固定曝光 | 固定光源和距离时建议锁定 |
| `exposure_us()` | 读取当前曝光时间 | 保存标定参数 |
| `auto_whitebal(enable, rgb_gain_db=...)` | 自动/固定白平衡 | RGB 红黑花色区分时尤其应锁定 |
| `rgb_gain_db()` | 读取 RGB 增益 | 自动稳定后读取再固定 |
| `hmirror()` / `vflip()` / `transpose()` | 硬件方向调整 | 相机安装方向固定后一次性设置 |
| `ioctl(...)` | 传感器专用底层命令 | 仅在标准接口无法满足时使用 |

### 3.2 为什么识别时不要直接用 JPEG 帧

OV5640 可直接输出 JPEG，适合高分辨率拍照或保存，但很多 `Image` 图像处理方法不支持压缩图像或 Bayer 图像。识别循环应使用 `GRAYSCALE` 或 `RGB565`；若需保存证据图，可对处理后的帧调用 `img.save(..., quality=...)`，让保存步骤单独压缩。

### 3.3 曝光、增益、白平衡锁定

模板匹配和颜色阈值都对成像一致性敏感。一个实用标定流程是：

1. 开启 AE/AGC/AWB，等待约 1～2 秒；
2. 读取 `exposure_us()`、`gain_db()`、`rgb_gain_db()`；
3. 用读到的值关闭自动控制并固定；
4. 更换环境光或机械位置后重新标定。

如果只处理灰度图，白平衡影响较小；若用 LAB 阈值区分红色和黑色花色，应同时关闭自动增益和自动白平衡。最好使用固定、漫射、无频闪光源，并避免牌面高光。

## 4. 文件与存储操作

### 4.1 文件系统路径

OpenMV 使用 MicroPython VFS：

- 内部 Flash 通常挂载在 `/flash`；
- 插入 microSD 后通常自动挂载在 `/sdcard`，并可能成为启动时的当前工作目录；
- v5 固件还可能有只读 `/rom`，用于内置模型、标签或级联文件。

不要假设相对路径一定指向内部 Flash。可以用 `os.getcwd()` 检查，并在重要资源上使用绝对路径。

### 4.2 Python 内置文件 API

```python
import os

base = "/sdcard/cards"
try:
    os.mkdir(base)
except OSError:
    pass

with open(base + "/result.csv", "a") as f:
    f.write("frame,label,score\n")

print(os.listdir(base))
print(os.stat(base + "/result.csv"))
os.sync()
```

常用接口：

| 接口 | 用途 |
|---|---|
| `open(path, mode)` | 以 `r/rb/w/wb/a` 等方式读写文本、模型、标签和日志 |
| `with open(...) as f` | 确保及时关闭文件，推荐写法 |
| `f.read()` / `f.readline()` / `f.write()` | 内容读写 |
| `os.getcwd()` / `os.chdir(path)` | 查询/切换工作目录 |
| `os.listdir(path)` / `os.ilistdir(path)` | 枚举文件；`ilistdir` 更适合逐项处理 |
| `os.mkdir(path)` / `os.rmdir(path)` | 创建/删除目录 |
| `os.rename(src, dst)` | 重命名，可用于日志轮换 |
| `os.remove(path)` / `os.unlink(path)` | 删除文件 |
| `os.stat(path)` | 文件状态 |
| `os.statvfs(path)` | 容量、空闲块等文件系统状态 |
| `os.sync()` | 将缓存写回存储介质 |
| `vfs.mount()` / `vfs.umount()` | 手动挂载/卸载；正常自动挂载时通常不需要 |
| `machine.SDCard()` | SD 卡块设备；通常只在自定义挂载或原始块访问时使用 |

FAT 文件系统对写入中突然断电不够健壮。采集大量训练图和日志时优先写 microSD；采用 `with` 关闭文件，阶段性 `os.sync()`，并避免每帧都打开/关闭同一个日志。

### 4.3 图像、模板与帧序列

| 接口 | 用途 |
|---|---|
| `img.save(path, roi=None, quality=50)` | 保存整帧或 ROI；扩展名决定常用图像格式 |
| `image.Image(path, copy_to_fb=False)` | 从文件加载模板/参考图；模板匹配时通常转灰度 |
| `image.ImageIO(path, "w")` / `.write(img)` | 写 OpenMV 帧序列 |
| `image.ImageIO(path, "r")` / `.read(...)` | 回放帧序列，适合离线复现算法 |
| `image.save_descriptor(desc, path)` | 保存 ORB/LBP 描述子 |
| `image.load_descriptor(path)` | 加载描述子用于匹配 |

建议目录结构：

```text
/sdcard/
  cards/
    templates/       # 归一化后的点数/花色模板
    samples/         # 原始或裁剪训练图
    debug/           # 失败帧、带框结果图
    card_model.tflite
    card_model.txt   # 每行一个标签；ml.Model 可自动加载同名标签文件
    config.json      # ROI、阈值、曝光等标定参数
    result.csv
```

## 5. 图像处理库与接口

核心模块是 `image`，摄像头的 `snapshot()`、磁盘加载和新建画布都会得到 `image.Image` 对象。多数方法原地修改图像并返回自身，适合链式调用，但原图可能因此被覆盖；要保留原图时应先 `copy()`。

### 5.1 ROI、裁剪、缩放与像素格式

| 接口 | 作用 | 牌角应用 |
|---|---|---|
| `img.width()` / `height()` / `format()` | 查询图像属性 | 做边界检查和调试 |
| `img.copy(roi=...)` | 复制整图或 ROI | 保留原帧并取出牌角 |
| `img.crop(roi=..., x_scale=..., y_scale=...)` | 裁剪/缩放 | 把牌角归一到固定输入尺寸 |
| `img.to_grayscale()` | 转灰度 | 点数、边缘、模板、ORB 前处理 |
| `img.to_rgb565()` | 转 RGB565 | 红/黑花色颜色判断 |
| `img.get_pixel(x, y)` / `set_pixel(...)` | 访问像素 | 调试或极小区域特征，避免整帧 Python 循环 |

优先把 `roi=(x, y, w, h)` 直接传给算法，而不是先复制整帧。C 实现的 ROI 运算通常更省时、省内存。

### 5.2 阈值、直方图与统计量

| 接口 | 作用 | 牌角应用 |
|---|---|---|
| `img.get_histogram(roi=...)` | 灰度或 LAB 直方图 | 估计前景/背景分布 |
| `hist.get_threshold()` | Otsu 自动阈值 | 白底与深色字符的初始分割值 |
| `hist.get_percentile(p)` | 分位数 | 忽略少量高光/阴影异常值 |
| `img.get_statistics(roi=...)` | 均值、中位数、方差、极值等 | 判断亮度、红黑色彩或空白区域 |
| `img.binary(thresholds, invert=..., mask=..., copy=...)` | 灰度或 LAB 阈值二值化 | 分离牌底、文字、红色花色；若只处理 ROI，先 `copy(roi=...)`，或传二值 `mask` |
| `img.invert()` | 反色 | 统一为白前景/黑背景 |

灰度阈值格式为 `[(low, high)]`；RGB565 的颜色阈值使用 LAB 六元组 `[(L_min, L_max, A_min, A_max, B_min, B_max)]`。阈值应通过 OpenMV IDE 的 Threshold Editor 在真实灯光、真实牌面上标定，而不是照抄示例数值。

### 5.3 滤波和形态学

| 接口 | 作用 | 使用时机 |
|---|---|---|
| `img.mean(size)` | 均值滤波 | 低成本降噪，但会模糊细线 |
| `img.median(size)` | 中值滤波 | 去椒盐噪声，保边性较好 |
| `img.gaussian(size)` | 高斯滤波；可选锐化 | Canny 前轻微降噪 |
| `img.bilateral(...)` | 双边滤波 | 保边降噪，代价高于均值/中值 |
| `img.erode(size)` / `dilate(size)` | 腐蚀/膨胀 | 去小噪点、连接断裂笔画 |
| `img.open(size)` / `close(size)` | 开/闭运算 | 清除小点或填补小孔洞 |
| `img.morph(size, kernel)` | 自定义卷积核 | 特殊增强需求 |

核尺寸参数 `size=1` 通常表示 3×3。应从最小核开始，否则 A、K、10 等细节容易粘连或消失。

### 5.4 边缘、直线、矩形和连通域

| 接口 | 输出 | 对本项目的用途 |
|---|---|---|
| `img.find_edges(image.EDGE_CANNY, threshold=(low, high))` | 修改后的边缘图 | 提取牌边和字符轮廓 |
| `img.find_lines(roi=..., threshold=...)` | Hough 无限直线列表 | 估计牌边主方向 |
| `img.find_line_segments(roi=..., ...)` | 线段列表 | 只看到牌角时找两条相交边 |
| `img.find_rects(roi=..., threshold=...)` | 矩形对象列表 | 完整牌框或完整牌角矩形区域可见时定位；支持一定旋转/剪切 |
| `img.find_blobs(thresholds, roi=..., pixels_threshold=..., area_threshold=..., merge=...)` | `blob` 列表 | 找牌的白色区域、字符或红色花色 |

`blob` 常用属性包括轴对齐包围框、中心、像素数、旋转角、最小面积旋转矩形角点等。可用面积、长宽比、位置和像素数过滤候选。只看到牌角时，建议优先组合“白色牌区 blob + 两条近似垂直的边”，而非要求找到完整牌框。

### 5.5 几何校正

| 接口 | 作用 | 注意事项 |
|---|---|---|
| `img.lens_corr(strength=1.8, zoom=1.0, ...)` | 桶形/鱼眼畸变校正 | 相机、镜头、距离固定后只需标定一次参数 |
| `img.rotation_corr(x_rotation=..., y_rotation=..., z_rotation=..., zoom=..., fov=...)` | 三维旋转和透视修正 | `z_rotation` 处理平面内偏转；x/y 处理俯仰透视 |
| `img.rotation_corr(corners=[p0,p1,p2,p3])` | 用已知四角做透视归一 | 四点顺序必须稳定；仅拍到一个牌角时可能无法得到整牌四角 |
| `img.linpolar()` / `img.logpolar()` | 极坐标/对数极坐标变换 | 可把旋转转为平移；对数极坐标还把尺度变化转为另一方向平移 |
| `img.find_displacement(template, logpolar=...)` | 相位相关求位移，或旋转/尺度变化 | 两个 ROI 尺寸需一致；2 的幂尺寸更合适 |

本项目距离相对固定，尺度变化不是主要问题，应先由牌边/线段估计平面内角度，再用 `z_rotation` 归一。若相机并非垂直俯拍且机械结构固定，可离线标定固定的 x/y 透视校正参数。

### 5.6 模板、相似度与局部特征

#### A. 模板匹配：首选基线

```python
import image

template = image.Image("/sdcard/cards/templates/A.pgm", copy_to_fb=False)
template.to_grayscale()

# img 和模板都应先完成方向、尺度、亮度归一化
rect = img.find_template(
    template,
    threshold=0.70,
    roi=(0, 0, 120, 120),
    step=2,
    search=image.SEARCH_DS,
)
if rect is not None:
    img.draw_rectangle(rect)
```

`find_template()` 使用归一化互相关，只支持灰度图。`SEARCH_DS` 快但可能漏掉全局最佳匹配，`SEARCH_EX` 慢但更彻底。模板匹配对旋转和尺度敏感，因此必须先纠偏/裁剪到固定尺寸，或为少量角度准备多个模板。

相关接口：

- `img.find_template(template, threshold, roi, step, search)`：在大图中定位小模板；
- `img.get_similarity(reference, roi=...)`：对等尺寸区域计算 SSIM，适合在已定位后做最终相似度打分；
- `img.difference(reference)`：绝对差分，可用于固定成像条件下的“黄金样本”比较。

#### B. ORB / LBP 描述子：处理一定旋转或尺度偏差

- `img.find_keypoints(...)`：提取 ORB 特征；`normalized=True` 可得到旋转归一化描述子；
- `image.match_descriptor(desc1, desc2, threshold=...)`：匹配描述子；
- `image.save_descriptor()` / `image.load_descriptor()`：保存和加载 `.orb` / `.lbp`；
- `img.find_lbp(roi)`：提取 LBP，适合纹理分类，但对扑克牌字符未必优于模板法。

ORB 比逐像素模板对旋转/尺度更稳，但牌角符号面积小、纯色区域多，可用关键点可能不足。应先实测每个点数模板能否稳定产生足量关键点。

### 5.7 机器学习：`ml`

当传统方法在不同牌组字体、印刷差异、光照和遮挡下不稳定时，可训练小型分类模型：输入为归一化后的牌角，而不是整帧。

```python
import ml

model = ml.Model("/sdcard/cards/card_model.tflite")
outputs = model.predict([corner_img])
print(model.input_shape, model.output_shape, model.labels)
```

主要接口：

| 接口/属性 | 作用 |
|---|---|
| `ml.Model(path, postprocess=None)` | 加载 TFLite 模型；同名 `.txt` 可自动作为标签列表 |
| `model.predict([img])` | 自动完成常见的图像到输入张量转换并推理 |
| `model.input_shape` / `input_dtype` | 检查输入尺寸和数据类型 |
| `model.input_scale` / `input_zero_point` | 量化参数 |
| `model.output_shape` / `output_dtype` | 检查模型输出 |
| `model.ram` / `model.len` | 评估模型内存和大小 |
| `ml.preprocessing` | 输入归一化/预处理工具 |
| `ml.postprocessing.*` | 针对 Darknet、Edge Impulse、MediaPipe、Ultralytics 等输出的后处理 |

H7 Plus 走 Cortex-M7/CMSIS-NN 路径，适合紧凑的 int8 量化模型。牌角已知位置时应优先训练“分类器”（如 52 类，或拆成 13 点数 + 4花色两个头/两个模型），不要一开始就训练整帧目标检测器。

## 6. 推荐的识别流程

### 6.1 传统视觉版本（优先实现）

```text
采集灰度/RGB565 图像
  → 限定牌角可能出现的 ROI
  → 轻度滤波与阈值分割
  → 找牌区 blob / 牌边线段
  → 根据边方向估计角度
  → rotation_corr() 纠偏
  → 裁出固定大小的点数区、花色区
  → 点数模板匹配 + 花色模板/颜色判断
  → 分数阈值和几何规则联合判定
  → 低置信度帧保存到 microSD
```

实现顺序建议：

1. 固定相机、焦距、光源和背景，采集各张牌的真实样本；
2. 只做 ROI 裁剪，验证牌角始终落在范围内；
3. 完成角度估计与纠偏，观察归一化后的点数/花色位置是否稳定；
4. 建立 A～K 和四种花色模板，先离线测试阈值；
5. 加入红/黑颜色作为辅助特征，而不是唯一特征；
6. 记录每类最高分和次高分，要求“最高分超过阈值且与次高分有足够差距”；
7. 保存误识别和低置信度帧，迭代模板、阈值和照明。

### 6.2 何时切换到 ML

满足任一情况时值得尝试 `ml.Model`：

- 牌的厂商/字体很多，模板数量膨胀；
- 旋转、透视、污损或遮挡无法可靠归一化；
- 传统特征的最高分与次高分经常接近；
- 已有足够的标注样本，且愿意维护训练与量化流程。

即使使用 ML，也建议保留传统视觉的牌角定位/纠偏步骤，以减小模型输入和数据需求。

## 7. 性能、内存与可靠性建议

- 从 `QVGA + ROI + GRAYSCALE` 开始；只有字符像素不足时再提高分辨率。
- 让耗时算法只处理小 ROI。每帧运行多次全图 Hough、矩形检测或模板穷举会很慢。
- 复用已加载的模板和模型，不要在循环内反复从 SD 卡加载。
- 避免 Python 逐像素循环，优先使用 `Image` 的 C 实现方法。
- 大量临时 `copy()` 会造成内存压力；只在确实需要保留原图时复制。
- `time.clock()` / `clock.fps()` 用于评估吞吐；对关键阶段可用 ticks 计时。
- 每次输出“类别 + 置信度 + 几何质量”，不要只输出类别。连续多帧投票可降低偶发误判。
- 建立拒识状态：牌角不完整、角度超限、过曝、模糊或最高分过低时输出 `UNKNOWN`，不要强行选一类。
- OV5640 是滚动快门。若牌在运动中拍摄，应提高照明、缩短曝光，并在机械触发后等待牌稳定。

## 8. 最小项目模块清单

| 模块 | 是否必需 | 用途 |
|---|---:|---|
| `csi`（新固件）或 `sensor`（旧固件） | 是 | 摄像头配置与采集 |
| `image` | 是 | 图像对象、滤波、几何、检测、模板和描述子 |
| `os` | 是 | 文件/目录、容量检查、同步 |
| 内置 `open()` | 是 | 配置、标签、CSV 日志和二进制资源 |
| `time` | 建议 | 稳定等待、帧率和耗时评估 |
| `vfs` / `machine.SDCard` | 通常否 | 自定义 SD 卡挂载或底层块访问 |
| `json` | 建议 | 保存 ROI、阈值、固定曝光等配置 |
| `gc` | 调试时建议 | 查看/回收堆内存，诊断内存不足 |
| `ml` | 可选 | TFLite 分类/检测 |

## 9. 官方资料

以下链接均为 OpenMV 官方文档；应选择与设备实际固件版本一致的文档分支：

- [OpenMV Cam H7 Plus 硬件快速参考](https://docs.openmv.io/v5.0.0/openmvcam/quickref/openmv-cam-h7-plus.html)
- [`csi` 摄像头接口（v5.0）](https://docs.openmv.io/v5.0.0/library/omv.csi.html)
- [`sensor` 旧式摄像头接口（v4.5.6）](https://docs.openmv.io/v4.5.6/library/omv.sensor.html)
- [`image` 模块总览](https://docs.openmv.io/v5.0.0/library/omv.image.html)
- [`Image` 类完整接口](https://docs.openmv.io/v5.0.0/library/omv.image.Image.html)
- [模板匹配教程](https://docs.openmv.io/v5.0.0/openmvcam/tutorial/image/matching/template-and-similarity.html)
- [镜头与透视校正教程](https://docs.openmv.io/v5.0.0/openmvcam/tutorial/image/transforms/lens-and-perspective.html)
- [Blob 检测教程](https://docs.openmv.io/v5.0.0/openmvcam/tutorial/image/finding/blobs.html)
- [文件系统说明](https://docs.openmv.io/v5.0.0/reference/filesystem.html)
- [`os` 文件系统 API](https://docs.openmv.io/v5.0.0/library/os.html)
- [`machine.SDCard` API](https://docs.openmv.io/v5.0.0/library/machine.SDCard.html)
- [`ImageIO` 帧流 API](https://docs.openmv.io/v5.0.0/library/omv.image.ImageIO.html)
- [`ml` / TFLite 接口](https://docs.openmv.io/v5.0.0/library/omv.ml.html)

## 10. 建议的下一步验证

在决定模板、ORB 或 ML 之前，先采集一组真实帧，覆盖：13 种点数、4 种花色、允许的最大正负角度、正常/偏暗/偏亮、牌角处于 ROI 边界等情况。用这些图片测量以下指标：

- 牌角定位成功率；
- 纠偏后的关键区域位置方差；
- 每类模板的最高匹配分与次高分间隔；
- 单帧耗时和峰值内存；
- 低质量输入能否可靠拒识。

这些数据能直接判断简单模板法是否足够，以及真正需要扩充的是预处理、模板集合还是 ML 数据集。

## 11. 当前项目路径与基础识别程序

### 11.1 电脑端文件

当前工作目录：

```text
C:\Users\heng1\Desktop\发牌机\vision
```

主要文件：

| 文件 | 功能 |
|---|---|
| `template_1.py` | 在 OpenMV 上采集点数、花色、大小王和牌背模板 |
| `recognize_templates.py` | 使用分组 ORB 模板完成普通牌组合及特殊牌识别 |
| `OpenMV_H7_Plus_扑克牌识别接口调研.md` | 接口说明、方案和当前操作文档 |
| `temp/` | 电脑端旧测试图片，不会被设备端识别程序自动读取 |

### 11.2 OpenMV 设备端路径

两个脚本默认使用内部 Flash：

```text
/flash/templates/
  rank/
    A_00.pgm
    A_00.jpg
    2_00.pgm
    ...
    K_00.pgm
  suit/
    heart_00.pgm
    diamond_00.pgm
    club_00.pgm
    spade_00.pgm
  special/
    joker_00.pgm
    joker_01.pgm
    back_00.pgm
```

`.pgm` 是 ORB 算法模板，`.jpg` 是供人工检查的彩色参考图。电脑工作区的 `temp/` 只是旧测试数据；只有复制到上述设备目录，或者由 `template_1.py` 在设备上重新采集，才会被 `recognize_templates.py` 加载。

若需要频繁采集，可同时把两个脚本顶部的 `TEMPLATE_ROOT` 改为：

```python
TEMPLATE_ROOT = "/sdcard/templates"
```

修改时必须保证两个脚本路径一致。

### 11.3 `template_1.py` 当前功能

程序使用 `csi.CSI` 和 QVGA RGB565 图像。用户通过顶部配置选择模板组和标签：

```python
TEMPLATE_GROUP = "rank"
TEMPLATE_LABEL = "A"
```

支持的组合：

- `rank`：`A、2、3、4、5、6、7、8、9、10、J、Q、K`；
- `suit`：`heart、diamond、club、spade`；
- `special`：推荐使用 `joker、back`。大小王共用 Joker 形状模板，最后通过颜色区分。程序仍兼容名为 `joker_big`、`joker_small` 的旧模板，但加载后会把它们合并为同一个 `joker` 类别。

P9 与 GND 之间的按键用于拍摄。程序包含按键消抖、标签校验、曝光/增益/白平衡锁定、文件同步、异常提示和 ROI 预览。每次拍摄保存灰度 PGM 模板和彩色 JPG 参考图。

采集前必须通过 IDE 预览调整：

```python
RANK_ROI = (...)
SUIT_ROI = (...)
SPECIAL_ROI = (...)
```

ROI 应只包含对分类有用的区域，且同一组模板的位置与大小保持一致。大小王的 ROI 必须包含能够区分二者的颜色、文字或图案；若拍摄区域内没有区别，算法无法识别。

### 11.4 `recognize_templates.py` 当前功能

识别程序启动时读取三个模板目录并预提取 ORB 描述子。每轮分别在以下区域提取实时特征：

```python
RANK_SEARCH_ROI = (...)
SUIT_SEARCH_ROI = (...)
SPECIAL_SEARCH_ROI = (...)
```

普通牌只有在点数和花色都通过阈值时才输出，例如：

```text
heart_A
spade_10
diamond_Q
```

特殊模板直接输出：

```text
joker_big
joker_small
back
```

大小王只有颜色差异时，识别分成两个步骤：

1. 在 `SPECIAL_SEARCH_ROI` 中用灰度 ORB 判断当前图像是 `joker` 而不是普通牌或牌背；
2. 在 RGB565 原图的 `JOKER_COLOR_ROI` 中使用 LAB 红色阈值统计红色像素，红色达到 `JOKER_RED_MIN_PIXELS` 时输出 `joker_big`，否则输出 `joker_small`。

默认映射为“大王=红、小王=黑”。如果实际牌组相反，只需交换：

```python
RED_JOKER_LABEL = "joker_big"
BLACK_JOKER_LABEL = "joker_small"
```

`JOKER_RED_THRESHOLD` 和 `JOKER_RED_MIN_PIXELS` 必须通过 IDE 的 Threshold Editor 和串口中的 `joker_color: red_pixels=...` 输出实测标定。

没有提取到任何关键点时输出 `NO_CARD`；存在图像特征但无法形成可靠组合时输出 `UNKNOWN`。程序要求连续 `STABLE_ROUNDS` 轮结果一致才通过串口打印正式 `RESULT`。

IDE 预览中：

- 绿色框：点数搜索区域；
- 红色框：花色搜索区域；
- 蓝色框：特殊牌搜索区域；
- 彩色粗框：该组接受的匹配区域；
- 灰色粗框：存在候选，但没有通过阈值。

串口调试可以打开：

```python
PRINT_ALL_MATCHES = True
PRINT_ROUND_SUMMARY = True
```

`PRINT_ALL_MATCHES` 会输出每一张模板的匹配点数量，适合确定 `MIN_MATCH_COUNT` 和 `DESCRIPTOR_THRESHOLD`；正式运行时应关闭，以免串口输出拖慢识别。

### 11.5 板载白光

两个脚本都会在相机自动曝光、增益和白平衡稳定之前执行：

```python
red_led.on()
green_led.on()
blue_led.on()
```

RGB 三通道在模板采集和识别期间始终保持点亮，形成固定的板载可见白光。程序不再使用 RGB LED 显示就绪、保存、成功或错误状态，因为任何通道变化都会改变曝光和颜色。运行状态改为通过 IDE 画面和串口文字观察。

当前不启用 850 nm 红外补光，因为红外光会削弱大小王的可见颜色差异。采集模板与正式识别必须保持相同的 LED 设置、相机距离和环境遮光条件。

### 11.6 当前基础版本的限制

- 当前没有自动寻找整张牌或执行透视校正，依赖固定机械位置和较小方向偏差；
- `NO_CARD` 目前仅以“所有搜索区域均无 ORB 特征”为基础判断，纯色或强纹理背景可能导致误判；
- 点数和花色模板仍需使用最终相机、灯光和牌组重新采集；
- ORB 对低纹理字符的稳定性有限，模板不足时应增加真实角度样本；
- 不同牌背需要分别采样，可使用相同 `back` 标签保存多张模板；
- 后续可保持相同目录和输出协议，将点数/花色 ORB 替换为小型量化 CNN。

当前程序按照固件 v5 绘图接口传递坐标元组，例如：

```python
img.draw_string((4, 4), "rank:A", color=(0, 255, 0))
img.draw_cross((cx, cy), color=(0, 255, 0))
```

如果把坐标写成旧式的 `draw_string(4, 4, ...)`，部分 v5 固件会把第一个整数当成坐标对象并报告 `TypeError: object 'int' isn't a tuple or list`。

### 11.7 建议调试顺序

1. 先只采集 2～3 个点数，打开 `PRINT_ALL_MATCHES`，确认点数 ROI 和匹配框正确；
2. 补齐 13 个点数，每类采集多个允许角度下的样本；
3. 单独采集并调试四种花色；
4. 采集大小王和牌背，观察特殊牌得分是否明显高于普通牌；
5. 调整各分组的最低匹配点数、领先差值和角度上限；
6. 最后关闭逐模板输出，测试连续识别、无牌和错误摆放场景。
