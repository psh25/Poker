"""OpenMV H7 Plus template/sample capture (firmware 5.x / csi).

Copy cards_fast_config.py and cards_fast_core.py to /flash first.
1. Tune shared SEARCH_ROIS. Put a card at the nominal angle and focus lens.
2. First run: CALIBRATE_CAMERA=True. Later runs: False, same illumination.
3. Select labels below. P9 -> GND once saves one sample. Release to re-arm.
4. Inspect live tight boxes AND normalized patches before collecting.
5. SAVE_TEMPLATES=False collects independent validation frames, not templates.
"""
import gc
import os
import time
from machine import Pin
import cards_fast_config as C
import cards_fast_core as V

# ----------------------- Edit these before running -----------------------
CAPTURE_KIND = "normal"  # "normal", "joker", "back"; no no-card class.
RANK_LABEL = "A"
SUIT_LABEL = "spade"
JOKER_COLOR = "red"  # Metadata for actual red/black joker, not a guessed label.
NORMAL_SAVE_GROUPS = ("rank",)  # Change to ("suit",) for suit collection.
SAVE_TEMPLATES = True  # False: independent validation set, even if extraction fails.
CALIBRATE_CAMERA = False  # Set True ONCE, before any templates in this ROOT.
CAPTURE_CORRECTION_DEG = 0  # Nominal pose normally needs no correction.
CANDIDATE_INDEX = {"rank": 0, "suit": 0, "joker": 0}
PREVIEW_INTERVAL_MS = 150
# ------------------------------------------------------------------------


def validate_capture():
    V.validate_config()
    if CAPTURE_KIND not in ("normal", "joker", "back"):
        raise ValueError("Invalid CAPTURE_KIND")
    if RANK_LABEL not in C.RANKS or SUIT_LABEL not in C.SUITS:
        raise ValueError("Invalid rank/suit label")
    if JOKER_COLOR not in ("red", "black"):
        raise ValueError("JOKER_COLOR must be red or black")
    if not NORMAL_SAVE_GROUPS or any(g not in ("rank", "suit") for g in NORMAL_SAVE_GROUPS):
        raise ValueError("Invalid NORMAL_SAVE_GROUPS")


def selection(frame):
    patches, boxes, diagnostics = {}, {}, {}
    groups = ("rank", "suit") if CAPTURE_KIND == "normal" else (CAPTURE_KIND,)
    for group in groups:
        if group == "back":
            patches[group] = V.back_patch(frame, CAPTURE_CORRECTION_DEG)
            boxes[group] = C.SEARCH_ROIS[group]
            diagnostics[group] = {"reason": "OK" if patches[group] is not None else "LOW_TEXTURE"}
            continue
        candidates, info = V.locate(frame, group)
        diagnostics[group] = info
        index = CANDIDATE_INDEX[group]
        if index < 0 or index >= len(candidates):
            patches[group] = None
            continue
        candidate = candidates[index]
        boxes[group] = candidate["box"]
        patches[group] = V.normalize(candidate["mask"], group, CAPTURE_CORRECTION_DEG)
        diagnostics[group]["candidates"] = len(candidates)
        diagnostics[group]["color"] = V.color_evidence(frame, candidate["box"])
    return patches, boxes, diagnostics


def groups_to_save():
    return NORMAL_SAVE_GROUPS if CAPTURE_KIND == "normal" else (CAPTURE_KIND,)


def label_for(group):
    return RANK_LABEL if group == "rank" else SUIT_LABEL if group == "suit" else group


def next_sample_stem():
    index = 0
    while True:
        stem = "sample_%06d" % index
        if not V.exists(C.ROOT + "/samples/" + stem + ".ppm") and not V.exists(C.ROOT + "/samples/" + stem + ".json"):
            return stem
        index += 1


def check_label_count(group, label):
    directory = C.ROOT + "/templates/" + group
    count = 0
    for name in os.listdir(directory):
        if name.endswith(".json") and V.read_json(directory + "/" + name).get("label") == label:
            count += 1
    if count >= C.MAX_TEMPLATES_PER_LABEL:
        raise ValueError("Template limit reached for %s/%s; use validation mode or curate bank" % (group, label))


def save_capture(frame, camera, patches, boxes, diagnostics):
    groups = groups_to_save()
    failures = []
    if SAVE_TEMPLATES:
        for group in groups:
            if patches.get(group) is None:
                failures.append(group + ":NO_VALID_PATCH")
            check_label_count(group, label_for(group))
        if CAPTURE_KIND == "normal":
            if "rank" not in boxes or "suit" not in boxes or not V.geometry_ok(boxes["rank"], boxes["suit"]):
                failures.append("NORMAL:BAD_RANK_SUIT_LAYOUT")
        for group in ("suit", "joker"):
            if group not in boxes:
                continue
            actual = V.color_evidence(frame, boxes[group])["color"]
            expected = JOKER_COLOR if group == "joker" else ("red" if SUIT_LABEL in ("heart", "diamond") else "black")
            if actual != expected:
                failures.append(group + ":CALIBRATE_COLOR_OR_CHECK_LABEL")
    stem = next_sample_stem()
    raw_path = C.ROOT + "/samples/" + stem + ".ppm"
    # Save the UNANNOTATED RGB565 frame as lossless PPM, before preview drawing.
    frame.save(raw_path)
    metadata = {"kind": CAPTURE_KIND, "rank": RANK_LABEL if CAPTURE_KIND == "normal" else None,
                "suit": SUIT_LABEL if CAPTURE_KIND == "normal" else None,
                "joker_color": JOKER_COLOR if CAPTURE_KIND == "joker" else None,
                "camera": camera, "signature": V.signature(), "boxes": boxes,
                "diagnostics": diagnostics, "correction_deg": CAPTURE_CORRECTION_DEG,
                "validation_only": not SAVE_TEMPLATES, "failures": failures, "templates": []}
    if SAVE_TEMPLATES and not failures:
        for group in groups:
            base = C.ROOT + "/templates/" + group + "/" + label_for(group) + "_" + stem
            patches[group].save(base + ".pgm")
            # Metadata is the per-template commit marker; never load raw PGM.
            V.write_json(base + ".json", {"group": group, "label": label_for(group),
                         "camera": camera, "signature": V.signature(), "source": raw_path,
                         "box": boxes[group], "joker_color": metadata["joker_color"]})
            metadata["templates"].append(base + ".pgm")
    V.write_json(C.ROOT + "/samples/" + stem + ".json", metadata)
    os.sync()
    print("CAPTURE_REJECTED" if failures else "TEMPLATE_SAVED" if SAVE_TEMPLATES else "VALIDATION_SAVED", stem)
    print("  diagnostics:", diagnostics)
    if failures:
        print("  raw frame saved; no templates added:", failures)
    return "REJECTED" if failures else "SAVED"


def main():
    validate_capture()
    V.ensure_storage()
    if CALIBRATE_CAMERA:
        for group in ("rank", "suit", "joker", "back"):
            if any(n.endswith(".pgm") for n in os.listdir(C.ROOT + "/templates/" + group)):
                raise ValueError("Camera recalibration needs a new ROOT or an archived template bank")
    cam, camera, leds = V.start_camera(CALIBRATE_CAMERA)
    button = Pin(C.TRIGGER_PIN, Pin.IN, Pin.PULL_UP)
    last_raw = stable = button.value()
    changed = last_preview = time.ticks_ms()
    patches, boxes, diagnostics = {}, {}, {}
    status = "READY"
    print("Capture ready:", CAPTURE_KIND, RANK_LABEL, SUIT_LABEL, "save", groups_to_save())
    print("Camera:", camera)
    print("SEARCH ROIs must be tuned; P9 to GND saves. Template pixels are shown at right.")
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
                status = save_capture(frame, camera, patches, boxes, diagnostics)
            except Exception as error:
                status = "SAVE_ERROR"
                print("CAPTURE_ERROR:", error)
            gc.collect()
        groups = ("rank", "suit") if CAPTURE_KIND == "normal" else (CAPTURE_KIND,)
        py = 25
        for group in groups:
            frame.draw_rectangle(C.SEARCH_ROIS[group], color=(0, 80, 255))
            if group in boxes:
                frame.draw_rectangle(boxes[group], color=(0, 255, 0))
            if patches.get(group) is not None:
                frame.draw_image(patches[group], 270, py)
                py += C.PATCH_SIZES[group][1] + 5
        frame.draw_string((2, 2), status + " " + CAPTURE_KIND, color=(255, 255, 255))


if __name__ == "__main__":
    main()
