"""Shared native-image pipeline for OpenMV 5.x. No OpenCV/CNN on the board.

Only connected-component metadata is processed in Python. Pixel operations,
rotation, resizing, histogram, and SSIM are OpenMV native operations.
"""
import gc
import json
import math
import os
import time
import image
import cards_fast_config as C


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


def ensure_storage():
    mkdir(C.ROOT)
    mkdir(C.ROOT + "/samples")
    mkdir(C.ROOT + "/templates")
    for group in ("rank", "suit", "joker", "back"):
        mkdir(C.ROOT + "/templates/" + group)


def read_json(path):
    with open(path, "r") as stream:
        return json.load(stream)


def write_json(path, data):
    with open(path, "w") as stream:
        json.dump(data, stream)


def signature():
    # Stable, explicit preprocessing schema. Decision thresholds may be tuned
    # without recollection; changes to geometry/masks must rebuild templates.
    return repr((C.PIPELINE_VERSION, C.FRAME_SIZE,
                 tuple((k, C.SEARCH_ROIS[k], C.PATCH_SIZES[k])
                       for k in ("rank", "suit", "joker", "back")),
                 C.PATCH_PADDING, C.MIN_CONTRAST, C.THRESHOLD_OFFSET,
                 C.MIN_COMPONENT_PIXELS, C.MAX_COMPONENTS, C.MAX_CANDIDATES,
                 tuple((k, C.MIN_BOX[k], C.MAX_BOX[k]) for k in ("rank", "suit", "joker")),
                 C.MIN_INK_FRACTION, C.MAX_INK_FRACTION, C.BORDER_GUARD,
                 C.ROW_OVERLAP, C.ROW_GAP_HEIGHT))


def validate_config():
    for group, roi in C.SEARCH_ROIS.items():
        x, y, w, h = roi
        if min(x, y) < 0 or min(w, h) <= 0 or x + w > 320 or y + h > 240:
            raise ValueError("Invalid QVGA search ROI: " + group)
        if min(C.PATCH_SIZES[group]) <= 2 * C.PATCH_PADDING:
            raise ValueError("Patch too small: " + group)
    if C.MAX_CANDIDATES < 1 or C.MAX_TEMPLATES_PER_LABEL < 1:
        raise ValueError("Invalid candidate/template limit")
    if any(abs(a) > 20 for a in C.COARSE_ANGLES + C.REFINE_ANGLES):
        raise ValueError("This pipeline is for small rotations (<=20 degrees)")


def start_camera(calibrate=False):
    """Shared persisted exposure/gain/WB. Recognition never auto-calibrates."""
    import csi
    from machine import LED
    leds = [LED("LED_RED"), LED("LED_GREEN"), LED("LED_BLUE")]
    for led in leds:
        led.on()
    path = C.ROOT + "/camera.json"
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
    probe.get_similarity(probe)
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


def group_components(items, group):
    """Union one text row; 10 remains intact. JOKER ROI is lettering-only."""
    if group == "joker":
        if not items:
            return []
        box, pixels = items[0]
        for other, count in items[1:]:
            box, pixels = union_box(box, other), pixels + count
        return [(box, pixels)]
    items = list(items)
    changed = True
    while changed:
        changed = False
        for i in range(len(items)):
            for j in range(i + 1, len(items)):
                if components_join(items[i][0], items[j][0]):
                    items[i] = (union_box(items[i][0], items[j][0]), items[i][1] + items[j][1])
                    items.pop(j)
                    changed = True
                    break
            if changed:
                break
    return items


def make_mask(color_img, group, threshold_offset=0):
    roi = C.SEARCH_ROIS[group]
    gray = color_img.copy(roi=roi).to_grayscale()
    hist = gray.get_histogram()
    low = attr(hist.get_percentile(0.05), "value")
    high = attr(hist.get_percentile(0.95), "value")
    if high - low < C.MIN_CONTRAST:
        return None, {"reason": "LOW_CONTRAST", "contrast": high - low}
    threshold = int(attr(hist.get_threshold(), "value")) + C.THRESHOLD_OFFSET + threshold_offset
    threshold = max(1, min(254, threshold))
    # Dark/red ink becomes WHITE foreground on BLACK. Native rotation's black
    # fill therefore cannot create fake dark strokes around image corners.
    gray.binary([(0, threshold)])
    return gray, {"threshold": threshold, "contrast": high - low}


def locate(color_img, group, threshold_offset=0, budget=None):
    check(budget)
    mask, info = make_mask(color_img, group, threshold_offset)
    if mask is None:
        return [], info
    check(budget)
    blobs = mask.find_blobs([(128, 255)], x_stride=1, y_stride=1,
                           pixels_threshold=C.MIN_COMPONENT_PIXELS,
                           area_threshold=C.MIN_COMPONENT_PIXELS, merge=False)
    if len(blobs) > C.MAX_COMPONENTS:
        return [], {"reason": "TOO_MANY_COMPONENTS"}
    items = [(tuple(attr(b, "rect")), int(attr(b, "pixels"))) for b in blobs]
    items = group_components(items, group)
    rx, ry, rw, rh = C.SEARCH_ROIS[group]
    candidates = []
    for box, pixels in items:
        x, y, w, h = box
        minw, minh = C.MIN_BOX[group]
        maxw, maxh = C.MAX_BOX[group]
        g = C.BORDER_GUARD
        if x <= g or y <= g or x + w >= rw - g or y + h >= rh - g:
            continue
        if not (minw <= w <= maxw and minh <= h <= maxh):
            continue
        density = pixels / float(w * h)
        if not (C.MIN_INK_FRACTION <= density <= C.MAX_INK_FRACTION):
            continue
        # Crop to candidate BEFORE doing any angle search.
        candidates.append({"mask": mask.copy(roi=box), "box": (rx + x, ry + y, w, h),
                           "pixels": pixels, "density": density})
    candidates.sort(key=lambda c: c["pixels"], reverse=True)
    info["reason"] = "OK" if candidates else "NO_COMPLETE_SYMBOL"
    return candidates[:C.MAX_CANDIDATES], info


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


def back_patch(color_img, angle=0, offset=(0, 0)):
    """Fixed textured ROI, with rotation context. Not character segmentation."""
    x, y, w, h = C.SEARCH_ROIS["back"]
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


def color_evidence(color_img, box):
    counts = []
    for threshold in (C.RED_THRESHOLD, C.BLACK_THRESHOLD):
        blobs = color_img.find_blobs([threshold], roi=box, x_stride=1, y_stride=1,
                                    pixels_threshold=1, area_threshold=1, merge=False)
        counts.append(sum(int(attr(b, "pixels")) for b in blobs))
    red, black = counts
    total = red + black
    color = None
    if total >= C.COLOR_MIN_PIXELS:
        if red / float(total) >= C.COLOR_MIN_FRACTION:
            color = "red"
        elif black / float(total) >= C.COLOR_MIN_FRACTION:
            color = "black"
    return {"color": color, "red": red, "black": black}


def geometry_ok(rank_box, suit_box):
    rx, ry, rw, rh = rank_box
    sx, sy, sw, sh = suit_box
    dx = (sx + sw * 0.5) - (rx + rw * 0.5)
    dy = (sy + sh * 0.5) - (ry + rh * 0.5)
    return (abs(dx) <= C.PAIR_MAX_DX and C.PAIR_MIN_DY <= dy <= C.PAIR_MAX_DY
            and ry + rh <= sy + sh * 0.20)


def load_bank(camera_settings):
    bank = {k: [] for k in ("rank", "suit", "joker", "back")}
    wanted = {"rank": C.RANKS, "suit": C.SUITS, "joker": ("joker",), "back": ("back",)}
    expected = signature()
    for group in bank:
        directory = C.ROOT + "/templates/" + group
        for name in sorted(os.listdir(directory)):
            if not name.endswith(".pgm"):
                continue
            meta_path = directory + "/" + name[:-4] + ".json"
            if not exists(meta_path):
                raise ValueError("Uncommitted template (missing JSON): " + name)
            meta = read_json(meta_path)
            if meta.get("signature") != expected or meta.get("camera") != camera_settings:
                raise ValueError("Template preprocessing/camera mismatch: " + name)
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
        if any(n > C.MAX_TEMPLATES_PER_LABEL for n in counts.values()):
            raise ValueError("Too many templates: " + group)
        missing = [label for label in wanted[group] if counts[label] == 0]
        if missing and C.STRICT_TEMPLATE_COVERAGE:
            raise ValueError("Missing " + group + " templates: " + repr(missing))
        print("BANK", group, counts)
    gc.collect()
    return bank


def score_patch(patch, catalogue, records, box, angle, budget=None):
    if patch is None:
        return
    for label, template, name in catalogue:
        check(budget)
        score = float(attr(patch.get_similarity(template), "mean"))
        if not math.isfinite(score):
            continue
        old = records.get(label)
        if old is None or score > old["score"]:
            records[label] = {"label": label, "score": score, "box": box,
                              "angle": angle, "template": name}


def best_group(records, group):
    ordered = sorted(records.values(), key=lambda r: r["score"], reverse=True)
    if not ordered:
        return {"accepted": False, "score": -1.0, "label": None, "margin": 0.0}
    best = dict(ordered[0])
    second = ordered[1]["score"] if len(ordered) > 1 else -1.0
    best["margin"] = best["score"] - second
    best["accepted"] = (best["score"] >= C.ACCEPT_SCORE[group]
                        and best["margin"] >= C.ACCEPT_MARGIN[group])
    return best


def decide(records, color_img):
    groups = {k: best_group(records[k], k) for k in records}
    rank, suit, joker, back = (groups[k] for k in ("rank", "suit", "joker", "back"))
    scenes = []
    if rank["accepted"] and suit["accepted"] and geometry_ok(rank["box"], suit["box"]):
        evidence = color_evidence(color_img, suit["box"])
        expected = "red" if suit["label"] in ("heart", "diamond") else "black"
        suit["color"] = evidence
        # Uncertain color does not exclude shape candidates. Explicit conflict
        # does reject a result; all 4 suits were scored before this check.
        if evidence["color"] in (None, expected):
            scenes.append((min(rank["score"] - C.ACCEPT_SCORE["rank"],
                               suit["score"] - C.ACCEPT_SCORE["suit"]),
                           suit["label"] + "_" + rank["label"]))
    if joker["accepted"]:
        evidence = color_evidence(color_img, joker["box"])
        joker["color"] = evidence
        if evidence["color"] is not None:
            label = C.RED_JOKER_LABEL if evidence["color"] == "red" else C.BLACK_JOKER_LABEL
            scenes.append((joker["score"] - C.ACCEPT_SCORE["joker"], label))
    if back["accepted"]:
        scenes.append((back["score"] - C.ACCEPT_SCORE["back"], "back"))
    scenes.sort(reverse=True)
    label, reason = "UNKNOWN", "LOW_SCORE_OR_GEOMETRY_OR_COLOR"
    if scenes:
        if len(scenes) == 1 or scenes[0][0] - scenes[1][0] >= C.SCENE_MARGIN:
            label, reason = scenes[0][1], "OK"
        else:
            reason = "SCENE_CONFLICT"
    return {"label": label, "reason": reason, "groups": groups}


def process_pass(color_img, bank, records, angles, threshold_offset=0, budget=None):
    start = time.ticks_ms()
    located = {}
    diagnostics = {}
    for group in ("rank", "suit", "joker"):
        if bank[group]:
            located[group], diagnostics[group] = locate(color_img, group, threshold_offset, budget)
        else:
            located[group] = []
    localized_ms = time.ticks_diff(time.ticks_ms(), start)
    for group in ("rank", "suit", "joker"):
        for candidate in located[group]:
            for angle in angles:
                check(budget)
                patch = normalize(candidate["mask"], group, angle)
                score_patch(patch, bank[group], records[group], candidate["box"], angle, budget)
    if bank["back"] and threshold_offset == 0:
        back_angles = (0,) + tuple(angles) if tuple(angles) == C.REFINE_ANGLES else angles
        offsets = C.BACK_REFINE_OFFSETS if tuple(angles) == C.REFINE_ANGLES else ((0, 0),)
        for angle in back_angles:
            for offset in offsets:
                check(budget)
                score_patch(back_patch(color_img, angle, offset), bank["back"], records["back"],
                            C.SEARCH_ROIS["back"], angle, budget)
    check(budget)
    result = decide(records, color_img)
    check(budget)
    result["locate_ms"] = localized_ms
    result["pass_ms"] = time.ticks_diff(time.ticks_ms(), start)
    result["diagnostics"] = diagnostics
    return result
