"""OpenMV Cam H7 Plus 扑克牌分组模板识别程序。

识别结果：
- 普通牌：heart_A、spade_10 等“花色_点数”组合；
- 特殊牌：先用 ORB 识别 joker/back，再按颜色输出 joker_big/joker_small；
- 拒识状态：NO_CARD、UNKNOWN。

模板由 template_1.py 采集，设备端目录为：
    /flash/templates/rank
    /flash/templates/suit
    /flash/templates/special

这是便于调试的基础版本。它使用 ORB 分组匹配，适合固定距离、牌角大致
位于固定区域且只有小幅旋转/尺度偏差的场景。正式使用前必须标定 ROI 和阈值。
"""

import csi
import gc
import image
import os
import time
from machine import LED


# ============================== 用户配置 =====================================

TEMPLATE_ROOT = "/flash/templates"

# QVGA（320x240）搜索区域。比采集模板 ROI 略大，用于容纳位置和角度偏差。
# 当前值仅为默认值，必须根据实际安装位置调整。
RANK_SEARCH_ROI = (70, 15, 130, 160)
SUIT_SEARCH_ROI = (70, 110, 130, 125)
SPECIAL_SEARCH_ROI = (45, 5, 210, 230)

# 大小王只有颜色区别时，在此区域统计红色像素。
# 默认假设大王为红色、小王为黑色；如实际牌组相反，交换两个标签即可。
JOKER_COLOR_ROI = (70, 20, 150, 210)
RED_JOKER_LABEL = "joker_big"
BLACK_JOKER_LABEL = "joker_small"

# LAB 红色阈值：(L_min, L_max, A_min, A_max, B_min, B_max)。
# 必须使用 IDE 的 Threshold Editor 根据实际白光和牌面重新标定。
JOKER_RED_THRESHOLD = (20, 100, 15, 127, -10, 127)
JOKER_RED_MIN_PIXELS = 30

# 模板关键点配置。
TEMPLATE_KEYPOINT_THRESHOLD = 5
TEMPLATE_MAX_KEYPOINTS = 100

# 实时画面关键点配置。降低 threshold 会增加关键点和运行时间。
SCENE_KEYPOINT_THRESHOLD = 18
SCENE_MAX_KEYPOINTS = 180
KEYPOINT_SCALE_FACTOR = 1.2

# ORB 描述子匹配阈值：0～100，越低越严格。
DESCRIPTOR_THRESHOLD = 68
FILTER_OUTLIERS = True

# 各分组的最低匹配数量和第一、二名最小差值。
RANK_MIN_MATCH_COUNT = 9
RANK_MIN_COUNT_MARGIN = 3
SUIT_MIN_MATCH_COUNT = 7
SUIT_MIN_COUNT_MARGIN = 2
SPECIAL_MIN_MATCH_COUNT = 10
SPECIAL_MIN_COUNT_MARGIN = 3

# 特殊牌与普通牌同时成立时，特殊牌至少要比普通牌的单项最高分高出该值。
SPECIAL_PRIORITY_MARGIN = 2

# 根据机械结构限制合理角度，超过范围时拒识。
MAX_ABS_MATCH_ANGLE = 30

# 连续若干次得到相同结果后才正式输出。
STABLE_ROUNDS = 3
RECOGNITION_INTERVAL_MS = 300

# 打开后会打印每个模板的匹配数量，标定阈值时非常有用。
PRINT_ALL_MATCHES = False

# 每轮输出三个分组的最佳候选，正常运行时可以关闭。
PRINT_ROUND_SUMMARY = True

LOCK_CAMERA_SETTINGS = True
CAMERA_SETTLE_MS = 2000


# ============================== 固定配置 =====================================

GROUP_CONFIG = {
    "rank": {
        "directory": TEMPLATE_ROOT + "/rank",
        "roi": RANK_SEARCH_ROI,
        "min_count": RANK_MIN_MATCH_COUNT,
        "min_margin": RANK_MIN_COUNT_MARGIN,
        "color": (0, 255, 0),
    },
    "suit": {
        "directory": TEMPLATE_ROOT + "/suit",
        "roi": SUIT_SEARCH_ROI,
        "min_count": SUIT_MIN_MATCH_COUNT,
        "min_margin": SUIT_MIN_COUNT_MARGIN,
        "color": (255, 0, 0),
    },
    "special": {
        "directory": TEMPLATE_ROOT + "/special",
        "roi": SPECIAL_SEARCH_ROI,
        "min_count": SPECIAL_MIN_MATCH_COUNT,
        "min_margin": SPECIAL_MIN_COUNT_MARGIN,
        "color": (0, 0, 255),
    },
}


def label_from_filename(filename):
    """把 A_00.pgm、joker_big_02.pgm 转换为 A、joker_big。"""
    stem = filename[:-4]
    separator = stem.rfind("_")

    if separator >= 0 and stem[separator + 1 :].isdigit():
        return stem[:separator]

    return stem


def find_pgm_files(directory):
    """返回目录中排序后的 PGM 文件；目录不存在时返回空列表。"""
    try:
        names = os.listdir(directory)
    except OSError as error:
        print("Template directory unavailable:", directory, error)
        return []

    files = []
    for name in names:
        if name.lower().endswith(".pgm"):
            files.append(name)

    files.sort()
    return files


def build_catalogue(group_name):
    """读取一个分组的模板并预先提取 ORB 描述子。"""
    directory = GROUP_CONFIG[group_name]["directory"]
    catalogue = []
    filenames = find_pgm_files(directory)

    print("Building catalogue:", group_name, "files=", len(filenames))

    for filename in filenames:
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
            descriptors = template.find_keypoints(
                threshold=TEMPLATE_KEYPOINT_THRESHOLD,
                normalized=True,
                scale_factor=KEYPOINT_SCALE_FACTOR,
                max_keypoints=TEMPLATE_MAX_KEYPOINTS,
                corner_detector=image.CORNER_AGAST,
            )

            if descriptors is None:
                print("  skipped without keypoints:", filename)
                continue

            label = label_from_filename(filename)

            # 大小王只有颜色区别，灰度 ORB 模板必须合并成同一个 joker 类。
            # 否则两张近似相同的灰度模板会互相占据第一名和第二名。
            if group_name == "special" and label.startswith("joker"):
                label = "joker"

            catalogue.append((label, filename, descriptors))
            print("  loaded %-22s -> %s" % (filename, label))
        except Exception as error:
            print("  skipped unreadable template:", filename, error)
        finally:
            if template is not None:
                del template
            gc.collect()

    print("  usable templates:", len(catalogue))
    return catalogue


def match_catalogue(group_name, scene_descriptors):
    """匹配一个分组，返回最佳、第二名及其几何信息。"""
    empty = {
        "group": group_name,
        "label": None,
        "count": 0,
        "second_count": 0,
        "margin": 0,
        "match": None,
        "filename": None,
        "accepted": False,
    }

    if scene_descriptors is None or not catalogues[group_name]:
        return empty

    # 同一标签允许多张模板，但同类只保留得分最高的一张。
    best_by_label = {}

    for label, filename, template_descriptors in catalogues[group_name]:
        match = image.match_descriptor(
            template_descriptors,
            scene_descriptors,
            threshold=DESCRIPTOR_THRESHOLD,
            filter_outliers=FILTER_OUTLIERS,
        )
        count = 0 if match is None else match.count

        if PRINT_ALL_MATCHES:
            print("match %-7s %-22s %d" % (group_name, filename, count))

        previous = best_by_label.get(label)
        if previous is None or count > previous[0]:
            best_by_label[label] = (count, match, filename)

    best_label = None
    best_count = 0
    second_count = 0
    best_match = None
    best_filename = None

    for label in best_by_label:
        count, match, filename = best_by_label[label]
        if count > best_count:
            second_count = best_count
            best_label = label
            best_count = count
            best_match = match
            best_filename = filename
        elif count > second_count:
            second_count = count

    margin = best_count - second_count
    config = GROUP_CONFIG[group_name]
    angle_ok = (
        best_match is not None
        and abs(best_match.theta) <= MAX_ABS_MATCH_ANGLE
    )
    accepted = (
        best_label is not None
        and angle_ok
        and best_count >= config["min_count"]
        and margin >= config["min_margin"]
    )

    return {
        "group": group_name,
        "label": best_label,
        "count": best_count,
        "second_count": second_count,
        "margin": margin,
        "match": best_match,
        "filename": best_filename,
        "accepted": accepted,
    }


def extract_scene_descriptors(gray, group_name):
    """只在相应搜索 ROI 中提取特征，减少背景干扰和运算量。"""
    return gray.find_keypoints(
        roi=GROUP_CONFIG[group_name]["roi"],
        threshold=SCENE_KEYPOINT_THRESHOLD,
        normalized=True,
        scale_factor=KEYPOINT_SCALE_FACTOR,
        max_keypoints=SCENE_MAX_KEYPOINTS,
        corner_detector=image.CORNER_AGAST,
    )


def decide_result(rank_result, suit_result, special_result, any_keypoints):
    """将三个分组结果合并为最终牌名或拒识状态。"""
    ordinary_ok = rank_result["accepted"] and suit_result["accepted"]
    special_ok = special_result["accepted"]

    # 特殊牌只有在普通牌不完整，或得分明显更强时才获得优先权。
    ordinary_peak = max(rank_result["count"], suit_result["count"])
    special_has_priority = (
        special_ok
        and (
            not ordinary_ok
            or special_result["count"]
            >= ordinary_peak + SPECIAL_PRIORITY_MARGIN
        )
    )

    if special_has_priority:
        return special_result["label"]

    if ordinary_ok:
        return "%s_%s" % (suit_result["label"], rank_result["label"])

    if not any_keypoints:
        return "NO_CARD"

    return "UNKNOWN"


def classify_joker_color(color_image):
    """在 RGB565 原图中统计红色区域，返回大小王标签和红色像素数量。"""
    red_blobs = color_image.find_blobs(
        [JOKER_RED_THRESHOLD],
        roi=JOKER_COLOR_ROI,
        pixels_threshold=5,
        area_threshold=5,
        merge=True,
        margin=2,
    )

    red_pixels = 0
    for blob in red_blobs:
        red_pixels += blob.pixels

    if red_pixels >= JOKER_RED_MIN_PIXELS:
        return RED_JOKER_LABEL, red_pixels

    return BLACK_JOKER_LABEL, red_pixels


def result_summary(result):
    """生成紧凑的串口调试字符串。"""
    label = result["label"] if result["label"] is not None else "-"
    angle = 0 if result["match"] is None else result["match"].theta
    return "%s:%s n=%d m=%d a=%d ok=%d" % (
        result["group"],
        label,
        result["count"],
        result["margin"],
        angle,
        1 if result["accepted"] else 0,
    )


def draw_result(img, result):
    """绘制该分组搜索 ROI 和最佳匹配区域。"""
    config = GROUP_CONFIG[result["group"]]
    roi_color = config["color"]
    img.draw_rectangle(config["roi"], color=roi_color, thickness=1)

    if result["match"] is not None:
        match_color = roi_color if result["accepted"] else (128, 128, 128)
        img.draw_rectangle(result["match"].rect, color=match_color, thickness=2)
        img.draw_cross(
            (result["match"].cx, result["match"].cy),
            color=match_color,
        )


def keep_white_light_on():
    """RGB 三通道始终点亮，识别状态只通过 IDE 图像和串口输出。"""
    red_led.on()
    green_led.on()
    blue_led.on()


# ============================== 初始化 ========================================

red_led = LED("LED_RED")
green_led = LED("LED_GREEN")
blue_led = LED("LED_BLUE")
ir_led = LED("LED_IR")
#ir_led.high()

# 必须在相机自动参数稳定前打开白光，采集与识别使用相同照明。
keep_white_light_on()

catalogues = {
    "rank": build_catalogue("rank"),
    "suit": build_catalogue("suit"),
    "special": build_catalogue("special"),
}

if not catalogues["rank"] and not catalogues["suit"] and not catalogues["special"]:
    raise OSError("No usable templates found under " + TEMPLATE_ROOT)

print("Catalogue summary:")
for group_name in ("rank", "suit", "special"):
    print("  %s=%d" % (group_name, len(catalogues[group_name])))

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


# ============================== 识别主循环 ====================================

last_recognition_ms = time.ticks_ms() - RECOGNITION_INTERVAL_MS
pending_label = None
pending_count = 0
reported_label = None
display_label = "waiting"

last_results = {
    "rank": match_catalogue("rank", None),
    "suit": match_catalogue("suit", None),
    "special": match_catalogue("special", None),
}

while True:
    img = cam.snapshot()
    now_ms = time.ticks_ms()

    if time.ticks_diff(now_ms, last_recognition_ms) >= RECOGNITION_INTERVAL_MS:
        last_recognition_ms = now_ms
        gray = None
        descriptors = {"rank": None, "suit": None, "special": None}

        try:
            gray = img.to_grayscale(copy=True)

            for group_name in ("rank", "suit", "special"):
                if catalogues[group_name]:
                    descriptors[group_name] = extract_scene_descriptors(
                        gray, group_name
                    )
                last_results[group_name] = match_catalogue(
                    group_name, descriptors[group_name]
                )

            any_keypoints = (
                descriptors["rank"] is not None
                or descriptors["suit"] is not None
                or descriptors["special"] is not None
            )
            current_label = decide_result(
                last_results["rank"],
                last_results["suit"],
                last_results["special"],
                any_keypoints,
            )

            joker_red_pixels = -1
            if current_label == "joker":
                current_label, joker_red_pixels = classify_joker_color(img)

            if PRINT_ROUND_SUMMARY:
                print("ROUND:", current_label)
                print(" ", result_summary(last_results["rank"]))
                print(" ", result_summary(last_results["suit"]))
                print(" ", result_summary(last_results["special"]))
                if joker_red_pixels >= 0:
                    print(
                        "  joker_color: red_pixels=%d threshold=%d"
                        % (joker_red_pixels, JOKER_RED_MIN_PIXELS)
                    )

            if current_label == pending_label:
                pending_count += 1
            else:
                pending_label = current_label
                pending_count = 1

            display_label = current_label
            keep_white_light_on()

            if pending_count >= STABLE_ROUNDS and reported_label != current_label:
                reported_label = current_label
                print("RESULT:", current_label, "stable=", pending_count)
        except Exception as error:
            display_label = "ERROR"
            keep_white_light_on()
            print("RECOGNITION_ERROR:", error)
        finally:
            if gray is not None:
                del gray
            for group_name in descriptors:
                if descriptors[group_name] is not None:
                    descriptors[group_name] = None
            gc.collect()

    for group_name in ("rank", "suit", "special"):
        draw_result(img, last_results[group_name])

    label_color = (
        (0, 255, 0)
        if display_label not in ("UNKNOWN", "NO_CARD", "ERROR", "waiting")
        else (255, 0, 0)
    )
    img.draw_string((4, 4), display_label, color=label_color)
    img.draw_string(
        (4, 18),
        "stable=%d mem=%d" % (pending_count, gc.mem_free()),
        color=label_color,
    )
