"""Hybrid LAB locator + local-Otsu template pipeline for OpenMV H7 Plus.

Only connected-component metadata is processed in Python. Pixel operations,
rotation, resizing, histogram, and image-difference statistics are OpenMV
native operations.
"""
import gc
import json
import math
import os
import time
import image
import cards_fast_config as C


# These values define template pixels and therefore must be identical during
# capture and recognition. Locator ROIs/LAB thresholds intentionally stay in
# the two entry scripts and may differ.
CANONICAL_PADDING = 6
CANONICAL_THRESHOLD_OFFSET = 0


class BudgetExceeded(Exception):
    pass


class Budget:
    def __init__(self, start_ms, duration_ms):
        self.start = start_ms
        self.duration = duration_ms

    def remaining(self):
        return self.duration - time.ticks_diff(time.ticks_ms(), self.start)

    def check(self, reserve=0):
        if self.remaining() <= reserve:
            raise BudgetExceeded("processing deadline")


def check(budget):
    if budget is not None:
        budget.check()


def attr(obj, name):
    """5.x attrtuples; also accepts older method-style image results."""
    value = getattr(obj, name)
    return value() if callable(value) else value


def exists(path):
    try:
        os.stat(path)
        return True
    except OSError as error:
        if error.args and error.args[0] == 2:
            return False
        raise


def mkdir(path):
    if not exists(path):
        os.mkdir(path)


def storage_free_bytes():
    """Return conservative free bytes for the configured template volume."""
    values = os.statvfs(C.ROOT)
    block_size = values[1] if values[1] else values[0]
    return int(block_size) * int(values[3])


def minimum_storage_free_bytes():
    return C.MIN_SD_FREE_BYTES if C.USE_SD_CARD else C.MIN_FLASH_FREE_BYTES


def ensure_storage():
    if C.USE_SD_CARD:
        if not C.ROOT.startswith("/sdcard/"):
            raise ValueError("USE_SD_CARD is True but ROOT is not under /sdcard")
        if not exists("/sdcard"):
            raise OSError("SD card is not mounted at /sdcard")
    mkdir(C.ROOT)
    mkdir(C.ROOT + "/templates")
    for group in ("rank", "suit", "joker", "back"):
        mkdir(C.ROOT + "/templates/" + group)
    free_bytes = storage_free_bytes()
    if free_bytes < minimum_storage_free_bytes():
        raise OSError("Insufficient template storage: %d bytes free" % free_bytes)
    return free_bytes


def read_json(path):
    with open(path, "r") as stream:
        return json.load(stream)


def write_json(path, data):
    with open(path, "w") as stream:
        json.dump(data, stream)


def save_template_atomic(patch, base, metadata):
    """Commit JSON first and PGM last; loader sees only complete templates."""
    if storage_free_bytes() < minimum_storage_free_bytes():
        raise OSError("Insufficient template storage before write")
    final_pgm = base + ".pgm"
    final_json = base + ".json"
    temp_pgm = base + ".tmp.pgm"
    temp_json = base + ".tmp.json"
    if exists(final_pgm) or exists(final_json) or exists(temp_pgm) or exists(temp_json):
        raise OSError("Template path already exists: " + base)
    patch.save(temp_pgm)
    write_json(temp_json, metadata)
    os.sync()
    # An orphan JSON is harmless because load_bank enumerates committed PGM
    # files. Renaming the PGM last acts as the commit marker.
    os.rename(temp_json, final_json)
    os.rename(temp_pgm, final_pgm)
    os.sync()
    return final_pgm


def signature():
    # Locator ROIs and thresholds may differ. Canonical template production
    # must remain identical between capture and recognition.
    return repr(("cards_hybrid", 1, C.FRAME_SIZE, "tight_local_otsu",
                 tuple((k, C.PATCH_SIZES[k])
                       for k in ("rank", "suit", "joker", "back")),
                 CANONICAL_PADDING, CANONICAL_THRESHOLD_OFFSET,
                 C.PATCH_PADDING, C.MIN_COMPONENT_PIXELS, C.MAX_COMPONENTS,
                 tuple((k, C.MIN_BOX[k], C.MAX_BOX[k]) for k in ("rank", "suit", "joker")),
                 C.MIN_INK_FRACTION, C.MAX_INK_FRACTION, C.BORDER_GUARD,
                 C.ROW_OVERLAP, C.ROW_GAP_HEIGHT))


def group_setting(settings, name, group):
    value = settings[name]
    return value[group] if isinstance(value, dict) else value


def validate_config(settings=None):
    rois = settings["rois"] if settings is not None else {}
    if settings is not None:
        missing = [g for g in ("rank", "suit", "joker", "back") if g not in rois]
        if missing:
            raise ValueError("Missing search ROI: " + repr(missing))
    for group, roi in rois.items():
        x, y, w, h = roi
        if min(x, y) < 0 or min(w, h) <= 0 or x + w > 320 or y + h > 240:
            raise ValueError("Invalid QVGA search ROI: " + group)
        if group not in C.PATCH_SIZES:
            raise ValueError("Unknown ROI group: " + group)
        if min(C.PATCH_SIZES[group]) <= 2 * C.PATCH_PADDING:
            raise ValueError("Patch too small: " + group)
    if settings is not None:
        for group in ("rank", "suit", "joker"):
            if group not in rois:
                continue
            if group not in C.MIN_BOX or group not in C.MAX_BOX:
                raise ValueError("Missing box limits: " + group)
        if int(settings["max_candidates"]) < 1:
            raise ValueError("max_candidates must be positive")


def start_camera(calibrate=False, enable_leds=True):
    """Shared persisted exposure/gain/WB. Recognition never auto-calibrates."""
    import csi
    from machine import LED
    leds = [LED("LED_RED"), LED("LED_GREEN"), LED("LED_BLUE")]
    if enable_leds:
        for led in leds:
            led.on()
    path = C.CAMERA_CONFIG_PATH
    if not calibrate and not exists(path):
        raise ValueError("No camera.json: calibrate with capture_cards_fast.py first")
    cam = csi.CSI()
    cam.reset()
    cam.pixformat(csi.RGB565)
    cam.framesize(csi.QVGA)
    cam.framebuffers(1)

    # Convert the physical camera mounting orientation
    # into the canonical image orientation used by the recognition pipeline.
    cam.hmirror(C.CAMERA_ROTATE_180)
    cam.vflip(C.CAMERA_ROTATE_180)

    if calibrate:
        cam.snapshot(time=C.CAMERA_SETTLE_MS)
        settings = {"exposure_us": cam.exposure_us(), "gain_db": cam.gain_db(),
                    "rgb_gain_db": list(cam.rgb_gain_db()),
                    "calibration_id": time.ticks_ms()}
    else:
        settings = read_json(path)
    cam.auto_exposure(False, exposure_us=int(settings["exposure_us"]))
    cam.auto_gain(False, gain_db=settings["gain_db"])
    cam.auto_whitebal(False, rgb_gain_db=tuple(settings["rgb_gain_db"]))
    cam.snapshot(time=C.CAMERA_APPLY_SETTLE_MS)
    if calibrate:
        write_json(path, settings)
        os.sync()
    # Force API availability checks before collecting a template bank.
    probe = image.Image(16, 16, image.GRAYSCALE)
    # The firmware SSIM implementation can return values outside [-1, 1] for
    # these patch heights.  Use the native absolute-difference statistic
    # instead and verify that API here before loading a template bank.
    probe.get_statistics(difference=probe)
    probe.rotation_corr(z_rotation=0)
    return cam, settings, leds


def union_box(a, b):
    x, y = min(a[0], b[0]), min(a[1], b[1])
    return (x, y, max(a[0] + a[2], b[0] + b[2]) - x,
            max(a[1] + a[3], b[1] + b[3]) - y)


def components_join(a, b):
    overlap = min(a[1] + a[3], b[1] + b[3]) - max(a[1], b[1])
    gap = max(a[0], b[0]) - min(a[0] + a[2], b[0] + b[2])
    return (overlap >= C.ROW_OVERLAP * min(a[3], b[3])
            and gap <= C.ROW_GAP_HEIGHT * max(a[3], b[3]))


def group_components(items, group, settings):
    """Keep singles; add only strict 10 pairs or a vertical JOKER column."""
    if group == "joker":
        if not items:
            return []
        best = None
        max_dx = settings.get("joker_column_dx", 22)
        for seed, unused in items:
            center = seed[0] + seed[2] * 0.5
            chosen = [(box, pixels) for box, pixels in items
                      if abs((box[0] + box[2] * 0.5) - center) <= max_dx]
            if len(chosen) < 2:
                continue
            box, pixels = chosen[0]
            for other, count in chosen[1:]:
                box, pixels = union_box(box, other), pixels + count
            if best is None or box[3] > best[0][3]:
                best = (box, pixels)
        return [best] if best is not None else []
    if group != "rank":
        return list(items)

    out = list(items)
    min_overlap = settings.get("ten_min_row_overlap", 0.70)
    max_gap_ratio = settings.get("ten_max_gap_height", 0.35)
    min_height_ratio = settings.get("ten_min_height_ratio", 0.72)
    for i in range(len(items)):
        a, ap = items[i]
        for j in range(i + 1, len(items)):
            b, bp = items[j]
            left, right = (a, b) if a[0] <= b[0] else (b, a)
            overlap = min(left[1] + left[3], right[1] + right[3]) - max(left[1], right[1])
            gap = right[0] - (left[0] + left[2])
            height_ratio = min(left[3], right[3]) / float(max(left[3], right[3]))
            if (overlap >= min_overlap * min(left[3], right[3])
                    and 0 <= gap <= max_gap_ratio * max(left[3], right[3])
                    and height_ratio >= min_height_ratio):
                out.append((union_box(left, right), ap + bp))
    return out


def make_mask(color_img, group, settings, unused_threshold_delta=0):
    """Extract filled red/black printing; reject bright cyan glare by LAB."""
    roi = settings["rois"][group]
    gray = color_img.copy(roi=roi).to_grayscale()
    hist = gray.get_histogram()
    low = attr(hist.get_percentile(0.05), "value")
    high = attr(hist.get_percentile(0.95), "value")
    contrast = high - low
    min_contrast = int(group_setting(settings, "min_contrast", group))
    if contrast < min_contrast:
        return None, {"reason": "LOW_CONTRAST", "contrast": high - low}
    del gray
    mask = color_img.copy(roi=roi)
    mask.binary([settings["red_threshold"], settings["black_threshold"]])
    mask.to_grayscale()
    return mask, {"method": "lab_ink", "contrast": contrast}


def locate(color_img, group, settings, threshold_offset_delta=0, budget=None):
    check(budget)
    mask, info = make_mask(color_img, group, settings, threshold_offset_delta)
    if mask is None:
        return [], info
    check(budget)
    blobs = mask.find_blobs([(128, 255)], x_stride=1, y_stride=1,
                           pixels_threshold=C.MIN_COMPONENT_PIXELS,
                           area_threshold=C.MIN_COMPONENT_PIXELS, merge=False)
    if len(blobs) > C.MAX_COMPONENTS:
        return [], {"reason": "TOO_MANY_COMPONENTS"}
    items = [(tuple(attr(b, "rect")), int(attr(b, "pixels"))) for b in blobs]
    info["raw_components"] = len(items)
    if group == "joker":
        # The visible word may continue outside the chosen partial-word ROI.
        # Do not let edge-connected artwork join otherwise clean J/O/K glyphs.
        rw, rh = mask.width(), mask.height()
        g = C.BORDER_GUARD
        items = [(box, pixels) for box, pixels in items
                 if box[0] > g and box[1] > g
                 and box[0] + box[2] < rw - g
                 and box[1] + box[3] < rh - g]
    items = group_components(items, group, settings)
    info["grouped_components"] = len(items)
    rx, ry, rw, rh = settings["rois"][group]
    candidates = []
    rejected = {"border": 0, "size": 0, "density": 0}
    for box, pixels in items:
        x, y, w, h = box
        minw, minh = C.MIN_BOX[group]
        maxw, maxh = C.MAX_BOX[group]
        g = C.BORDER_GUARD
        if x <= g or y <= g or x + w >= rw - g or y + h >= rh - g:
            rejected["border"] += 1
            continue
        if not (minw <= w <= maxw and minh <= h <= maxh):
            rejected["size"] += 1
            continue
        density = pixels / float(w * h)
        if not (C.MIN_INK_FRACTION <= density <= C.MAX_INK_FRACTION):
            rejected["density"] += 1
            continue
        # Keep metadata only. Canonicalization later crops the selected boxes
        # from the original frame, avoiding one image allocation per candidate.
        candidates.append({"box": (rx + x, ry + y, w, h),
                           "pixels": pixels, "density": density})
    candidates.sort(key=lambda c: c["pixels"], reverse=True)
    info["valid_candidates"] = len(candidates)
    info["rejected"] = rejected
    info["reason"] = "OK" if candidates else "NO_COMPLETE_SYMBOL"
    return candidates[:int(settings["max_candidates"])], info


def normalize(mask, group, angle=0):
    """Rotate with padding, re-tighten ALL strokes, preserve aspect ratio."""
    w, h = mask.width(), mask.height()
    pad = int(math.ceil(max(w, h) * 0.40)) + 3
    work = image.Image(w + 2 * pad, h + 2 * pad, image.GRAYSCALE)
    work.draw_image(mask, pad, pad)
    if angle:
        work.rotation_corr(z_rotation=angle)
    blobs = work.find_blobs([(128, 255)], x_stride=1, y_stride=1,
                           pixels_threshold=2, area_threshold=2, merge=False)
    if not blobs:
        return None
    box = tuple(attr(blobs[0], "rect"))
    for blob in blobs[1:]:
        box = union_box(box, tuple(attr(blob, "rect")))
    tw, th = C.PATCH_SIZES[group]
    margin = C.PATCH_PADDING
    scale = min((tw - 2 * margin) / float(box[2]), (th - 2 * margin) / float(box[3]))
    dw, dh = max(1, int(box[2] * scale)), max(1, int(box[3] * scale))
    target = image.Image(tw, th, image.GRAYSCALE)
    target.draw_image(work, (tw - dw) // 2, (th - dh) // 2,
                      roi=box, x_scale=scale, y_scale=scale, hint=image.AREA)
    return target


def canonical_patch(color_img, box, group, settings, angle=0,
                    threshold_offset_delta=0):
    """Make the representation shared by capture and recognition."""
    pad = CANONICAL_PADDING
    x, y, w, h = box
    x0, y0 = max(0, x - pad), max(0, y - pad)
    x1, y1 = min(C.FRAME_SIZE[0], x + w + pad), min(C.FRAME_SIZE[1], y + h + pad)
    gray = color_img.copy(roi=(x0, y0, x1 - x0, y1 - y0)).to_grayscale()
    hist = gray.get_histogram()
    threshold = int(attr(hist.get_threshold(), "value"))
    threshold += CANONICAL_THRESHOLD_OFFSET
    threshold += int(threshold_offset_delta)
    threshold = max(1, min(254, threshold))
    gray.binary([(0, threshold)])
    return normalize(gray, group, angle)


def back_patch(color_img, roi, angle=0, offset=(0, 0)):
    """Fixed textured ROI, with rotation context. Not character segmentation."""
    x, y, w, h = roi
    x, y = x + offset[0], y + offset[1]
    if x < 0 or y < 0 or x + w > 320 or y + h > 240:
        return None
    pad = int(max(w, h) * 0.25) + 3
    x0, y0 = max(0, x - pad), max(0, y - pad)
    x1, y1 = min(320, x + w + pad), min(240, y + h + pad)
    # Symmetric context is necessary to rotate about the patch centre.
    pad = min(x - x0, y - y0, x1 - x - w, y1 - y - h)
    src = color_img.copy(roi=(x - pad, y - pad, w + 2 * pad, h + 2 * pad)).to_grayscale()
    if angle:
        src.rotation_corr(z_rotation=angle)
    tw, th = C.PATCH_SIZES["back"]
    out = image.Image(tw, th, image.GRAYSCALE)
    out.draw_image(src, 0, 0, roi=(pad, pad, w, h), x_scale=tw / float(w),
                   y_scale=th / float(h), hint=image.AREA)
    if attr(out.get_statistics(), "stdev") < 5:
        return None
    return out


def color_evidence(color_img, box, settings=None):
    counts = []
    red_threshold = settings.get("color_red_threshold", C.RED_THRESHOLD) if settings else C.RED_THRESHOLD
    black_threshold = settings.get("color_black_threshold", C.BLACK_THRESHOLD) if settings else C.BLACK_THRESHOLD
    for threshold in (red_threshold, black_threshold):
        blobs = color_img.find_blobs([threshold], roi=box, x_stride=1, y_stride=1,
                                    pixels_threshold=1, area_threshold=1, merge=False)
        counts.append(sum(int(attr(b, "pixels")) for b in blobs))
    red, black = counts
    color = None
    red_min = settings.get("color_red_min_pixels", C.COLOR_MIN_PIXELS) if settings else C.COLOR_MIN_PIXELS
    black_min = settings.get("color_black_min_pixels", C.COLOR_MIN_PIXELS) if settings else C.COLOR_MIN_PIXELS
    # A sufficiently positive LAB a value is direct evidence of red ink.
    # Dark red can also satisfy a broad black locator, so do not compare the
    # two overlapping locator pixel counts as a ratio.
    if red >= red_min:
        color = "red"
    elif black >= black_min:
        color = "black"
    return {"color": color, "red": red, "black": black}


def geometry_ok(rank_box, suit_box, settings=None):
    rx, ry, rw, rh = rank_box
    sx, sy, sw, sh = suit_box
    dx = (sx + sw * 0.5) - (rx + rw * 0.5)
    dy = (sy + sh * 0.5) - (ry + rh * 0.5)
    max_dx = settings.get("pair_max_dx", C.PAIR_MAX_DX) if settings else C.PAIR_MAX_DX
    min_dy = settings.get("pair_min_dy", C.PAIR_MIN_DY) if settings else C.PAIR_MIN_DY
    max_dy = settings.get("pair_max_dy", C.PAIR_MAX_DY) if settings else C.PAIR_MAX_DY
    return (abs(dx) <= max_dx and min_dy <= dy <= max_dy
            and ry + rh <= sy + sh * 0.20)


def layout_pairs(ranks, suits, settings, color_img=None):
    """Return all plausible pairs, best geometric alignment first."""
    pairs = []
    target_dy = settings.get("pair_target_dy", 95)
    for rank in ranks:
        for suit in suits:
            if not geometry_ok(rank["box"], suit["box"], settings):
                continue
            rb, sb = rank["box"], suit["box"]
            dx = (sb[0] + sb[2] * 0.5) - (rb[0] + rb[2] * 0.5)
            dy = (sb[1] + sb[3] * 0.5) - (rb[1] + rb[3] * 0.5)
            color_penalty = 0
            if color_img is not None:
                if "color_evidence" not in rank:
                    rank["color_evidence"] = color_evidence(color_img, rb, settings)
                if "color_evidence" not in suit:
                    suit["color_evidence"] = color_evidence(color_img, sb, settings)
                rc = rank["color_evidence"]["color"]
                sc = suit["color_evidence"]["color"]
                if rc is not None and sc is not None and rc != sc:
                    continue
                if rc is None or sc is None:
                    color_penalty = 8
            penalty = abs(dx) + 0.20 * abs(dy - target_dy) + color_penalty
            pairs.append((penalty, rank, suit))
    pairs.sort(key=lambda item: item[0])
    return pairs


def load_bank(strict_coverage=False, max_templates_per_label=None):
    """Load templates compatible with the current image-processing pipeline.

    Camera exposure/gain/white-balance settings are deliberately not part of
    template compatibility.  This permits testing one template bank with
    different locked camera calibrations.  The canonicalization signature and
    patch dimensions remain mandatory compatibility checks.
    """
    bank = {k: [] for k in ("rank", "suit", "joker", "back")}
    bank_counts = {}
    wanted = {"rank": C.RANKS, "suit": C.SUITS, "joker": ("joker",), "back": ("back",)}
    expected = signature()
    for group in bank:
        directory = C.ROOT + "/templates/" + group
        for name in sorted(os.listdir(directory)):
            if not name.endswith(".pgm") or name.endswith(".tmp.pgm"):
                continue
            meta_path = directory + "/" + name[:-4] + ".json"
            if not exists(meta_path):
                raise ValueError("Uncommitted template (missing JSON): " + name)
            meta = read_json(meta_path)
            if meta.get("signature") != expected:
                raise ValueError("Template preprocessing mismatch: " + name)
            label = meta.get("label")
            if label not in wanted[group] or meta.get("group") != group:
                raise ValueError("Invalid template label/group: " + name)
            patch = image.Image(directory + "/" + name, copy_to_fb=False)
            if (patch.width(), patch.height()) != C.PATCH_SIZES[group]:
                raise ValueError("Wrong patch size: " + name)
            bank[group].append((label, patch, name))
        counts = {label: 0 for label in wanted[group]}
        for label, patch, name in bank[group]:
            counts[label] += 1
        if (max_templates_per_label is not None
                and any(n > max_templates_per_label for n in counts.values())):
            raise ValueError("Too many templates: " + group)
        missing = [label for label in wanted[group] if counts[label] == 0]
        if missing and strict_coverage:
            raise ValueError("Missing " + group + " templates: " + repr(missing))
        bank_counts[group] = sum(counts.values())
    print("BANK_READY rank=%d suit=%d joker=%d back=%d" % (
        bank_counts["rank"], bank_counts["suit"],
        bank_counts["joker"], bank_counts["back"]))
    gc.collect()
    return bank


def difference_score(patch, template):
    """Return bounded similarity derived from native mean absolute error.

    OpenMV's ``get_statistics(difference=...)`` performs the pixel difference
    in C without allocating another image.  All inputs have identical sizes.
    A perfect match is 1 and a full-scale difference is 0.
    """
    difference = float(attr(patch.get_statistics(difference=template), "mean"))
    if not math.isfinite(difference):
        return None
    score = 1.0 - difference / 255.0
    # Integer grayscale differences are mathematically in [0, 255].  Clamp
    # only harmless floating-point roundoff so confidence is always bounded.
    return max(0.0, min(1.0, score))


def score_patch(patch, catalogue, records, box, angle, budget=None):
    if patch is None:
        return
    for label, template, name in catalogue:
        check(budget)
        score = difference_score(patch, template)
        if score is None:
            continue
        old = records.get(label)
        if old is None or score > old["score"]:
            records[label] = {"label": label, "score": score, "box": box,
                              "angle": angle, "template": name}


def best_group(records, group, decision):
    ordered = sorted(records.values(), key=lambda r: r["score"], reverse=True)
    if not ordered:
        return {"accepted": False, "score": -1.0, "label": None, "margin": 0.0}
    best = dict(ordered[0])
    # Joker and back each have one class, so no meaningful runner-up exists.
    # Report a zero margin instead of score - (-1), which produced values > 1.
    second = ordered[1]["score"] if len(ordered) > 1 else best["score"]
    best["margin"] = best["score"] - second
    best["accepted"] = (best["score"] >= decision["accept_score"][group]
                        and best["margin"] >= decision["accept_margin"][group])
    return best


def decide(records, color_img, decision):
    groups = {k: best_group(records[k], k, decision) for k in records}
    rank, suit, joker, back = (groups[k] for k in ("rank", "suit", "joker", "back"))
    scenes = []
    normal_plausible = False
    normal_geometry = (rank.get("label") is not None and suit.get("label") is not None
                       and geometry_ok(rank["box"], suit["box"], decision))
    if (normal_geometry
            and rank["score"] >= decision["accept_score"]["rank"]
            and suit["score"] >= decision["accept_score"]["suit"]):
        evidence = color_evidence(color_img, suit["box"], decision)
        expected = "red" if suit["label"] in ("heart", "diamond") else "black"
        suit["color"] = evidence
        # Uncertain color does not exclude shape candidates. Explicit conflict
        # does reject a result; all 4 suits were scored before this check.
        if evidence["color"] in (None, expected):
            normal_plausible = True
    if normal_plausible and rank["accepted"] and suit["accepted"]:
        scenes.append((min(rank["score"] - decision["accept_score"]["rank"],
                           suit["score"] - decision["accept_score"]["suit"]),
                       suit["label"] + "_" + rank["label"]))
    joker_plausible = False
    if joker["accepted"]:
        evidence = color_evidence(color_img, joker["box"], decision)
        joker["color"] = evidence
        if evidence["color"] is not None:
            joker_plausible = True
            label = (decision["red_joker_label"] if evidence["color"] == "red"
                     else decision["black_joker_label"])
            scenes.append((joker["score"] - decision["accept_score"]["joker"], label))
    # Back is a grayscale, single-label fallback. A face with plausible rank
    # and suit evidence must not be overwritten merely because the fixed back
    # ROI happens to resemble the back template. If its margin is insufficient,
    # return UNKNOWN and report that ambiguity instead.
    if back["accepted"] and not normal_plausible and not joker_plausible:
        scenes.append((back["score"] - decision["accept_score"]["back"], "back"))
    scenes.sort(reverse=True)
    label, reason = "UNKNOWN", "LOW_SCORE_OR_GEOMETRY_OR_COLOR"
    if scenes:
        if len(scenes) == 1 or scenes[0][0] - scenes[1][0] >= decision["scene_margin"]:
            label, reason = scenes[0][1], "OK"
        else:
            reason = "SCENE_CONFLICT"
    return {"label": label, "reason": reason, "groups": groups}


def process_pass(color_img, bank, records, angles, settings,
                 threshold_offset_delta=0, budget=None):
    start = time.ticks_ms()
    located = {}
    diagnostics = {}
    for group in ("rank", "suit", "joker"):
        if bank[group]:
            located[group], diagnostics[group] = locate(
                color_img, group, settings, threshold_offset_delta, budget)
        else:
            located[group] = []
    if bank["rank"] and bank["suit"]:
        pairs = layout_pairs(located["rank"], located["suit"], settings, color_img)
        diagnostics["layout_pairs"] = len(pairs)
        keep = settings.get("max_layout_pairs", 3)
        if pairs:
            ranks, suits = [], []
            for unused, rank, suit in pairs[:keep]:
                if not any(rank is item for item in ranks):
                    ranks.append(rank)
                if not any(suit is item for item in suits):
                    suits.append(suit)
            located["rank"], located["suit"] = ranks, suits
        else:
            located["rank"], located["suit"] = [], []
    localized_ms = time.ticks_diff(time.ticks_ms(), start)
    for group in ("rank", "suit", "joker"):
        for candidate in located[group]:
            for angle in angles:
                check(budget)
                patch = canonical_patch(color_img, candidate["box"], group,
                                        settings, angle, threshold_offset_delta)
                score_patch(patch, bank[group], records[group], candidate["box"], angle, budget)
    if bank["back"] and threshold_offset_delta == 0:
        refine_angles = tuple(settings["refine_angles"])
        back_angles = (0,) + tuple(angles) if tuple(angles) == refine_angles else angles
        offsets = settings["back_refine_offsets"] if tuple(angles) == refine_angles else ((0, 0),)
        for angle in back_angles:
            for offset in offsets:
                check(budget)
                back_roi = settings["rois"]["back"]
                score_patch(back_patch(color_img, back_roi, angle, offset),
                            bank["back"], records["back"], back_roi, angle, budget)
    check(budget)
    result = decide(records, color_img, settings)
    check(budget)
    result["locate_ms"] = localized_ms
    result["pass_ms"] = time.ticks_diff(time.ticks_ms(), start)
    result["diagnostics"] = diagnostics
    return result
