"""Host-only regression checks for the hybrid locator/canonicalizer."""
import copy
import importlib
import sys

import cv2
import numpy as np

from evaluate_pok_hybrid import HostImage, install_host_api


def load_modules():
    install_host_api()
    import cards_hybrid_core as core
    import capture_cards_hybrid as capture
    import recognize_cards_hybrid as recognize
    importlib.reload(core)
    importlib.reload(capture)
    importlib.reload(recognize)
    return core, capture, recognize


def test_widened_black_locator_and_relative_layout():
    V, Capture, unused = load_modules()
    a = np.full((240, 320, 3), (190, 225, 200), np.uint8)
    cv2.putText(a, "9", (125, 105), cv2.FONT_HERSHEY_SIMPLEX,
                2.0, (18, 28, 18), 5, cv2.LINE_AA)
    cv2.fillPoly(a, [np.array([[150, 145], [170, 170], [150, 198], [130, 170]])],
                 (18, 28, 18))
    # Unrelated centre artwork lies inside the broad search window.
    cv2.circle(a, (218, 165), 38, (18, 28, 18), -1)
    frame = HostImage(a)
    ranks, unused_info = V.locate(frame, "rank", Capture.CAPTURE_VISION)
    suits, unused_info = V.locate(frame, "suit", Capture.CAPTURE_VISION)
    pairs = V.layout_pairs(ranks, suits, Capture.CAPTURE_VISION, frame)
    assert pairs
    assert V.canonical_patch(frame, pairs[0][1]["box"], "rank",
                             Capture.CAPTURE_VISION) is not None


def test_joker_uses_complete_partial_word_and_color_presence():
    V, Capture, unused = load_modules()
    for bgr, expected in (((20, 20, 150), "red"), ((20, 30, 20), "black")):
        a = np.full((240, 320, 3), (205, 225, 205), np.uint8)
        for char, y in zip("JOK", (65, 112, 159)):
            cv2.putText(a, char, (135, y), cv2.FONT_HERSHEY_SIMPLEX,
                        1.35, bgr, 4, cv2.LINE_AA)
        frame = HostImage(a)
        candidates, info = V.locate(frame, "joker", Capture.CAPTURE_VISION)
        assert candidates, info
        assert V.color_evidence(frame, candidates[0]["box"],
                                Capture.CAPTURE_VISION)["color"] == expected


def test_recognizer_has_no_recapture_and_unique_template_limit():
    unused, unused_capture, Recognize = load_modules()
    assert Recognize.MAX_ATTEMPTS == 1
    assert Recognize.MAX_TEMPLATES_PER_LABEL == 2
    assert Recognize.COARSE_ANGLES == (0,)
