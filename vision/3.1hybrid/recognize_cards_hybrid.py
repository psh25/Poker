"""OpenMV H7 Plus periodically triggered card recognition.

Deploy with cards_hybrid_core.py, cards_fast_config.py and a matching hybrid bank.
After startup, one recognition starts automatically every three seconds:
fresh capture -> native pixel difference -> same-frame refinement. Results and
diagnostics are printed through the OpenMV IDE USB terminal.
UNKNOWN means insufficient evidence, NEVER "no card".
The 900 ms deadline is cooperative: an individual native call cannot be
interrupted. Measure TIME_MS on the actual H7 Plus before acceptance.
"""
import gc
import os
import time
import cards_fast_config as C
import cards_hybrid_core as V

# ---------------- Runtime-only settings; tune without editing config/core ----------------
# Wider than capture ROIs so the complete symbols remain visible throughout
# the measured mechanical displacement range.
RECOGNITION_ROIS = {
    "rank": (120, 25, 100, 155),
    "suit": (120, 130, 100, 110),
    "joker": (120, 25, 80, 150),
    "back": (185, 120, 40, 40),
}

COARSE_ANGLES = (0,)
REFINE_ANGLES = (-5, 5)
REFINE_OTSU_OFFSETS = (-8, 8)
BACK_REFINE_OFFSETS = ((0, 0), (-4, 0), (4, 0), (0, -4), (0, 4),
                       (-4, -4), (-4, 4), (4, -4), (4, 4))

RECOGNITION_VISION = {
    "rois": RECOGNITION_ROIS,
    "red_threshold": C.RED_THRESHOLD,
    "black_threshold": (0, 45, -30, 20, -25, 35),
    "color_red_threshold": (10, 95, 15, 127, -35, 127),
    "color_black_threshold": C.BLACK_THRESHOLD,
    "color_red_min_pixels": 20,
    "color_black_min_pixels": 20,
    "min_contrast": 24,
    "max_candidates": 8,
    "ten_min_row_overlap": 0.70,
    "ten_max_gap_height": 0.35,
    "ten_min_height_ratio": 0.72,
    "joker_column_dx": 22,
    "pair_max_dx": 28,
    "pair_min_dy": 35,
    "pair_max_dy": 150,
    "pair_target_dy": 95,
    "max_layout_pairs": 2,
    "refine_angles": REFINE_ANGLES,
    "back_refine_offsets": BACK_REFINE_OFFSETS,
    "accept_score": {"rank": 0.80, "suit": 0.80, "joker": 0.84, "back": 0.86},
    # Current screenshot validation separates the closest wrong suit (0.022)
    # from the weakest correct suit (0.028). Recalibrate from raw trigger
    # frames after accumulating a larger hardware validation set.
    "accept_margin": {"rank": 0.05, "suit": 0.015, "joker": 0.0, "back": 0.0},
    "scene_margin": 0.035,
    "red_joker_label": "joker_big",
    "black_joker_label": "joker_small",
}

STRICT_TEMPLATE_COVERAGE = True
MAX_TEMPLATES_PER_LABEL = 2
REASON_MAX_CHARS = 240
INFO_MAX_CHARS = 160
RESULT_BUDGET_MS = 900
OUTPUT_RESERVE_MS = 15
AUTO_TRIGGER_INTERVAL_MS = 3000
MAX_ATTEMPTS = 1  # Current requirement: no recapture; failure emits UNKNOWN.
RETRY_SETTLE_MS = 20
DISCARD_AFTER_TRIGGER = 1
PRINT_TIMING = True
DEBUG_DRAW_TRIGGER_FRAME = True  # Draw only after USB result/timing completes.

# 可选的真值样本采集。默认关闭，避免将大量 BMP 写入内部 Flash；
# 开启前请确认 Flash 剩余空间，识别所用原始帧会在 IDE 绘制矩形前保存。
SAVE_TEST_SAMPLES = False
SAMPLE_USE_SD_CARD = False
SAMPLE_SESSION = "light01"
SAMPLE_LABEL = "10_club"  # e.g. 10_club, spade, joker_red, back
SAMPLE_POSITION = "center"  # center, left, right, up, down, angle_left...
SAMPLE_MIN_FREE_BYTES = 2 * 1024 * 1024
DEBUG_ROI_COLORS = {
    "rank": (255, 0, 0),
    "suit": (0, 0, 255),
    "joker": (255, 0, 255),
    "back": (0, 255, 255),
}
DEBUG_CANDIDATE_COLOR = (255, 255, 0)
DEBUG_ACCEPTED_COLOR = (0, 255, 0)
# --------------------------------------------------------------------------------


def _group_failure(group, info):
    """Return one compact, machine-readable rejection item."""
    label = info.get("label")
    if label is None:
        return group.upper() + "_NO_MATCH"
    score = info.get("score", -1.0)
    score_limit = RECOGNITION_VISION["accept_score"][group]
    if score < score_limit:
        return "%s_SCORE=%s,%.3f<%.3f" % (
            group.upper(), label, score, score_limit)
    margin = info.get("margin", 0.0)
    margin_limit = RECOGNITION_VISION["accept_margin"][group]
    if margin < margin_limit:
        return "%s_MARGIN=%s,%.3f<%.3f" % (
            group.upper(), label, margin, margin_limit)
    return None


def unknown_detail(result):
    """Explain why no class passed without dumping a large Python dict."""
    reason = result.get("reason", "UNKNOWN")
    if reason in ("TIMEOUT", "RETRY_BUDGET_EXHAUSTED", "NO_RESULT"):
        return reason

    groups = result.get("groups", {})
    diagnostics = result.get("diagnostics", {})
    parts = []
    layout_pairs = diagnostics.get("layout_pairs")
    if (layout_pairs == 0
            and (groups.get("rank", {}).get("label") is None
                 or groups.get("suit", {}).get("label") is None)):
        rank_count = diagnostics.get("rank", {}).get("valid_candidates", 0)
        suit_count = diagnostics.get("suit", {}).get("valid_candidates", 0)
        parts.append("NO_LAYOUT=r%d,s%d" % (rank_count, suit_count))

    for group in ("rank", "suit", "joker", "back"):
        info = groups.get(group, {})
        item = _group_failure(group, info)
        if item is not None:
            if info.get("label") is None and group in diagnostics:
                locate_reason = diagnostics[group].get("reason")
                if locate_reason and locate_reason != "OK":
                    item += ":" + locate_reason
            parts.append(item)

    rank, suit = groups.get("rank", {}), groups.get("suit", {})
    if (rank.get("accepted") and suit.get("accepted")
            and rank.get("box") is not None and suit.get("box") is not None):
        if not V.geometry_ok(rank["box"], suit["box"], RECOGNITION_VISION):
            parts.append("NORMAL_GEOMETRY")
        else:
            evidence = suit.get("color", {}).get("color")
            expected = ("red" if suit.get("label") in ("heart", "diamond")
                        else "black")
            if evidence is not None and evidence != expected:
                parts.append("SUIT_COLOR=%s!=%s" % (evidence, expected))

    joker = groups.get("joker", {})
    if joker.get("accepted") and joker.get("color", {}).get("color") is None:
        parts.append("JOKER_COLOR_NONE")
    if reason == "SCENE_CONFLICT":
        parts.insert(0, "SCENE_CONFLICT")
    if not parts:
        parts.append(reason)
    return ";".join(parts)[:REASON_MAX_CHARS]


def result_info(result):
    """Compact group scores for USB; no diagnostic text is drawn on images."""
    parts = []
    codes = {"rank": "R", "suit": "S", "joker": "J", "back": "B"}
    for group in ("rank", "suit", "joker", "back"):
        info = result.get("groups", {}).get(group, {})
        label = info.get("label")
        if label is None:
            continue
        parts.append("%s:%s,%.3f,%.3f,%d" % (
            codes[group], label, info.get("score", -1.0),
            info.get("margin", 0.0), 1 if info.get("accepted") else 0))
    return ";".join(parts)[:INFO_MAX_CHARS]


def print_result(result):
    label = result.get("label", "UNKNOWN")
    print("RESULT:" + label)
    if label == "UNKNOWN":
        print("REASON:" + unknown_detail(result))
    info = result_info(result)
    if info:
        print("INFO:" + info)


def _safe_sample_token(value):
    """Keep user-edited labels safe for FAT paths and CSV filenames."""
    out = ""
    for char in str(value):
        if (("a" <= char <= "z") or ("A" <= char <= "Z")
                or ("0" <= char <= "9") or char in ("_", "-")):
            out += char
        else:
            out += "_"
    return out or "unknown"


def _free_bytes(path):
    values = os.statvfs(path)
    block_size = values[1] if values[1] else values[0]
    return int(block_size) * int(values[3])


def prepare_sample_storage():
    if not SAVE_TEST_SAMPLES:
        return None
    if SAMPLE_USE_SD_CARD:
        volume = "/sdcard"
        if not V.exists(volume):
            raise OSError("Sample mode needs an SD card mounted at /sdcard")
    else:
        volume = C.ROOT
    base = volume + "/card_samples"
    V.mkdir(base)
    directory = base + "/" + _safe_sample_token(SAMPLE_SESSION)
    V.mkdir(directory)
    if _free_bytes(volume) < SAMPLE_MIN_FREE_BYTES:
        raise OSError("Insufficient sample storage")

    next_index = 1
    for name in os.listdir(directory):
        if len(name) < 5 or name[4] != "_":
            continue
        try:
            index = int(name[:4])
            if index >= next_index:
                next_index = index + 1
        except ValueError:
            pass

    csv_path = directory + "/samples.csv"
    if not V.exists(csv_path):
        with open(csv_path, "w") as stream:
            stream.write(
                "index,filename,ground_truth,position,result,reason,info,"
                "time_ms,exposure_us,gain_db,rgb_gain_db,calibration_id,"
                "rank_box,suit_box,joker_box,back_box\n")
        os.sync()
    print("SAMPLE_READY:%s label=%s position=%s" % (
        directory, SAMPLE_LABEL, SAMPLE_POSITION))
    return {"directory": directory, "csv": csv_path,
            "volume": volume, "next": next_index}


def _csv_cell(value):
    text = str(value).replace("\r", " ").replace("\n", " ")
    return '"' + text.replace('"', '""') + '"'


def _result_box(result, group):
    return result.get("groups", {}).get(group, {}).get("box", "")


def save_test_sample(frame, result, profile, camera, sample_state):
    """Save one raw trigger frame plus compact, corresponding metadata."""
    if sample_state is None or frame is None:
        return
    if _free_bytes(sample_state["volume"]) < SAMPLE_MIN_FREE_BYTES:
        raise OSError("Sample storage is nearly full")

    index = sample_state["next"]
    sample_state["next"] = index + 1
    truth = _safe_sample_token(SAMPLE_LABEL)
    position = _safe_sample_token(SAMPLE_POSITION)
    name = "%04d_%s_%s.bmp" % (index, truth, position)
    image_path = sample_state["directory"] + "/" + name
    save_start = time.ticks_ms()

    # Recognition only reads/copies from frame. Saving here therefore records
    # the exact unannotated pixels used by the just-completed decision.
    frame.save(image_path)
    label = result.get("label", "UNKNOWN")
    reason = (unknown_detail(result) if label == "UNKNOWN"
              else result.get("reason", "OK"))
    values = (
        index, name, SAMPLE_LABEL, SAMPLE_POSITION, label, reason,
        result_info(result), profile.get("total_ms", -1),
        camera.get("exposure_us", ""), camera.get("gain_db", ""),
        camera.get("rgb_gain_db", ""), camera.get("calibration_id", ""),
        _result_box(result, "rank"), _result_box(result, "suit"),
        _result_box(result, "joker"), _result_box(result, "back"))
    with open(sample_state["csv"], "a") as stream:
        stream.write(",".join(_csv_cell(value) for value in values) + "\n")
    os.sync()
    print("SAMPLE:%s SAVE_MS:%d" % (
        name, time.ticks_diff(time.ticks_ms(), save_start)))


def draw_trigger_debug(frame, result, sequence, profile):
    """Draw geometry only; all text diagnostics are printed through USB."""
    if not DEBUG_DRAW_TRIGGER_FRAME or frame is None:
        return

    # Search windows are drawn after recognition, so these pixels never enter
    # LAB location, local Otsu, template scoring, or the result timing measurement.
    for group in ("rank", "suit", "joker", "back"):
        frame.draw_rectangle(RECOGNITION_ROIS[group],
                             color=DEBUG_ROI_COLORS[group])

    groups = result.get("groups", {})
    for group in ("rank", "suit", "joker", "back"):
        info = groups.get(group)
        if not info or "box" not in info:
            continue
        # A rejected back box is identical to its fixed search ROI and would
        # hide the cyan outline without adding useful information.
        if group == "back" and not info.get("accepted", False):
            continue
        color = (DEBUG_ACCEPTED_COLOR if info.get("accepted", False)
                 else DEBUG_CANDIDATE_COLOR)
        box = info["box"]
        frame.draw_rectangle(box, color=color)
    # 识别和计时完成后再刷新IDE调试帧，避免画框污染本次判决。
    if hasattr(frame, "flush"):
        frame.flush()


def recognize_trigger(cam, bank, trigger_ms):
    budget = V.Budget(trigger_ms, RESULT_BUDGET_MS - OUTPUT_RESERVE_MS)
    result = {"label": "UNKNOWN", "reason": "NO_RESULT", "groups": {}}
    profile = {"attempts": 0, "capture_ms": 0, "passes": []}
    frame = None
    try:
        for attempt in range(MAX_ATTEMPTS):
            budget.check()
            start = time.ticks_ms()
            if attempt == 0:
                for unused in range(DISCARD_AFTER_TRIGGER):
                    cam.snapshot()
                    budget.check()
            frame = cam.snapshot()  # 自动周期到达后采集新帧，不复用旧图。
            budget.check()
            profile["capture_ms"] += time.ticks_diff(time.ticks_ms(), start)
            profile["attempts"] += 1
            records = {k: {} for k in ("rank", "suit", "joker", "back")}
            result = V.process_pass(frame, bank, records, COARSE_ANGLES,
                                    RECOGNITION_VISION, budget=budget)
            coarse_ms = result["pass_ms"]
            profile["passes"].append((attempt + 1, "coarse", result["locate_ms"], result["pass_ms"]))
            if result["label"] != "UNKNOWN":
                break
            # Refine rotation on SAME frame. All labels remain eligible so an
            # initially missed competitor does not produce an artificial margin.
            if REFINE_ANGLES and budget.remaining() > max(80, coarse_ms * 2):
                result = V.process_pass(frame, bank, records, REFINE_ANGLES,
                                        RECOGNITION_VISION, budget=budget)
                profile["passes"].append((attempt + 1, "angle", result["locate_ms"], result["pass_ms"]))
            if result["label"] != "UNKNOWN":
                break
            for offset in REFINE_OTSU_OFFSETS:
                if budget.remaining() <= max(100, coarse_ms * 2):
                    break
                # Re-evaluate all coarse angles after local-Otsu adjustment.
                result = V.process_pass(frame, bank, records, COARSE_ANGLES,
                                        RECOGNITION_VISION, offset, budget)
                profile["passes"].append((attempt + 1, "otsu", result["locate_ms"], result["pass_ms"]))
                if result["label"] != "UNKNOWN":
                    break
            if result["label"] != "UNKNOWN":
                break
            if attempt + 1 >= MAX_ATTEMPTS:
                break
            # Admit a retry only if observed capture+coarse cost fits; this is
            # an estimate, with deadline checks around each subsequent stage.
            needed = max(120, profile["capture_ms"] // profile["attempts"] + coarse_ms + RETRY_SETTLE_MS + 30)
            if budget.remaining() <= needed:
                result["reason"] = "RETRY_BUDGET_EXHAUSTED"
                break
            frame = None
            time.sleep_ms(RETRY_SETTLE_MS)
        budget.check()
    except V.BudgetExceeded:
        result = {"label": "UNKNOWN", "reason": "TIMEOUT", "groups": {}}
    profile["processing_ms"] = time.ticks_diff(time.ticks_ms(), trigger_ms)
    return result, profile, frame


def wait_for_auto_trigger(last_trigger_ms):
    """等待到下一个三秒周期，并返回本次识别的开始时刻。"""
    while True:
        now = time.ticks_ms()
        remaining = AUTO_TRIGGER_INTERVAL_MS - time.ticks_diff(now, last_trigger_ms)
        if remaining <= 0:
            return now
        # 短间隔休眠便于在IDE中停止脚本，同时避免空转占用CPU。
        time.sleep_ms(min(50, remaining))


def main():
    V.validate_config(RECOGNITION_VISION)
    V.ensure_storage()
    sample_state = prepare_sample_storage()
    cam, camera, leds = V.start_camera(False)
    bank = V.load_bank(STRICT_TEMPLATE_COVERAGE, MAX_TEMPLATES_PER_LABEL)
    print("READY: automatic recognition every %d ms" % AUTO_TRIGGER_INTERVAL_MS)
    print("Camera:", camera)
    print("Difference-score thresholds are provisional; check TIME_MS on hardware.")
    print("IDE trigger-frame overlay:", DEBUG_DRAW_TRIGGER_FRAME)
    sequence = 0
    last_trigger_ms = time.ticks_ms()
    gc.collect()
    while True:
        # 周期按相邻两次识别的开始时刻计算；正常识别耗时不会额外拉长3秒间隔。
        trigger_ms = wait_for_auto_trigger(last_trigger_ms)
        last_trigger_ms = trigger_ms
        sequence += 1
        debug_frame = None
        try:
            result, profile, debug_frame = recognize_trigger(cam, bank, trigger_ms)
        except Exception as error:
            result = {"label": "ERROR", "reason": repr(error), "groups": {}}
            profile = {}
        # 保留少量时间给主要的USB结果行；超时结果不能当作识别成功。
        if time.ticks_diff(time.ticks_ms(), trigger_ms) >= RESULT_BUDGET_MS - OUTPUT_RESERVE_MS:
            result["label"], result["reason"] = "UNKNOWN", "TIMEOUT"
        usb_start = time.ticks_ms()
        print_result(result)
        profile["usb_result_ms"] = time.ticks_diff(time.ticks_ms(), usb_start)
        profile["total_ms"] = time.ticks_diff(time.ticks_ms(), trigger_ms)
        profile["over_budget"] = profile["total_ms"] > RESULT_BUDGET_MS
        if PRINT_TIMING:
            print("TIME_MS:%d" % profile["total_ms"])
        try:
            save_test_sample(debug_frame, result, profile,
                             camera, sample_state)
        except Exception as error:
            print("SAMPLE_ERROR:" + repr(error))
        draw_trigger_debug(debug_frame, result, sequence, profile)
        gc.collect()  # 两次自动识别之间清理内存，不在模板比较时执行。


if __name__ == "__main__":
    main()
