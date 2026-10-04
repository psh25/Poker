"""OpenMV Cam H7 Plus：GPIO 单次触发的扑克牌识别程序（不使用 ORB）。

工作方式：
- 平时只采集并向 OpenMV IDE 持续传输画面，不执行模板匹配；
- P9 被按下（低电平）时，对当前帧执行一次识别；
- 结果立即以 ``RESULT:<label>`` 的形式输出到 USB 串口；
- 按键保持按下不会重复识别，必须释放后再次按下才会再次触发。

识别方法：
- 点数：在 rank 模板中进行一次快速初筛，只对命中的少数模板精细估分；
- 花色：先在 RGB565 原图中判定红/黑，再分别二选一模板匹配；
- 特殊牌：special 目录存在时，先尝试 joker/back 模板；
- 大小王：joker 形状匹配成功后，根据红色像素数量区分大小王。

设备端模板目录：
    /flash/templates/rank
    /flash/templates/suit
    /flash/templates/special       # 可选
"""

import csi
import gc
import image
import os
import time
from machine import LED, Pin


# ============================== GPIO 与相机 ==================================

TRIGGER_PIN = "P9"
TRIGGER_ACTIVE_LEVEL = 0
BUTTON_DEBOUNCE_MS = 50

TEMPLATE_ROOT = "/flash/templates"
CAMERA_SETTLE_MS = 2000

# 模板采集程序使用 RGB565 后再转灰度；识别端保持相同流程。
LOCK_CAMERA_SETTINGS = True

# 如果已经完成标定，可以填写固定值，使每次启动与采集模板时完全一致。
# 保持 None 时，程序先自动稳定，再读取当前值并锁定。
MANUAL_EXPOSURE_US = None
MANUAL_GAIN_DB = None
MANUAL_RGB_GAIN_DB = None


# ============================== ROI 配置 =====================================

# QVGA（320x240）坐标。ROI 越小，模板匹配越快，但必须容纳实际位置偏差。
RANK_SEARCH_ROI = (70, 15, 130, 160)
SUIT_SEARCH_ROI = (70, 110, 130, 125)
SPECIAL_SEARCH_ROI = (45, 5, 210, 230)
JOKER_COLOR_ROI = (70, 20, 150, 210)


# ============================== 匹配参数 =====================================

# 每张模板先只匹配一次。未命中的模板不再参与后续精细估分。
RANK_COARSE_THRESHOLD = 0.52
SUIT_COARSE_THRESHOLD = 0.52
SPECIAL_COARSE_THRESHOLD = 0.58

# 初筛后仅对少量候选二分估分；4轮约有 0.03 的分数分辨率。
REFINE_ROUNDS = 3
REFINE_HIGH = 0.98

# 第一名最低分数和相对第二名的最小分差。
RANK_ACCEPT_SCORE = 0.58
RANK_ACCEPT_MARGIN = 0.03
SUIT_ACCEPT_SCORE = 0.56
SUIT_ACCEPT_MARGIN = 0.03
SPECIAL_ACCEPT_SCORE = 0.64
SPECIAL_ACCEPT_MARGIN = 0.03

# 快速初筛使用 SEARCH_DS。全部未命中时，用一次 SEARCH_EX 复查，减少漏检。
FALLBACK_TO_EXHAUSTIVE = True
COARSE_EX_STEP = 3
REFINE_EX_STEP = 2


# ============================== 颜色参数 =====================================

# RGB565 图像使用 LAB 阈值。应使用 IDE Threshold Editor 按实际灯光标定。
RED_THRESHOLD = (20, 100, 15, 127, -10, 127)
SUIT_RED_MIN_PIXELS = 20
JOKER_RED_MIN_PIXELS = 30

RED_SUIT_LABELS = ("heart", "diamond")
BLACK_SUIT_LABELS = ("spade", "club")
RED_JOKER_LABEL = "joker_big"
BLACK_JOKER_LABEL = "joker_small"


# ============================== 调试配置 =====================================

PRINT_CANDIDATE_SCORES = True
DRAW_SEARCH_ROIS = True


# ============================== 模板加载 =====================================

def label_from_filename(filename):
    """把 A_00.pgm、heart_02.pgm 转换为 A、heart。"""
    stem = filename[:-4]
    separator = stem.rfind("_")

    if separator >= 0 and stem[separator + 1 :].isdigit():
        return stem[:separator]

    return stem


def load_template_group(group_name, required):
    """加载一个分组中的 P5 PGM；special 缺失时允许继续运行。"""
    directory = TEMPLATE_ROOT + "/" + group_name

    try:
        filenames = sorted(os.listdir(directory))
    except OSError as error:
        if required:
            raise OSError("Template directory unavailable: %s %s" % (directory, error))
        print("Optional template directory unavailable:", directory, error)
        return []

    catalogue = []
    print("Loading templates:", group_name)

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
            label = label_from_filename(filename)

            # 大小王共用形状模板，最终结果由颜色决定。
            if group_name == "special" and label.startswith("joker"):
                label = "joker"

            catalogue.append((label, filename, template))
            print(
                "  loaded %-22s -> %-12s %dx%d"
                % (filename, label, template.width(), template.height())
            )
        except Exception as error:
            print("  skipped unreadable template:", filename, error)
            if template is not None:
                del template

        gc.collect()

    print("  usable templates:", len(catalogue))
    return catalogue


def validate_template_sizes(catalogue, search_roi):
    """find_template 要求模板不能大于搜索 ROI。"""
    roi_width = search_roi[2]
    roi_height = search_roi[3]

    for label, filename, template in catalogue:
        if template.width() > roi_width or template.height() > roi_height:
            raise ValueError(
                "Template %s (%dx%d) is larger than ROI (%dx%d)"
                % (
                    filename,
                    template.width(),
                    template.height(),
                    roi_width,
                    roi_height,
                )
            )


# ============================== 快速模板匹配 ==================================

def label_is_allowed(label, allowed_labels):
    return allowed_labels is None or label in allowed_labels


def find_template_fast(gray, template, roi, threshold):
    """快速初筛；SEARCH_DS 不使用 step 参数。"""
    return gray.find_template(
        template,
        threshold,
        roi=roi,
        search=image.SEARCH_DS,
    )


def find_template_exhaustive(gray, template, roi, threshold, step):
    """更可靠的穷举搜索，只用于复查或少量候选精细估分。"""
    return gray.find_template(
        template,
        threshold,
        roi=roi,
        step=step,
        search=image.SEARCH_EX,
    )


def collect_coarse_candidates(gray, catalogue, roi, allowed_labels, threshold):
    """所有模板各快速匹配一次，返回超过初筛阈值的模板。"""
    candidates = []

    for label, filename, template in catalogue:
        if not label_is_allowed(label, allowed_labels):
            continue

        match_rect = find_template_fast(gray, template, roi, threshold)
        if match_rect is not None:
            candidates.append((label, filename, template, match_rect))

    # SEARCH_DS 可能在边缘漏检；仅当全部失败时才执行较粗步长的穷举复查。
    if not candidates and FALLBACK_TO_EXHAUSTIVE:
        for label, filename, template in catalogue:
            if not label_is_allowed(label, allowed_labels):
                continue

            match_rect = find_template_exhaustive(
                gray,
                template,
                roi,
                threshold,
                COARSE_EX_STEP,
            )
            if match_rect is not None:
                candidates.append((label, filename, template, match_rect))

    return candidates


def refine_candidate_score(gray, candidate, roi, coarse_threshold):
    """已知候选通过初筛，只在该候选上二分估算最高可通过阈值。"""
    label, filename, template, best_rect = candidate
    low = coarse_threshold
    high = REFINE_HIGH

    for unused in range(REFINE_ROUNDS):
        middle = (low + high) * 0.5
        match_rect = find_template_exhaustive(
            gray,
            template,
            roi,
            middle,
            REFINE_EX_STEP,
        )

        if match_rect is None:
            high = middle
        else:
            low = middle
            best_rect = match_rect

    return {
        "label": label,
        "filename": filename,
        "score": low,
        "rect": best_rect,
    }


def insert_label_best(best_by_label, result):
    """同一标签允许多张角度模板，但只保留最高分。"""
    previous = best_by_label.get(result["label"])
    if previous is None or result["score"] > previous["score"]:
        best_by_label[result["label"]] = result


def select_first_and_second(best_by_label):
    """不用依赖元组排序，避免同分时比较矩形对象。"""
    best = None
    second = None

    for label in best_by_label:
        result = best_by_label[label]

        if best is None or result["score"] > best["score"]:
            second = best
            best = result
        elif second is None or result["score"] > second["score"]:
            second = result

    return best, second


def match_group(
    gray,
    catalogue,
    roi,
    allowed_labels,
    coarse_threshold,
    accept_score,
    accept_margin,
    group_name,
):
    """初筛全部模板，只精细计算实际命中的少量候选。"""
    empty = {
        "group": group_name,
        "label": None,
        "score": 0.0,
        "second_score": 0.0,
        "margin": 0.0,
        "rect": None,
        "filename": None,
        "accepted": False,
        "candidate_count": 0,
    }

    if not catalogue:
        return empty

    candidates = collect_coarse_candidates(
        gray,
        catalogue,
        roi,
        allowed_labels,
        coarse_threshold,
    )

    if not candidates:
        return empty

    best_by_label = {}

    for candidate in candidates:
        result = refine_candidate_score(gray, candidate, roi, coarse_threshold)
        insert_label_best(best_by_label, result)

        if PRINT_CANDIDATE_SCORES:
            print(
                "  %-7s %-12s %-22s score=%.3f"
                % (group_name, result["label"], result["filename"], result["score"])
            )

    best, second = select_first_and_second(best_by_label)
    if best is None:
        return empty

    second_score = 0.0 if second is None else second["score"]
    margin = best["score"] - second_score
    accepted = best["score"] >= accept_score and margin >= accept_margin

    return {
        "group": group_name,
        "label": best["label"],
        "score": best["score"],
        "second_score": second_score,
        "margin": margin,
        "rect": best["rect"],
        "filename": best["filename"],
        "accepted": accepted,
        "candidate_count": len(candidates),
    }


# ============================== 颜色和最终决策 ================================

def count_red_pixels(color_img, roi):
    """统计 ROI 中符合 LAB 红色阈值的像素数量。"""
    blobs = color_img.find_blobs(
        [RED_THRESHOLD],
        roi=roi,
        pixels_threshold=4,
        area_threshold=4,
        merge=True,
        margin=2,
    )

    red_pixels = 0
    for blob in blobs:
        red_pixels += blob.pixels

    return red_pixels


def print_group_result(result):
    label = result["label"] if result["label"] is not None else "-"
    print(
        "  %s label=%s score=%.3f second=%.3f margin=%.3f candidates=%d ok=%d"
        % (
            result["group"],
            label,
            result["score"],
            result["second_score"],
            result["margin"],
            result["candidate_count"],
            1 if result["accepted"] else 0,
        )
    )


def recognize_once(color_img):
    """只处理当前帧一次，返回最终标签、绘制框和调试信息。"""
    gray = None
    boxes = []
    start_ms = time.ticks_ms()

    try:
        gray = color_img.to_grayscale(copy=True)

        # 有 special 模板时优先检查 joker/back；目录缺失则完全跳过。
        special_result = match_group(
            gray,
            special_catalogue,
            SPECIAL_SEARCH_ROI,
            None,
            SPECIAL_COARSE_THRESHOLD,
            SPECIAL_ACCEPT_SCORE,
            SPECIAL_ACCEPT_MARGIN,
            "special",
        )

        if special_result["accepted"]:
            special_label = special_result["label"]
            red_pixels = -1

            if special_label == "joker":
                red_pixels = count_red_pixels(color_img, JOKER_COLOR_ROI)
                special_label = (
                    RED_JOKER_LABEL
                    if red_pixels >= JOKER_RED_MIN_PIXELS
                    else BLACK_JOKER_LABEL
                )

            boxes.append((special_result["rect"], (255, 0, 0)))
            elapsed_ms = time.ticks_diff(time.ticks_ms(), start_ms)
            print_group_result(special_result)
            if red_pixels >= 0:
                print("  joker red_pixels=", red_pixels)
            return special_label, boxes, elapsed_ms

        # 点数保留模板匹配，但每张模板只做一次初筛。
        rank_result = match_group(
            gray,
            rank_catalogue,
            RANK_SEARCH_ROI,
            None,
            RANK_COARSE_THRESHOLD,
            RANK_ACCEPT_SCORE,
            RANK_ACCEPT_MARGIN,
            "rank",
        )

        # 花色先按颜色拆成两个候选，避免四种花色同时竞争。
        suit_red_pixels = count_red_pixels(color_img, SUIT_SEARCH_ROI)
        if suit_red_pixels >= SUIT_RED_MIN_PIXELS:
            suit_color = "red"
            allowed_suits = RED_SUIT_LABELS
        else:
            suit_color = "black"
            allowed_suits = BLACK_SUIT_LABELS

        suit_result = match_group(
            gray,
            suit_catalogue,
            SUIT_SEARCH_ROI,
            allowed_suits,
            SUIT_COARSE_THRESHOLD,
            SUIT_ACCEPT_SCORE,
            SUIT_ACCEPT_MARGIN,
            "suit",
        )

        print_group_result(rank_result)
        print(
            "  suit color=%s red_pixels=%d allowed=%s"
            % (suit_color, suit_red_pixels, allowed_suits)
        )
        print_group_result(suit_result)

        if rank_result["rect"] is not None:
            boxes.append((rank_result["rect"], (0, 255, 0)))
        if suit_result["rect"] is not None:
            boxes.append((suit_result["rect"], (0, 128, 255)))

        if rank_result["accepted"] and suit_result["accepted"]:
            final_label = "%s_%s" % (suit_result["label"], rank_result["label"])
        else:
            final_label = "UNKNOWN"

        elapsed_ms = time.ticks_diff(time.ticks_ms(), start_ms)
        return final_label, boxes, elapsed_ms
    finally:
        if gray is not None:
            del gray
        gc.collect()


# ============================== 灯光与相机初始化 ==============================

def keep_white_light_on():
    red_led.on()
    green_led.on()
    blue_led.on()


red_led = LED("LED_RED")
green_led = LED("LED_GREEN")
blue_led = LED("LED_BLUE")
keep_white_light_on()

trigger = Pin(TRIGGER_PIN, Pin.IN, Pin.PULL_UP)

rank_catalogue = load_template_group("rank", required=True)
suit_catalogue = load_template_group("suit", required=True)
special_catalogue = load_template_group("special", required=False)

if not rank_catalogue:
    raise OSError("No rank templates under " + TEMPLATE_ROOT + "/rank")
if not suit_catalogue:
    raise OSError("No suit templates under " + TEMPLATE_ROOT + "/suit")

validate_template_sizes(rank_catalogue, RANK_SEARCH_ROI)
validate_template_sizes(suit_catalogue, SUIT_SEARCH_ROI)
validate_template_sizes(special_catalogue, SPECIAL_SEARCH_ROI)

cam = csi.CSI()
cam.reset()
cam.pixformat(csi.RGB565)
cam.framesize(csi.QVGA)
cam.framebuffers(1)
cam.snapshot(time=CAMERA_SETTLE_MS)

if LOCK_CAMERA_SETTINGS:
    exposure_us = (
        cam.exposure_us() if MANUAL_EXPOSURE_US is None else MANUAL_EXPOSURE_US
    )
    gain_db = cam.gain_db() if MANUAL_GAIN_DB is None else MANUAL_GAIN_DB
    rgb_gain_db = (
        cam.rgb_gain_db() if MANUAL_RGB_GAIN_DB is None else MANUAL_RGB_GAIN_DB
    )

    cam.auto_exposure(False, exposure_us=exposure_us)
    cam.auto_gain(False, gain_db=gain_db)
    cam.auto_whitebal(False, rgb_gain_db=rgb_gain_db)

    print("Camera locked:")
    print("  exposure_us=", exposure_us)
    print("  gain_db=", gain_db)
    print("  rgb_gain_db=", rgb_gain_db)

print("Triggered recognition ready:")
print("  trigger pin=", TRIGGER_PIN, "active level=", TRIGGER_ACTIVE_LEVEL)
print("  press once -> recognize current frame once")
print("  result format: RESULT:<label>")


# ============================== 实时预览与触发 ================================

last_raw_value = trigger.value()
stable_value = last_raw_value
last_change_ms = time.ticks_ms()
display_label = "READY"
last_boxes = []

while True:
    # snapshot 持续更新帧缓冲区，因此 IDE 可以持续接收预览图像。
    img = cam.snapshot()
    now_ms = time.ticks_ms()
    raw_value = trigger.value()

    if raw_value != last_raw_value:
        last_raw_value = raw_value
        last_change_ms = now_ms

    # 只有稳定电平发生变化才更新按键状态，完成软件消抖。
    if (
        raw_value != stable_value
        and time.ticks_diff(now_ms, last_change_ms) >= BUTTON_DEBOUNCE_MS
    ):
        stable_value = raw_value

        # 只响应按下沿。保持按下不会重复，释放后才能再次触发。
        if stable_value == TRIGGER_ACTIVE_LEVEL:
            print("TRIGGER")
            keep_white_light_on()

            try:
                display_label, last_boxes, elapsed_ms = recognize_once(img)
                print("RESULT:" + display_label)
                print("RECOGNITION_MS:", elapsed_ms)
            except Exception as error:
                display_label = "ERROR"
                last_boxes = []
                print("RESULT:ERROR")
                print("RECOGNITION_ERROR:", error)

            keep_white_light_on()

    if DRAW_SEARCH_ROIS:
        img.draw_rectangle(RANK_SEARCH_ROI, color=(0, 180, 0), thickness=1)
        img.draw_rectangle(SUIT_SEARCH_ROI, color=(0, 100, 180), thickness=1)

    for rect, color in last_boxes:
        if rect is not None:
            img.draw_rectangle(rect, color=color, thickness=2)

    img.draw_string((2, 2), display_label, color=(255, 255, 255))

