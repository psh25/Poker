# OpenMV 扑克牌角标识别

使用 OpenMV Cam H7 Plus 和 MicroPython `csi` 接口，识别固定牌型的点数、花色、大小王及牌背。适用于拍摄牌角、距离固定、位置和角度小范围变化的场景，使用板载补光与锁定的曝光、增益、白平衡。当前不依赖 SD 卡。

算法流程：LAB 红黑墨迹定位 → 连通域与布局筛选 → 局部 Otsu 二值化 → 等比例归一化 → 小图像素差模板比较 → 分数、分差、颜色和布局综合判决。低置信度时在同一帧尝试角度及阈值补偿；`UNKNOWN` 表示证据不足或超时，当前没有无牌检测。

## 项目目录

| 路径 | 用途 |
|---|---|
| `3.1hybrid/` | 当前使用的混合定位识别方案、采集程序和通信文档 |
| `templates/` | 当前模板库：`rank`、`suit`、`joker`、`back`，PGM/JSON 同名配对 |
| `2.1fast/` | 保留的早期小模板方案，使用自己的配置和核心模块 |
| `1.1/` | 保留的早期模板匹配及 LED/UART 调试程序 |
| `tests/` | 电脑端测试与离线评估脚本，不上传到 OpenMV |

当前使用 `3.1hybrid` 的以下文件：

- `capture_cards_hybrid.py`：相机标定和模板采集；采集前检查标签、ROI、`CALIBRATE_CAMERA` 和 `SAVE_TEMPLATES`。
- `recognize_cards_hybrid.py`：IDE 调试入口，每 3 秒自动识别，结果及诊断通过 USB 输出。
- `main_standalone.py`：部署入口，P6 下降沿触发一次识别，UART3（P4/TX、P5/RX，115200、8N1）返回 `RESULT:<label>`；支持 `CALIBRATE` 校准命令。
- `cards_fast_config.py`、`cards_hybrid_core.py`：公共配置与算法；两个识别入口的 ROI 和判决参数仍分别定义，调试后需同步到部署入口。

历史版本的模板格式可能不同，应使用对应版本重新采集，避免与当前模板混用。

## 部署到 OpenMV

将 `3.1hybrid/main_standalone.py` 复制为板上的 `main.py`，将同目录的配置与核心模块一起复制到 `/flash`。将本项目根目录 `templates/` 复制到 `/flash/cards_fast_v1/templates/`：

```text
/flash/
├── main.py
├── cards_fast_config.py
├── cards_hybrid_core.py
└── cards_fast_v1/
    ├── camera.json             # 在实际补光条件下标定生成
    └── templates/
        ├── rank/
        ├── suit/
        ├── joker/
        └── back/
```

首次使用先通过采集脚本生成 `camera.json`，再运行识别。更新板端配置或核心模块后复位 OpenMV。IDE 可直接执行电脑端入口脚本，但导入的公共模块来自板端。

详见 [使用说明](3.1hybrid/OPENMV_CARDS_HYBRID_README.md)、[通信协议](3.1hybrid/STANDALONE_IO_PROTOCOL.md) 和 [技术框图与 Flash 目录](3.1hybrid/图像识别系统技术路径.md)。

## 电脑端验证

安装 `pytest`、`numpy`、`opencv-python` 后，在项目根目录执行（PowerShell）：

```powershell
$env:PYTHONPATH = (Join-Path (Get-Location) '3.1hybrid')
python -m pytest -q tests/test_cards_hybrid.py
```

主机端测试不能代表板端速度或实拍准确率。离线评估脚本需要自行提供 `pok/` 图片；这些测试图片不纳入上传范围。

## Git 文件范围

`.gitignore` 保留程序、Markdown 文档、测试脚本、`pytest.ini` 和根目录 `templates/` 的 PGM/JSON。缓存、截图、日志、离线评估结果及重复模板副本默认忽略；`3.1hybrid/templates/` 与根目录模板当前一致，上传时仅保留根目录的一份。

忽略规则不影响已有提交历史，也不会自动移除已跟踪文件。上传前可用 `git ls-files -ci --exclude-standard` 查看仍需从 Git 索引移除的文件。
