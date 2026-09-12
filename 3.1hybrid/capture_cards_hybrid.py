"""OpenMV H7 Plus hybrid LAB/local-Otsu template capture.

Copy cards_fast_config.py and cards_hybrid_core.py beside this script on /flash.
1. Tune CAPTURE_ROIS to cover the measured mechanical range.
2. Set CALIBRATE_CAMERA=True whenever deliberately testing a new locked
   exposure/gain/white-balance calibration. Existing templates remain usable.
3. Select labels below. P9 -> GND once saves one sample. Release to re-arm.
4. Inspect live tight boxes AND normalized patches before collecting.
5. SAVE_TEMPLATES=False performs a visual validation pass without writing files.
"""
import gc
import os
import time
from machine import Pin
import cards_fast_config as C
import cards_hybrid_core as V

# ----------------------- Edit these before running -----------------------
CAPTURE_KIND = "normal"  # "normal", "joker", "back"; no no-card class.
RANK_LABEL = "Q"
SUIT_LABEL = "club"
JOKER_COLOR = "red"  # Metadata for actual red/black joker, not a guessed label.
NORMAL_SAVE_GROUPS = ("rank",)  # Change to ("suit",) for suit collection.
SAVE_TEMPLATES = False  # False: independent validation set, even if extraction fails.
CALIBRATE_CAMERA = True  # Existing templates no longer block recalibration.
CAPTURE_CORRECTION_DEG = 0  # Nominal pose normally needs no correction.
PREVIEW_INTERVAL_MS = 150
MAX_TEMPLATES_PER_LABEL = 2

# Search windows may be broad; the selected glyph is cropped again before Otsu.
CAPTURE_ROIS = {
    "rank": (105, 25, 100, 145),
    "suit": (105, 130, 100, 110),
    # Keep only the stable visible JOKER lettering, without the artwork.
    "joker": (120, 40, 50, 140),
    "back": (170, 120, 40, 40),
}

CAPTURE_VISION = {
    "rois": CAPTURE_ROIS,
    "red_threshold": C.RED_THRESHOLD,
    # Wider a/b range captures black ink under the measured green/cyan cast;
    # L<=45 still rejects the bright card, ceiling and dotted glare.
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
}

ROI_COLORS = {
    "rank": (255, 0, 0),
    "suit": (0, 0, 255),
    "joker": (255, 0, 255),
    "back": (0, 255, 255),
}
# ------------------------------------------------------------------------


def validate_capture():
    V.validate_config(CAPTURE_VISION)
    if CAPTURE_KIND not in ("normal", "joker", "back"):
        raise ValueError("Invalid CAPTURE_KIND")
    if RANK_LABEL not in C.RANKS or SUIT_LABEL not in C.SUITS:
        raise ValueError("Invalid rank/suit label")
    if JOKER_COLOR not in ("red", "black"):
        raise ValueError("JOKER_COLOR must be red or black")
    if not NORMAL_SAVE_GROUPS or any(g not in ("rank", "suit") for g in NORMAL_SAVE_GROUPS):
        raise ValueError("Invalid NORMAL_SAVE_GROUPS")
    if MAX_TEMPLATES_PER_LABEL < 1:
        raise ValueError("MAX_TEMPLATES_PER_LABEL must be positive")


def selection(frame):
    patches, boxes, diagnostics = {}, {}, {}
    groups = ("rank", "suit") if CAPTURE_KIND == "normal" else (CAPTURE_KIND,)
    located = {}
    for group in groups:
        if group == "back":
            boxes[group] = CAPTURE_ROIS[group]
            patches[group] = V.back_patch(frame, boxes[group], CAPTURE_CORRECTION_DEG)
            diagnostics[group] = {"reason": "OK" if patches[group] is not None else "LOW_TEXTURE"}
            continue
        candidates, info = V.locate(frame, group, CAPTURE_VISION)
        located[group] = candidates
        diagnostics[group] = info
        diagnostics[group]["candidates"] = len(candidates)

    selected = {}
    if CAPTURE_KIND == "normal":
        pairs = V.layout_pairs(located.get("rank", []), located.get("suit", []),
                               CAPTURE_VISION, frame)
        diagnostics["layout_pairs"] = len(pairs)
        if pairs:
            selected["rank"], selected["suit"] = pairs[0][1], pairs[0][2]
    elif CAPTURE_KIND != "back" and located.get(CAPTURE_KIND):
        selected[CAPTURE_KIND] = located[CAPTURE_KIND][0]

    for group, candidate in selected.items():
        boxes[group] = candidate["box"]
        patches[group] = V.canonical_patch(
            frame, candidate["box"], group, CAPTURE_VISION,
            CAPTURE_CORRECTION_DEG)
        diagnostics[group]["color"] = V.color_evidence(frame, candidate["box"], CAPTURE_VISION)
    return patches, boxes, diagnostics


def groups_to_save():
    return NORMAL_SAVE_GROUPS if CAPTURE_KIND == "normal" else (CAPTURE_KIND,)


def label_for(group):
    return RANK_LABEL if group == "rank" else SUIT_LABEL if group == "suit" else group


def next_template_base(group, label):
    directory = C.ROOT + "/templates/" + group
    index = 0
    while True:
        base = directory + "/" + label + "_%02d" % index
        if (not V.exists(base + ".pgm") and not V.exists(base + ".json")
                and not V.exists(base + ".tmp.pgm") and not V.exists(base + ".tmp.json")):
            return base
        index += 1


def check_label_count(group, label):
    directory = C.ROOT + "/templates/" + group
    count = 0
    for name in os.listdir(directory):
        pgm_path = directory + "/" + name[:-5] + ".pgm"
        if name.endswith(".json") and not name.endswith(".tmp.json") and V.exists(pgm_path):
            metadata = V.read_json(directory + "/" + name)
            if (metadata.get("signature") == V.signature()
                    and metadata.get("label") == label):
                count += 1
    if count >= MAX_TEMPLATES_PER_LABEL:
        raise ValueError("Template limit reached for %s/%s; use validation mode or curate bank" % (group, label))


def require_compatible_bank():
    """Do not silently mix invalid older templates into the unique bank."""
    for group in ("rank", "suit", "joker", "back"):
        directory = C.ROOT + "/templates/" + group
        for name in os.listdir(directory):
            if not name.endswith(".pgm") or name.endswith(".tmp.pgm"):
                continue
            meta_path = directory + "/" + name[:-4] + ".json"
            if not V.exists(meta_path):
                raise ValueError("Remove incomplete old template: " + name)
            metadata = V.read_json(meta_path)
            if metadata.get("signature") != V.signature():
                raise ValueError("Old/incompatible preprocessing under %s; archive or clear templates first" % C.ROOT)


def save_capture(frame, patches, boxes, diagnostics):
    groups = groups_to_save()
    failures = []
    if SAVE_TEMPLATES:
        for group in groups:
            if patches.get(group) is None:
                failures.append(group + ":NO_VALID_PATCH")
            check_label_count(group, label_for(group))
        if CAPTURE_KIND == "normal":
            if ("rank" not in boxes or "suit" not in boxes
                    or not V.geometry_ok(boxes["rank"], boxes["suit"], CAPTURE_VISION)):
                failures.append("NORMAL:BAD_RANK_SUIT_LAYOUT")
        for group in ("suit", "joker"):
            if group not in boxes:
                continue
            actual = V.color_evidence(frame, boxes[group], CAPTURE_VISION)["color"]
            expected = JOKER_COLOR if group == "joker" else ("red" if SUIT_LABEL in ("heart", "diamond") else "black")
            if actual != expected:
                failures.append(group + ":CALIBRATE_COLOR_OR_CHECK_LABEL")
    if failures:
        print("CAPTURE_REJECTED")
        print("  diagnostics:", diagnostics)
        print("  no files written:", failures)
        return "REJECTED"
    if not SAVE_TEMPLATES:
        print("VALIDATION_OK (no files written)")
        print("  diagnostics:", diagnostics)
        return "VALID"
    saved = []
    if SAVE_TEMPLATES:
        for group in groups:
            label = label_for(group)
            base = next_template_base(group, label)
            metadata = {"group": group, "label": label,
                        "signature": V.signature(), "box": boxes[group],
                        "joker_color": JOKER_COLOR if group == "joker" else None,
                        "correction_deg": CAPTURE_CORRECTION_DEG,
                        "canonical": "tight_local_otsu"}
            saved.append(V.save_template_atomic(patches[group], base, metadata))
    print("TEMPLATE_SAVED", saved)
    print("  diagnostics:", diagnostics)
    return "SAVED"


def main():
    validate_capture()
    free_bytes = V.ensure_storage()
    cam, camera, leds = V.start_camera(CALIBRATE_CAMERA)
    require_compatible_bank()
    button = Pin(C.TRIGGER_PIN, Pin.IN, Pin.PULL_UP)
    last_raw = stable = button.value()
    changed = last_preview = time.ticks_ms()
    patches, boxes, diagnostics = {}, {}, {}
    status = "READY"
    print("Capture ready:", CAPTURE_KIND, RANK_LABEL, SUIT_LABEL, "save", groups_to_save())
    print("Template storage:", C.ROOT, "free_bytes=", free_bytes)
    print("Camera:", camera)
    print("LAB locate + local Otsu; P9 to GND saves.")
    while True:
        frame = cam.snapshot()
        now = time.ticks_ms()
        raw = button.value()
        if raw != last_raw:
            last_raw, changed = raw, now
        pressed = False
        if raw != stable and time.ticks_diff(now, changed) >= C.BUTTON_DEBOUNCE_MS:
            stable = raw
            pressed = stable == 0
        if pressed or time.ticks_diff(now, last_preview) >= PREVIEW_INTERVAL_MS:
            patches, boxes, diagnostics = selection(frame)
            last_preview = now
        if pressed:
            try:
                status = save_capture(frame, patches, boxes, diagnostics)
            except Exception as error:
                status = "SAVE_ERROR"
                print("CAPTURE_ERROR:", error)
            gc.collect()
        groups = ("rank", "suit") if CAPTURE_KIND == "normal" else (CAPTURE_KIND,)
        py = 25
        for group in groups:
            frame.draw_rectangle(CAPTURE_ROIS[group], color=ROI_COLORS[group])
            if group in boxes:
                frame.draw_rectangle(boxes[group], color=(0, 255, 0))
            if patches.get(group) is not None:
                frame.draw_image(patches[group], 270, py)
                py += C.PATCH_SIZES[group][1] + 5
        frame.draw_string((2, 2), status + " " + CAPTURE_KIND, color=(255, 255, 255))


if __name__ == "__main__":
    main()
