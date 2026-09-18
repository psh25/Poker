"""OpenMV Cam H7 Plus 扑克牌模板采集程序。

接线：P9 ---- 瞬时按键 ---- GND

使用步骤：
1. 修改 TEMPLATE_GROUP 和 TEMPLATE_LABEL。
2. 根据 IDE 预览中的矩形调整对应 ROI。
3. 将牌放稳后按下 P9 按键；每次按下保存一组 PGM/JPG 文件。

模板分组：
- rank：A、2～10、J、Q、K
- suit：heart、diamond、club、spade
- special：joker、back（大小王共用 joker 形状模板，识别时按颜色区分）
"""

import csi
import gc
import os
import time
from machine import LED, Pin


# ============================== 用户配置 =====================================

# 可选值："rank"、"suit"、"special"。
TEMPLATE_GROUP = "suit"

# 示例：
# rank    -> "A"、"2"、...、"10"、"J"、"Q"、"K"
# suit    -> "heart"、"diamond"、"club"、"spade"
# special -> 推荐使用 "joker" 或 "back"
# 为兼容已有文件，也允许 "joker_big" 和 "joker_small"；识别时仍按颜色区分。
TEMPLATE_LABEL = "heart"

# OpenMV 设备端模板根目录，固定使用内部 Flash。
TEMPLATE_ROOT = "/flash/templates"

# 以下 ROI 均为 QVGA（320x240）坐标：(x, y, w, h)。
# 当前数值只是便于启动的默认值，必须根据实际机械位置调整。
RANK_ROI = (95, 35, 80, 120)
SUIT_ROI = (95, 140, 70, 70)
SPECIAL_ROI = (70, 20, 150, 210)

JPEG_REFERENCE_QUALITY = 90

# 按键不用时可打开自动采集；启动后等待指定时间，只采集一次。
AUTO_CAPTURE_ON_START = False
AUTO_CAPTURE_DELAY_MS = 3000

# 按键消抖时间。
BUTTON_DEBOUNCE_MS = 100

# 先让自动曝光稳定，再锁定曝光、增益和白平衡。
LOCK_CAMERA_SETTINGS = True
CAMERA_SETTLE_MS = 2000

# 是否保存一张未裁剪的整帧调试图。大量采集时建议关闭。
SAVE_FULL_FRAME_DEBUG = False


# ============================== 固定配置 =====================================

VALID_LABELS = {
    "rank": ("A", "2", "3", "4", "5", "6", "7", "8", "9", "10", "J", "Q", "K"),
    "suit": ("heart", "diamond", "club", "spade"),
    "special": ("joker", "joker_big", "joker_small", "back"),
}

ROI_BY_GROUP = {
    "rank": RANK_ROI,
    "suit": SUIT_ROI,
    "special": SPECIAL_ROI,
}

ROI_COLOR = {
    "rank": (0, 255, 0),
    "suit": (255, 0, 0),
    "special": (0, 0, 255),
}


def validate_config():
    """尽早检查拼写，避免采集完成后才发现标签错误。"""
    if TEMPLATE_GROUP not in VALID_LABELS:
        raise ValueError("Invalid TEMPLATE_GROUP: " + TEMPLATE_GROUP)

    if TEMPLATE_LABEL not in VALID_LABELS[TEMPLATE_GROUP]:
        raise ValueError(
            "Invalid label '%s' for group '%s'" % (TEMPLATE_LABEL, TEMPLATE_GROUP)
        )


def ensure_directory(path):
    """创建单级目录；只有确认目录已经存在时才忽略异常。"""
    try:
        os.mkdir(path)
    except OSError:
        # 目录损坏、只读或路径类型错误时，listdir 会继续抛出异常。
        os.listdir(path)


def next_template_paths(group_dir):
    """生成下一组未使用的模板、参考图和整帧调试图路径。"""
    index = 0

    while True:
        stem = "%s/%s_%02d" % (group_dir, TEMPLATE_LABEL, index)
        pgm_path = stem + ".pgm"

        try:
            os.stat(pgm_path)
            index += 1
        except OSError:
            return pgm_path, stem + ".jpg", stem + "_full.jpg"


def keep_white_light_on():
    """同时点亮 RGB 三通道，形成拍摄期间保持不变的板载白光。"""
    red_led.on()
    green_led.on()
    blue_led.on()


def save_template(source_image):
    """保存灰度算法模板、彩色参考图，以及可选的整帧调试图。"""
    roi = ROI_BY_GROUP[TEMPLATE_GROUP]
    color_crop = None
    gray_crop = None

    try:
        # 保存前再次确认补光状态，不能用 RGB LED 显示保存/错误状态。
        keep_white_light_on()
        color_crop = source_image.copy(roi=roi)
        gray_crop = color_crop.to_grayscale(copy=True)
        pgm_path, jpg_path, full_path = next_template_paths(group_dir)

        gray_crop.save(pgm_path)
        color_crop.save(jpg_path, quality=JPEG_REFERENCE_QUALITY)

        if SAVE_FULL_FRAME_DEBUG:
            source_image.save(full_path, quality=JPEG_REFERENCE_QUALITY)

        # FAT 文件系统不耐受写入时断电，成功提示前先同步。
        os.sync()
        print("CAPTURE_OK group=%s label=%s" % (TEMPLATE_GROUP, TEMPLATE_LABEL))
        print("  template:", pgm_path)
        print("  reference:", jpg_path)
        if SAVE_FULL_FRAME_DEBUG:
            print("  full_frame:", full_path)

        keep_white_light_on()
        return True
    except Exception as error:
        print("CAPTURE_ERROR:", error)
        keep_white_light_on()
        return False
    finally:
        if gray_crop is not None:
            del gray_crop
        if color_crop is not None:
            del color_crop
        gc.collect()


# ============================== 初始化 ========================================

validate_config()

red_led = LED("LED_RED")
green_led = LED("LED_GREEN")
blue_led = LED("LED_BLUE")

# H7 Plus 的 IR LED 逻辑特殊，此处保持用户现有设置，不启用红外补光。

# 在相机自动曝光、增益和白平衡稳定之前开启板载白光。
keep_white_light_on()
capture_button = Pin("P9", Pin.IN, Pin.PULL_UP)

ensure_directory(TEMPLATE_ROOT)
group_dir = TEMPLATE_ROOT + "/" + TEMPLATE_GROUP
ensure_directory(group_dir)

cam = csi.CSI()
cam.reset()
cam.pixformat(csi.RGB565)
cam.framesize(csi.QVGA)
cam.framebuffers(1)
cam.snapshot(time=CAMERA_SETTLE_MS)

if LOCK_CAMERA_SETTINGS:
    exposure_us = cam.exposure_us()
    gain_db = cam.gain_db()
    rgb_gain_db = cam.rgb_gain_db()

    cam.auto_exposure(False, exposure_us=exposure_us)
    cam.auto_gain(False, gain_db=gain_db)
    cam.auto_whitebal(False, rgb_gain_db=rgb_gain_db)

    print("Camera locked:")
    print("  exposure_us=", exposure_us)
    print("  gain_db=", gain_db)
    print("  rgb_gain_db=", rgb_gain_db)

print("Template capture ready:")
print("  group=", TEMPLATE_GROUP)
print("  label=", TEMPLATE_LABEL)
print("  roi=", ROI_BY_GROUP[TEMPLATE_GROUP])
print("  directory=", group_dir)
print("  onboard RGB white light=ON")
keep_white_light_on()


# ============================== 拍摄主循环 ====================================

last_button_value = capture_button.value()
last_capture_ms = time.ticks_ms() - BUTTON_DEBOUNCE_MS
start_ms = time.ticks_ms()
auto_capture_done = not AUTO_CAPTURE_ON_START

while True:
    img = cam.snapshot()
    now_ms = time.ticks_ms()

    button_value = capture_button.value()
    button_pressed = last_button_value == 1 and button_value == 0
    last_button_value = button_value

    debounce_ok = (
        time.ticks_diff(now_ms, last_capture_ms) >= BUTTON_DEBOUNCE_MS
    )
    auto_capture_due = (
        not auto_capture_done
        and time.ticks_diff(now_ms, start_ms) >= AUTO_CAPTURE_DELAY_MS
    )

    if (button_pressed and debounce_ok) or auto_capture_due:
        if save_template(img):
            last_capture_ms = now_ms
        auto_capture_done = True

    # 保存后再绘制，保证调试框不会进入模板。
    roi = ROI_BY_GROUP[TEMPLATE_GROUP]
    img.draw_rectangle(roi, color=ROI_COLOR[TEMPLATE_GROUP], thickness=2)
    img.draw_string(
        (4, 4),
        "%s:%s" % (TEMPLATE_GROUP, TEMPLATE_LABEL),
        color=ROI_COLOR[TEMPLATE_GROUP],
    )
