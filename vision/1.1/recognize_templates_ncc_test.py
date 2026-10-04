"""OpenMV Cam H7 Plus：不使用 ORB 的简单模板匹配测试程序。

用途：
- 验证 /flash/templates 中的 PGM 模板能否直接用于图像匹配；
- 默认只测试一个分组，减少运算量，方便观察串口输出；
- 使用 NCC（归一化互相关）find_template()，不提取关键点。

使用方法：
1. 修改 TEST_GROUP，可选 "rank"、"suit"、"special"；
2. 把对应扑克牌放到采集模板时的位置；
3. 在 IDE 中运行，观察 BEST、候选分数和画面矩形；
4. 根据现场结果调整 SEARCH_ROI、ACCEPT_SCORE 和 ACCEPT_MARGIN。

注意：NCC 对旋转和尺度变化比较敏感。正式使用时，建议每个类别采集多张
不同小角度的模板，例如 label_00.pgm、label_01.pgm、label_02.pgm。
"""

import csi
import gc
import image
import os
import time
from machine import LED


# ============================== 用户配置 =====================================

TEMPLATE_ROOT = "/flash/templates"

# 一次只测试一个分组，避免 OpenMV 同时做大量穷举匹配。
TEST_GROUP = "rank"

# QVGA（320x240）下的搜索区域，应比模板区域略大。
SEARCH_ROI_BY_GROUP = {
    "rank": (70, 15, 130, 160),
    "suit": (70, 110, 130, 125),
    "special": (45, 5, 210, 230),
}

# 最低匹配分数。范围为 0.0～1.0，越高越严格。
ACCEPT_SCORE = 0.55

# 第一名至少比第二名高出该值，否则判为歧义。
ACCEPT_MARGIN = 0.03

# 为了获得便于比较的近似分数，程序使用二分法多次调用 find_template()。
# 次数越多分数越精细，但运行越慢。4～5 适合调试。
SCORE_SEARCH_ROUNDS = 4
SCORE_SEARCH_MIN = 0.35
SCORE_SEARCH_MAX = 0.98

# SERACH_DX 菱形搜索更快，SEARCH_EX 穷举搜索更可靠但更慢；步长增大可以提高速度、降低定位精度。
SEARCH_STEP = 2
RECOGNITION_INTERVAL_MS = 1000

# 先在白光下等待自动曝光稳定，然后锁定曝光和增益。
CAMERA_SETTLE_MS = 2000
LOCK_CAMERA_SETTINGS = True

# 是否打印每张模板的近似得分。
PRINT_ALL_SCORES = True


# ============================== 工具函数 =====================================

def label_from_filename(filename):
    """把 heart_00.pgm 转换为 heart；允许同一标签有多张模板。"""
    stem = filename[:-4]
    separator = stem.rfind("_")

    if separator >= 0 and stem[separator + 1 :].isdigit():
        return stem[:separator]

    return stem


def load_templates(group_name):
    """加载所选分组的 P5 PGM；NCC 不需要 ORB 关键点。"""
    directory = TEMPLATE_ROOT + "/" + group_name

    try:
        filenames = sorted(os.listdir(directory))
    except OSError as error:
        raise OSError("Template directory unavailable: %s %s" % (directory, error))

    catalogue = []
    print("Loading NCC templates:", directory)

    for filename in filenames:
        if not filename.lower().endswith(".pgm"):
            continue

        path = directory + "/" + filename
        template = None

        try:
            file_size = os.stat(path)[6]
            with open(path, "rb") as template_file:
                file_header = template_file.read(2)

            if file_size < 16 or file_header != b"P5":
                print("  skipped invalid PGM:", filename)
                continue

            template = image.Image(path, copy_to_fb=False)
            catalogue.append((label_from_filename(filename), filename, template))
            print(
                "  loaded %-22s size=%dx%d"
                % (filename, template.width(), template.height())
            )
        except Exception as error:
            print("  skipped unreadable template:", filename, error)
            if template is not None:
                del template

        gc.collect()

    print("Usable NCC templates:", len(catalogue))
    return catalogue


def validate_template_sizes(catalogue, search_roi):
    """find_template 要求模板尺寸不能大于搜索区域。"""
    search_width = search_roi[2]
    search_height = search_roi[3]

    for label, filename, template in catalogue:
        if template.width() > search_width or template.height() > search_height:
            raise ValueError(
                "Template %s (%dx%d) is larger than search ROI (%dx%d)"
                % (
                    filename,
                    template.width(),
                    template.height(),
                    search_width,
                    search_height,
                )
            )


def find_at_threshold(gray_image, template, search_roi, threshold):
    """在指定阈值下执行一次 NCC 穷举搜索。"""
    return gray_image.find_template(
        template,
        threshold,
        roi=search_roi,
        step=SEARCH_STEP,
        search=image.SEARCH_DS,
    )


def estimate_score(gray_image, template, search_roi):
    """二分查找最高可通过阈值，返回近似分数和最后一次匹配矩形。"""
    low = SCORE_SEARCH_MIN
    high = SCORE_SEARCH_MAX
    best_rect = find_at_threshold(gray_image, template, search_roi, low)

    if best_rect is None:
        return 0.0, None

    for unused in range(SCORE_SEARCH_ROUNDS):
        middle = (low + high) * 0.5
        match_rect = find_at_threshold(gray_image, template, search_roi, middle)

        if match_rect is None:
            high = middle
        else:
            low = middle
            best_rect = match_rect

    return low, best_rect


def match_catalogue(gray_image, catalogue, search_roi):
    """逐张测试模板；同一标签只保留得分最高的一张。"""
    best_by_label = {}

    for label, filename, template in catalogue:
        score, match_rect = estimate_score(gray_image, template, search_roi)

        if PRINT_ALL_SCORES:
            print("  %-12s %-22s score=%.3f" % (label, filename, score))

        previous = best_by_label.get(label)
        if previous is None or score > previous[0]:
            best_by_label[label] = (score, match_rect, filename)

    ranked = []
    for label in best_by_label:
        score, match_rect, filename = best_by_label[label]
        ranked.append((score, label, match_rect, filename))

    ranked.sort(reverse=True)

    if not ranked:
        return None, 0.0, 0.0, None, None, False

    best_score, best_label, best_rect, best_filename = ranked[0]
    second_score = ranked[1][0] if len(ranked) > 1 else 0.0
    accepted = (
        best_rect is not None
        and best_score >= ACCEPT_SCORE
        and (best_score - second_score) >= ACCEPT_MARGIN
    )

    return (
        best_label,
        best_score,
        second_score,
        best_rect,
        best_filename,
        accepted,
    )


def keep_white_light_on():
    """同时点亮板载红、绿、蓝 LED，提供固定白光。"""
    red_led.on()
    green_led.on()
    blue_led.on()


# ============================== 初始化 ========================================

if TEST_GROUP not in SEARCH_ROI_BY_GROUP:
    raise ValueError("Invalid TEST_GROUP: " + TEST_GROUP)

red_led = LED("LED_RED")
green_led = LED("LED_GREEN")
blue_led = LED("LED_BLUE")
keep_white_light_on()

search_roi = SEARCH_ROI_BY_GROUP[TEST_GROUP]
catalogue = load_templates(TEST_GROUP)

if not catalogue:
    raise OSError("No readable PGM templates under " + TEMPLATE_ROOT + "/" + TEST_GROUP)

validate_template_sizes(catalogue, search_roi)

cam = csi.CSI()
cam.reset()
cam.pixformat(csi.GRAYSCALE)
cam.framesize(csi.QVGA)
cam.framebuffers(1)
cam.snapshot(time=CAMERA_SETTLE_MS)

if LOCK_CAMERA_SETTINGS:
    exposure_us = cam.exposure_us()
    gain_db = cam.gain_db()
    cam.auto_exposure(False, exposure_us=exposure_us)
    cam.auto_gain(False, gain_db=gain_db)
    print("Camera locked: exposure_us=", exposure_us, "gain_db=", gain_db)

print("NCC test group:", TEST_GROUP)
print("Search ROI:", search_roi)
print("Accept: score>=%.2f margin>=%.2f" % (ACCEPT_SCORE, ACCEPT_MARGIN))


# ============================== 测试主循环 ====================================

last_recognition_ms = time.ticks_ms() - RECOGNITION_INTERVAL_MS
display_text = "waiting"

while True:
    img = cam.snapshot()
    now_ms = time.ticks_ms()

    # 始终画出搜索区域，便于检查牌角是否放置正确。
    img.draw_rectangle(search_roi, color=180, thickness=1)

    if time.ticks_diff(now_ms, last_recognition_ms) >= RECOGNITION_INTERVAL_MS:
        last_recognition_ms = now_ms

        try:
            print("MATCH ROUND:")
            (
                best_label,
                best_score,
                second_score,
                best_rect,
                best_filename,
                accepted,
            ) = match_catalogue(img, catalogue, search_roi)

            margin = best_score - second_score
            status = "ACCEPT" if accepted else "REJECT"
            display_text = "%s %s %.2f" % (status, best_label, best_score)

            print(
                "BEST: status=%s label=%s score=%.3f second=%.3f margin=%.3f file=%s"
                % (
                    status,
                    best_label,
                    best_score,
                    second_score,
                    margin,
                    best_filename,
                )
            )

            if best_rect is not None:
                img.draw_rectangle(
                    best_rect,
                    color=255 if accepted else 100,
                    thickness=2,
                )
        except Exception as error:
            display_text = "ERROR"
            print("NCC_TEST_ERROR:", error)

        keep_white_light_on()
        gc.collect()

    img.draw_string((2, 2), display_text, color=255)

