"""Host-only logic/image smoke tests. Does NOT benchmark/emulate the H7 CPU.

The small OpenCV adapter exercises shared preprocessing on controlled images;
OpenMV interpolation, blob connectivity, and SSIM kernels can differ slightly.
Run: python -m pytest -q tests/test_cards_fast.py
"""
import importlib
import json
import os
from pathlib import Path
import sys
import time
import types

import cv2
import numpy as np
import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))


class Histogram:
    def __init__(self, pixels):
        self.pixels = pixels

    def get_percentile(self, p):
        return types.SimpleNamespace(value=int(np.percentile(self.pixels, p * 100)))

    def get_threshold(self):
        value, unused = cv2.threshold(self.pixels, 0, 255, cv2.THRESH_BINARY | cv2.THRESH_OTSU)
        return types.SimpleNamespace(value=value)


class HostImage:
    def __init__(self, width, height=None, fmt=None, copy_to_fb=False):
        if isinstance(width, str):
            self.a = cv2.imdecode(np.fromfile(width, dtype=np.uint8), cv2.IMREAD_GRAYSCALE)
        elif isinstance(width, np.ndarray):
            self.a = width.copy()
        else:
            self.a = np.zeros((height, width), dtype=np.uint8)

    def width(self):
        return self.a.shape[1]

    def height(self):
        return self.a.shape[0]

    def copy(self, roi=None):
        if roi is None:
            return HostImage(self.a)
        x, y, w, h = roi
        return HostImage(self.a[y:y+h, x:x+w])

    def to_grayscale(self):
        if self.a.ndim == 3:
            self.a = cv2.cvtColor(self.a, cv2.COLOR_BGR2GRAY)
        return self

    def get_histogram(self):
        return Histogram(self.a)

    def get_statistics(self, difference=None):
        if difference is None:
            return types.SimpleNamespace(mean=float(self.a.mean()),
                                         stdev=float(self.a.std()))
        delta = np.abs(self.a.astype(np.int16) - difference.a.astype(np.int16))
        return types.SimpleNamespace(mean=float(delta.mean()),
                                     stdev=float(delta.std()))

    def binary(self, thresholds):
        if self.a.ndim == 3:
            lab = cv2.cvtColor(self.a, cv2.COLOR_BGR2LAB).astype(np.float32)
            lab[:, :, 0] *= 100.0 / 255
            lab[:, :, 1:] -= 128
            mask = np.zeros(self.a.shape[:2], np.uint8)
            for threshold in thresholds:
                selected = np.ones(self.a.shape[:2], np.uint8)
                for k in range(3):
                    selected &= ((lab[:, :, k] >= threshold[2*k])
                                 & (lab[:, :, k] <= threshold[2*k+1]))
                mask |= selected
        else:
            mask = np.zeros(self.a.shape, np.uint8)
            for lo, hi in thresholds:
                mask |= ((self.a >= lo) & (self.a <= hi))
        self.a = mask.astype(np.uint8) * 255
        return self

    def mean(self, size, threshold=False, offset=0, invert=False, **kwargs):
        kernel = 2 * size + 1
        source = self.a.copy()
        local = cv2.blur(source, (kernel, kernel), borderType=cv2.BORDER_REPLICATE)
        if threshold:
            matched = source.astype(np.int16) > local.astype(np.int16) + offset
            if invert:
                matched = ~matched
            self.a = matched.astype(np.uint8) * 255
        else:
            self.a = local
        return self

    def find_blobs(self, thresholds, roi=None, pixels_threshold=1, area_threshold=1, **kwargs):
        x, y, w, h = roi or (0, 0, self.width(), self.height())
        a = self.a[y:y+h, x:x+w]
        threshold = thresholds[0]
        if len(threshold) == 2:
            lo, hi = threshold
            mask = ((a >= lo) & (a <= hi)).astype(np.uint8)
        else:
            lab = cv2.cvtColor(a, cv2.COLOR_BGR2LAB).astype(np.float32)
            lab[:, :, 0] *= 100.0 / 255
            lab[:, :, 1:] -= 128
            mask = np.ones(a.shape[:2], np.uint8)
            for k in range(3):
                mask &= ((lab[:, :, k] >= threshold[2*k]) & (lab[:, :, k] <= threshold[2*k+1]))
        count, labels, stats, centroids = cv2.connectedComponentsWithStats(mask, connectivity=8)
        blobs = []
        for sx, sy, sw, sh, n in stats[1:]:
            if n >= pixels_threshold and sw * sh >= area_threshold:
                blobs.append(types.SimpleNamespace(rect=(int(sx+x), int(sy+y), int(sw), int(sh)), pixels=int(n)))
        return blobs

    def draw_image(self, other, x=0, y=0, roi=None, x_scale=1, y_scale=1, **kwargs):
        source = other.copy(roi).a
        w, h = max(1, int(source.shape[1] * x_scale)), max(1, int(source.shape[0] * y_scale))
        scaled = cv2.resize(source, (w, h), interpolation=cv2.INTER_AREA)
        self.a[y:y+h, x:x+w] = scaled
        return self

    def rotation_corr(self, z_rotation=0):
        matrix = cv2.getRotationMatrix2D(((self.width()-1)/2, (self.height()-1)/2), z_rotation, 1)
        self.a = cv2.warpAffine(self.a, matrix, (self.width(), self.height()))
        return self

    def get_similarity(self, other):
        # 8x8-block SSIM approximation for host checks, not a firmware substitute.
        scores = []
        for y in range(0, self.height(), 8):
            for x in range(0, self.width(), 8):
                a = self.a[y:y+8, x:x+8].astype(float)
                b = other.a[y:y+8, x:x+8].astype(float)
                ma, mb = a.mean(), b.mean()
                cov = ((a-ma)*(b-mb)).mean()
                scores.append(((2*ma*mb+6.5025)*(2*cov+58.5225)) /
                              ((ma*ma+mb*mb+6.5025)*(a.var()+b.var()+58.5225)))
        return types.SimpleNamespace(mean=float(np.mean(scores)))

    def save(self, path):
        cv2.imencode(Path(path).suffix, self.a)[1].tofile(path)


@pytest.fixture
def modules(monkeypatch):
    monkeypatch.setitem(sys.modules, "image", types.SimpleNamespace(Image=HostImage, GRAYSCALE=0, AREA=0))
    monkeypatch.setitem(sys.modules, "machine", types.SimpleNamespace(Pin=object, UART=object))
    monkeypatch.setattr(time, "ticks_ms", lambda: int(time.monotonic() * 1000), raising=False)
    monkeypatch.setattr(time, "ticks_diff", lambda a, b: a-b, raising=False)
    monkeypatch.setattr(time, "sleep_ms", lambda ms: time.sleep(ms/1000), raising=False)
    monkeypatch.setattr(os, "sync", lambda: None, raising=False)
    monkeypatch.setattr(os, "statvfs",
                        lambda path: (4096, 4096, 100000, 100000, 100000, 0, 0, 0, 255, 255),
                        raising=False)
    import cards_fast_config as config
    import cards_fast_core as core
    import recognize_cards_fast as recognize
    import capture_cards_fast as capture
    importlib.reload(core)
    importlib.reload(recognize)
    importlib.reload(capture)
    return config, core, recognize, capture


def card_scene(rank="10", dx=0, dy=0, angle=0):
    a = np.full((240, 320, 3), 240, np.uint8)
    cv2.putText(a, rank, (94, 116), cv2.FONT_HERSHEY_SIMPLEX, 2.1, (25, 25, 25), 5, cv2.LINE_AA)
    cv2.fillPoly(a, [np.array([[130, 155], [149, 177], [130, 199], [111, 177]])], (20, 20, 210))
    matrix = cv2.getRotationMatrix2D((130, 125), angle, 1)
    matrix[:, 2] += [dx, dy]
    return HostImage(cv2.warpAffine(a, matrix, (320, 240), borderValue=(240, 240, 240)))


def test_normalize_translation_and_ten_keeps_both_digits(modules):
    C, V, R, capture = modules
    first, info = V.locate(card_scene(), "rank", R.RECOGNITION_VISION)
    shifted, info2 = V.locate(card_scene(dx=5, dy=4), "rank", R.RECOGNITION_VISION)
    assert len(first) == len(shifted) == 1
    assert first[0]["box"][2] > 60  # Both 1 and 0, not a single digit.
    a = V.normalize(first[0]["mask"], "rank")
    b = V.normalize(shifted[0]["mask"], "rank")
    assert a.get_similarity(b).mean > 0.98
    assert (a.width(), a.height()) == C.PATCH_SIZES["rank"]


def test_rotation_search_recovers_a_small_tilt(modules):
    C, V, R, capture = modules
    canonical = V.locate(card_scene("A"), "rank", R.RECOGNITION_VISION)[0][0]
    tilted = V.locate(card_scene("A", angle=6), "rank", R.RECOGNITION_VISION)[0][0]
    template = V.normalize(canonical["mask"], "rank")
    scores = {a: V.normalize(tilted["mask"], "rank", a).get_similarity(template).mean
              for a in R.COARSE_ANGLES}
    assert max(scores, key=scores.get) == -6
    assert scores[-6] > scores[0] + 0.04


def test_blank_and_clipped_symbols_are_not_templates(modules):
    C, V, R, capture = modules
    blank = HostImage(np.full((240, 320, 3), 230, np.uint8))
    assert not V.locate(blank, "rank", R.RECOGNITION_VISION)[0]
    assert not V.locate(card_scene(dx=-35), "rank", R.RECOGNITION_VISION)[0]


def test_class_margin_aggregates_duplicate_label(modules):
    C, V, R, capture = modules
    patches = [HostImage(np.full((48, 32), n, np.uint8)) for n in (120, 120, 0)]
    records = {}
    V.score_patch(patches[0], [("A", patches[0], "A0"), ("A", patches[1], "A1"),
                              ("K", patches[2], "K0")], records, (90, 30, 60, 90), 0)
    best = V.best_group(records, "rank", R.RECOGNITION_VISION)
    assert best["label"] == "A" and best["accepted"]
    assert best["margin"] > 0.5


def test_color_needs_positive_red_or_black_evidence(modules):
    C, V, R, capture = modules
    red = card_scene()
    box = V.locate(red, "suit", R.RECOGNITION_VISION)[0][0]["box"]
    assert V.color_evidence(red, box)["color"] == "red"
    blank = HostImage(np.full((240, 320, 3), 240, np.uint8))
    assert V.color_evidence(blank, box)["color"] is None


def record(label, score, box):
    return {"label": label, "score": score, "box": box, "angle": 0, "template": "test"}


def test_normal_vs_back_conflict_is_unknown(modules):
    C, V, R, capture = modules
    scene = card_scene("A")
    rb = V.locate(scene, "rank", R.RECOGNITION_VISION)[0][0]["box"]
    sb = V.locate(scene, "suit", R.RECOGNITION_VISION)[0][0]["box"]
    records = {"rank": {"A": record("A", .91, rb)},
               "suit": {"diamond": record("diamond", .91, sb)}, "joker": {},
               "back": {"back": record("back", .97, (95, 45, 80, 120))}}
    assert V.decide(records, scene, R.RECOGNITION_VISION)["reason"] == "SCENE_CONFLICT"
    records["back"] = {}
    assert V.decide(records, scene, R.RECOGNITION_VISION)["label"] == "diamond_A"


def test_joker_without_color_is_not_black_by_default(modules):
    C, V, R, capture = modules
    blank = HostImage(np.full((240, 320, 3), 240, np.uint8))
    records = {"rank": {}, "suit": {}, "back": {},
               "joker": {"joker": record("joker", .98, (100, 30, 40, 90))}}
    assert V.decide(records, blank, R.RECOGNITION_VISION)["label"] == "UNKNOWN"


def test_bank_rejects_mismatched_pipeline_and_duplicate_overflow(modules, monkeypatch, tmp_path):
    C, V, R, capture = modules
    monkeypatch.setattr(C, "ROOT", str(tmp_path))
    monkeypatch.setattr(C, "USE_SD_CARD", False)
    V.ensure_storage()
    camera = {"exposure_us": 1000, "gain_db": 1, "rgb_gain_db": [1, 2, 3]}
    base = tmp_path / "templates/rank/A_0"
    HostImage(32, 48).save(str(base) + ".pgm")
    V.write_json(str(base) + ".json", {"group": "rank", "label": "A", "signature": "old", "camera": camera})
    with pytest.raises(ValueError, match="mismatch"):
        V.load_bank(camera)
    meta = {"group": "rank", "label": "A", "signature": V.signature(), "camera": camera}
    V.write_json(str(base) + ".json", meta)
    assert len(V.load_bank(camera)["rank"]) == 1
    for i in range(1, capture.MAX_TEMPLATES_PER_LABEL+1):
        base = tmp_path / ("templates/rank/A_%d" % i)
        HostImage(32, 48).save(str(base)+".pgm")
        V.write_json(str(base)+".json", meta)
    with pytest.raises(ValueError, match="Too many"):
        V.load_bank(camera, max_templates_per_label=capture.MAX_TEMPLATES_PER_LABEL)


def test_uart_handles_partial_writes_and_waits_for_txdone(modules):
    C, V, R, capture = modules
    class UART:
        def __init__(self):
            self.data = b""
            self.polls = 0
        def write(self, data):
            self.data += data[:3]
            return min(3, len(data))
        def txdone(self):
            self.polls += 1
            return self.polls >= 2
    uart = UART()
    R.send_result(uart, "spade_A")
    assert uart.data == b"RESULT:spade_A\r\n" and uart.polls == 2


def test_uart_timeout_does_not_claim_success(modules, monkeypatch):
    C, V, R, capture = modules
    clock = iter(range(0, 1000, 10))
    monkeypatch.setattr(time, "ticks_ms", lambda: next(clock))
    with pytest.raises(OSError, match="timeout"):
        R.send_result(types.SimpleNamespace(write=lambda data: None), "UNKNOWN")


def test_expired_budget_never_captures_or_emits_success(modules):
    C, V, R, capture = modules
    class NoCamera:
        def snapshot(self):
            raise AssertionError("expired request must not start capture")
    result, profile = R.recognize_trigger(NoCamera(), {}, time.ticks_ms()-2000)
    assert result["label"] == "UNKNOWN" and result["reason"] == "TIMEOUT"
    assert profile["attempts"] == 0


def test_real_old_rank_crops_have_contrast_but_need_new_metadata(modules, monkeypatch):
    C, V, R, capture = modules
    paths = list((ROOT / "templates/rank").glob("*.pgm"))
    if not paths:
        pytest.skip("Old sample crops are optional host fixtures")
    settings = dict(R.RECOGNITION_VISION)
    settings["rois"] = dict(R.RECOGNITION_ROIS, rank=(0, 0, 80, 120))
    for path in paths:
        mask, info = V.make_mask(HostImage(str(path)), "rank", settings)
        assert mask is not None, path.name
        assert info["contrast"] >= settings["min_contrast"]


def test_capture_load_recognize_round_trip(modules, monkeypatch, tmp_path):
    C, V, R, capture = modules
    monkeypatch.setattr(C, "ROOT", str(tmp_path))
    monkeypatch.setattr(C, "USE_SD_CARD", False)
    monkeypatch.setattr(capture, "NORMAL_SAVE_GROUPS", ("rank", "suit"))
    monkeypatch.setattr(capture, "RANK_LABEL", "A")
    monkeypatch.setattr(capture, "SUIT_LABEL", "diamond")
    V.ensure_storage()
    camera = {"exposure_us": 1000, "gain_db": 1, "rgb_gain_db": [1, 2, 3]}
    frame = card_scene("A")
    patches, boxes, info = capture.selection(frame)
    assert capture.save_capture(frame, camera, patches, boxes, info) == "SAVED"
    assert len(list(tmp_path.rglob("*.pgm"))) == 2
    assert not list(tmp_path.rglob("*.ppm"))
    assert not list(tmp_path.rglob("*.bmp"))
    bank = V.load_bank(camera)
    records = {k: {} for k in bank}
    result = V.process_pass(card_scene("A", dx=4, dy=3), bank, records,
                            R.COARSE_ANGLES, R.RECOGNITION_VISION)
    assert result["label"] == "diamond_A"


def test_validation_mode_and_rejection_write_nothing(modules, monkeypatch, tmp_path):
    C, V, R, capture = modules
    monkeypatch.setattr(C, "ROOT", str(tmp_path))
    monkeypatch.setattr(C, "USE_SD_CARD", False)
    monkeypatch.setattr(capture, "SAVE_TEMPLATES", False)
    V.ensure_storage()
    frame = HostImage(np.full((240, 320, 3), 230, np.uint8))
    patches, boxes, info = capture.selection(frame)
    assert capture.save_capture(frame, {}, patches, boxes, info) == "VALID"
    assert not list(tmp_path.rglob("*.pgm"))
    assert not list(tmp_path.rglob("*.json"))
    monkeypatch.setattr(capture, "SAVE_TEMPLATES", True)
    assert capture.save_capture(frame, {}, patches, boxes, info) == "REJECTED"
    assert not list(tmp_path.rglob("*.pgm"))
    assert not list(tmp_path.rglob("*.json"))


def test_atomic_template_commit_and_temp_files_are_ignored(modules, monkeypatch, tmp_path):
    C, V, R, capture = modules
    monkeypatch.setattr(C, "ROOT", str(tmp_path))
    monkeypatch.setattr(C, "USE_SD_CARD", False)
    V.ensure_storage()
    camera = {"exposure_us": 1000, "gain_db": 1, "rgb_gain_db": [1, 2, 3]}
    base = str(tmp_path / "templates/rank/A_00")
    metadata = {"group": "rank", "label": "A", "signature": V.signature(), "camera": camera}
    final_path = V.save_template_atomic(HostImage(32, 48), base, metadata)
    assert final_path.endswith("A_00.pgm")
    assert Path(base + ".pgm").exists() and Path(base + ".json").exists()
    assert not Path(base + ".tmp.pgm").exists() and not Path(base + ".tmp.json").exists()
    HostImage(32, 48).save(str(tmp_path / "templates/rank/K_00.tmp.pgm"))
    V.write_json(str(tmp_path / "templates/rank/K_00.tmp.json"),
                 dict(metadata, label="K"))
    bank = V.load_bank(camera)
    assert [entry[0] for entry in bank["rank"]] == ["A"]


def test_sd_selection_requires_sd_root(modules, monkeypatch, tmp_path):
    C, V, R, capture = modules
    monkeypatch.setattr(C, "ROOT", str(tmp_path))
    monkeypatch.setattr(C, "USE_SD_CARD", True)
    with pytest.raises(ValueError, match="ROOT is not under /sdcard"):
        V.ensure_storage()


def test_retry_captures_fresh_frame_and_returns_single_final_result(modules, monkeypatch):
    C, V, R, capture = modules
    monkeypatch.setattr(R, "REFINE_ANGLES", ())
    monkeypatch.setattr(R, "REFINE_ADAPTIVE_OFFSETS", ())
    monkeypatch.setattr(R, "RETRY_SETTLE_MS", 0)
    class Camera:
        def __init__(self):
            self.calls = 0
        def snapshot(self):
            self.calls += 1
            return self.calls
    seen = []
    def process(frame, bank, records, angles, settings, **kwargs):
        seen.append(frame)
        return {"label": "UNKNOWN" if len(seen) == 1 else "spade_A",
                "reason": "test", "groups": {}, "locate_ms": 1, "pass_ms": 5}
    monkeypatch.setattr(V, "process_pass", process)
    camera = Camera()
    result, profile = R.recognize_trigger(camera, {}, time.ticks_ms())
    assert seen == [2, 3]  # Frame 1 discarded, frame 3 really re-acquired.
    assert profile["attempts"] == 2 and result["label"] == "spade_A"


def test_native_call_overrun_discards_apparent_success(modules, monkeypatch):
    C, V, R, capture = modules
    clock = [100]
    monkeypatch.setattr(time, "ticks_ms", lambda: clock[0])
    def process(*args, **kwargs):
        clock[0] += R.RESULT_BUDGET_MS + 10
        return {"label": "spade_A", "reason": "OK", "groups": {}, "locate_ms": 1, "pass_ms": 999}
    monkeypatch.setattr(V, "process_pass", process)
    camera = types.SimpleNamespace(snapshot=lambda: object())
    result, profile = R.recognize_trigger(camera, {}, 100)
    assert result["label"] == "UNKNOWN" and result["reason"] == "TIMEOUT"
