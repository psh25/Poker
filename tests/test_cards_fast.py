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

    def get_statistics(self):
        return types.SimpleNamespace(stdev=float(self.a.std()))

    def binary(self, thresholds):
        lo, hi = thresholds[0]
        self.a = ((self.a >= lo) & (self.a <= hi)).astype(np.uint8) * 255
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
    first, info = V.locate(card_scene(), "rank")
    shifted, info2 = V.locate(card_scene(dx=5, dy=4), "rank")
    assert len(first) == len(shifted) == 1
    assert first[0]["box"][2] > 60  # Both 1 and 0, not a single digit.
    a = V.normalize(first[0]["mask"], "rank")
    b = V.normalize(shifted[0]["mask"], "rank")
    assert a.get_similarity(b).mean > 0.98
    assert (a.width(), a.height()) == C.PATCH_SIZES["rank"]


def test_rotation_search_recovers_a_small_tilt(modules):
    C, V, R, capture = modules
    canonical = V.locate(card_scene("A"), "rank")[0][0]
    tilted = V.locate(card_scene("A", angle=6), "rank")[0][0]
    template = V.normalize(canonical["mask"], "rank")
    scores = {a: V.normalize(tilted["mask"], "rank", a).get_similarity(template).mean
              for a in C.COARSE_ANGLES}
    assert max(scores, key=scores.get) == -6
    assert scores[-6] > scores[0] + 0.04


def test_blank_and_clipped_symbols_are_not_templates(modules):
    C, V, R, capture = modules
    blank = HostImage(np.full((240, 320, 3), 230, np.uint8))
    assert not V.locate(blank, "rank")[0]
    assert not V.locate(card_scene(dx=-35), "rank")[0]


def test_class_margin_aggregates_duplicate_label(modules):
    C, V, R, capture = modules
    patches = [HostImage(np.full((48, 32), n, np.uint8)) for n in (120, 120, 0)]
    records = {}
    V.score_patch(patches[0], [("A", patches[0], "A0"), ("A", patches[1], "A1"),
                              ("K", patches[2], "K0")], records, (90, 30, 60, 90), 0)
    best = V.best_group(records, "rank")
    assert best["label"] == "A" and best["accepted"]
    assert best["margin"] > 0.5


def test_color_needs_positive_red_or_black_evidence(modules):
    C, V, R, capture = modules
    red = card_scene()
    box = V.locate(red, "suit")[0][0]["box"]
    assert V.color_evidence(red, box)["color"] == "red"
    blank = HostImage(np.full((240, 320, 3), 240, np.uint8))
    assert V.color_evidence(blank, box)["color"] is None


def record(label, score, box):
    return {"label": label, "score": score, "box": box, "angle": 0, "template": "test"}


def test_normal_vs_back_conflict_is_unknown(modules):
    C, V, R, capture = modules
    scene = card_scene("A")
    rb = V.locate(scene, "rank")[0][0]["box"]
    sb = V.locate(scene, "suit")[0][0]["box"]
    records = {"rank": {"A": record("A", .91, rb)},
               "suit": {"diamond": record("diamond", .91, sb)}, "joker": {},
               "back": {"back": record("back", .97, (95, 45, 80, 120))}}
    assert V.decide(records, scene)["reason"] == "SCENE_CONFLICT"
    records["back"] = {}
    assert V.decide(records, scene)["label"] == "diamond_A"


def test_joker_without_color_is_not_black_by_default(modules):
    C, V, R, capture = modules
    blank = HostImage(np.full((240, 320, 3), 240, np.uint8))
    records = {"rank": {}, "suit": {}, "back": {},
               "joker": {"joker": record("joker", .98, (100, 30, 40, 90))}}
    assert V.decide(records, blank)["label"] == "UNKNOWN"


def test_bank_rejects_mismatched_pipeline_and_duplicate_overflow(modules, monkeypatch, tmp_path):
    C, V, R, capture = modules
    monkeypatch.setattr(C, "ROOT", str(tmp_path))
    monkeypatch.setattr(C, "STRICT_TEMPLATE_COVERAGE", False)
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
    for i in range(1, C.MAX_TEMPLATES_PER_LABEL+1):
        base = tmp_path / ("templates/rank/A_%d" % i)
        HostImage(32, 48).save(str(base)+".pgm")
        V.write_json(str(base)+".json", meta)
    with pytest.raises(ValueError, match="Too many"):
        V.load_bank(camera)


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
    monkeypatch.setattr(C, "SEARCH_ROIS", dict(C.SEARCH_ROIS, rank=(0, 0, 80, 120)))
    for path in paths:
        mask, info = V.make_mask(HostImage(str(path)), "rank")
        assert mask is not None, path.name
        assert info["contrast"] >= C.MIN_CONTRAST


def test_capture_load_recognize_round_trip(modules, monkeypatch, tmp_path):
    C, V, R, capture = modules
    monkeypatch.setattr(C, "ROOT", str(tmp_path))
    monkeypatch.setattr(C, "STRICT_TEMPLATE_COVERAGE", False)
    monkeypatch.setattr(capture, "NORMAL_SAVE_GROUPS", ("rank", "suit"))
    monkeypatch.setattr(capture, "SUIT_LABEL", "diamond")
    V.ensure_storage()
    camera = {"exposure_us": 1000, "gain_db": 1, "rgb_gain_db": [1, 2, 3]}
    frame = card_scene("A")
    patches, boxes, info = capture.selection(frame)
    assert capture.save_capture(frame, camera, patches, boxes, info) == "SAVED"
    assert len(list(tmp_path.rglob("*.pgm"))) == 2
    assert len(list((tmp_path / "samples").glob("*.ppm"))) == 1
    bank = V.load_bank(camera)
    records = {k: {} for k in bank}
    result = V.process_pass(card_scene("A", dx=4, dy=3), bank, records, C.COARSE_ANGLES)
    assert result["label"] == "diamond_A"


def test_validation_mode_preserves_failed_frame_without_templates(modules, monkeypatch, tmp_path):
    C, V, R, capture = modules
    monkeypatch.setattr(C, "ROOT", str(tmp_path))
    monkeypatch.setattr(capture, "SAVE_TEMPLATES", False)
    V.ensure_storage()
    frame = HostImage(np.full((240, 320, 3), 230, np.uint8))
    patches, boxes, info = capture.selection(frame)
    capture.save_capture(frame, {}, patches, boxes, info)
    assert not list(tmp_path.rglob("*.pgm"))
    sample = json.loads(next((tmp_path / "samples").glob("*.json")).read_text())
    assert sample["validation_only"] and sample["diagnostics"]["rank"]["reason"] == "LOW_CONTRAST"


def test_retry_captures_fresh_frame_and_returns_single_final_result(modules, monkeypatch):
    C, V, R, capture = modules
    monkeypatch.setattr(C, "REFINE_ANGLES", ())
    monkeypatch.setattr(C, "REFINE_THRESHOLD_OFFSETS", ())
    monkeypatch.setattr(C, "RETRY_SETTLE_MS", 0)
    class Camera:
        def __init__(self):
            self.calls = 0
        def snapshot(self):
            self.calls += 1
            return self.calls
    seen = []
    def process(frame, bank, records, angles, **kwargs):
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
        clock[0] += C.RESULT_BUDGET_MS + 10
        return {"label": "spade_A", "reason": "OK", "groups": {}, "locate_ms": 1, "pass_ms": 999}
    monkeypatch.setattr(V, "process_pass", process)
    camera = types.SimpleNamespace(snapshot=lambda: object())
    result, profile = R.recognize_trigger(camera, {}, 100)
    assert result["label"] == "UNKNOWN" and result["reason"] == "TIMEOUT"
