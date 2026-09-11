"""OpenMV H7 Plus triggered card recognition, no CNN, no no-card class.

Deploy with cards_hybrid_core.py, cards_fast_config.py and newly captured bank.
P9 falling edge: fresh capture -> native SSIM -> same-frame refinement -> at
most one retry. UART3 P4 TX / P5 RX, 115200 8N1: RESULT:<label>\r\n.
UNKNOWN means insufficient evidence, NEVER "no card". Diagnostics use USB.
The 900 ms deadline is cooperative: an individual native call cannot be
interrupted. Measure PROFILE total_ms on the actual H7 Plus before acceptance.
"""
import gc
import time
from machine import Pin, UART
import cards_fast_config as C
import cards_hybrid_core as V

# ---------------- Runtime-only settings; tune without editing config/core ----------------
# Wider than capture ROIs so the complete symbols remain visible throughout
# the measured mechanical displacement range.
RECOGNITION_ROIS = {
    "rank": (100, 25, 100, 155),
    "suit": (100, 130, 100, 110),
    "joker": (110, 25, 80, 150),
    "back": (160, 120, 40, 40),
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
    "accept_margin": {"rank": 0.05, "suit": 0.05, "joker": 0.0, "back": 0.0},
    "scene_margin": 0.035,
    "red_joker_label": "joker_big",
    "black_joker_label": "joker_small",
}

STRICT_TEMPLATE_COVERAGE = True
MAX_TEMPLATES_PER_LABEL = 2
UART_ENABLED = True
UART_ID = 3  # H7 Plus P4=TX, P5=RX; common GND with receiver.
UART_BAUD = 115200
UART_TIMEOUT_MS = 25
RESULT_BUDGET_MS = 900
OUTPUT_RESERVE_MS = 35
MAX_ATTEMPTS = 1  # Current requirement: no recapture; failure emits UNKNOWN.
RETRY_SETTLE_MS = 20
DISCARD_AFTER_TRIGGER = 1
PRINT_PROFILE = True
DEBUG_DRAW_TRIGGER_FRAME = True  # Draw only after UART output/timing completes.
DEBUG_ROI_COLORS = {
    "rank": (255, 0, 0),
    "suit": (0, 0, 255),
    "joker": (255, 0, 255),
    "back": (0, 255, 255),
}
DEBUG_CANDIDATE_COLOR = (255, 255, 0)
DEBUG_ACCEPTED_COLOR = (0, 255, 0)
# --------------------------------------------------------------------------------


def send_result(uart, label):
    payload = ("RESULT:" + label + "\r\n").encode()
    if uart is None:
        print(payload.decode().strip())
        return
    start = time.ticks_ms()
    sent = 0
    while sent < len(payload):
        count = uart.write(payload[sent:])
        if count:
            sent += count
        if time.ticks_diff(time.ticks_ms(), start) >= UART_TIMEOUT_MS:
            raise OSError("UART write timeout")
        if not count:
            time.sleep_ms(1)
    # txdone means the final byte has actually left the UART, not only queued.
    while not uart.txdone():
        if time.ticks_diff(time.ticks_ms(), start) >= UART_TIMEOUT_MS:
            raise OSError("UART drain timeout")
        time.sleep_ms(1)


def draw_trigger_debug(frame, result, sequence, profile):
    """Overlay the already-processed trigger frame for OpenMV IDE display."""
    if not DEBUG_DRAW_TRIGGER_FRAME or frame is None:
        return

    # Search windows are drawn after recognition, so these pixels never enter
    # LAB location, local Otsu, SSIM, or the UART timing measurement.
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
        score = info.get("score")
        if score is not None:
            text = "%s %.2f" % (group, score)
            frame.draw_string((max(0, box[0]), max(14, box[1] - 11)),
                              text, color=color)

    total_ms = profile.get("total_ms")
    title = "#%d %s" % (sequence, result.get("label", "UNKNOWN"))
    if total_ms is not None:
        title += " %dms" % total_ms
    frame.draw_string((2, 2), title, color=(255, 255, 255))
    frame.draw_string((2, 13), result.get("reason", "")[:38],
                      color=(255, 255, 255))


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
            frame = cam.snapshot()  # Acquired AFTER trigger/debounce, not preview.
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


def main():
    V.validate_config(RECOGNITION_VISION)
    V.ensure_storage()
    cam, camera, leds = V.start_camera(False)
    bank = V.load_bank(camera, STRICT_TEMPLATE_COVERAGE, MAX_TEMPLATES_PER_LABEL)
    uart = UART(UART_ID, baudrate=UART_BAUD, bits=8, parity=None, stop=1,
                timeout=UART_TIMEOUT_MS) if UART_ENABLED else None
    if uart is not None and not hasattr(uart, "txdone"):
        raise RuntimeError("Firmware UART.txdone() required to measure complete transmission")
    pin = Pin(C.TRIGGER_PIN, Pin.IN, Pin.PULL_UP)
    # IRQ only latches timestamp. Never perform camera/I/O work in interrupt.
    state = [0, 0, 0]  # pending, first falling-edge time, armed

    def on_falling(unused):
        if state[2] and not state[0]:
            state[1] = time.ticks_ms()
            state[0] = 1
            state[2] = 0

    pin.irq(trigger=Pin.IRQ_FALLING, handler=on_falling)
    print("READY: P9 -> GND; UART3 P4 TX/P5 RX; no no-card detection")
    print("Camera:", camera)
    print("SSIM thresholds are provisional. New templates only; profile on hardware.")
    print("IDE trigger-frame overlay:", DEBUG_DRAW_TRIGGER_FRAME)
    sequence = 0
    high_since = None
    gc.collect()
    while True:
        now = time.ticks_ms()
        if state[0]:
            # Button must still be low after debounce; bounce is discarded.
            if time.ticks_diff(now, state[1]) < C.BUTTON_DEBOUNCE_MS:
                time.sleep_ms(2)
                continue
            trigger_ms = state[1]
            if pin.value() == 0:
                sequence += 1
                debug_frame = None
                try:
                    result, profile, debug_frame = recognize_trigger(cam, bank, trigger_ms)
                except Exception as error:
                    result = {"label": "ERROR", "reason": repr(error), "groups": {}}
                    profile = {}
                # Ensure no success label is emitted after the processing deadline.
                if time.ticks_diff(time.ticks_ms(), trigger_ms) >= RESULT_BUDGET_MS - OUTPUT_RESERVE_MS:
                    result["label"], result["reason"] = "UNKNOWN", "TIMEOUT"
                tx_start = time.ticks_ms()
                try:
                    send_result(uart, result["label"])
                    profile["uart_ms"] = time.ticks_diff(time.ticks_ms(), tx_start)
                    profile["total_ms"] = time.ticks_diff(time.ticks_ms(), trigger_ms)
                    profile["over_budget"] = profile["total_ms"] > RESULT_BUDGET_MS
                except Exception as error:
                    profile["transport_error"] = repr(error)
                # USB printing is outside the UART result critical path.
                if PRINT_PROFILE:
                    print("RESULT:" + result["label"] if uart is not None else "RESULT_SENT")
                    print("PROFILE", sequence, profile)
                    print("DECISION", result["reason"], result["groups"])
                draw_trigger_debug(debug_frame, result, sequence, profile)
                gc.collect()  # Idle cleanup before re-arming; not per template.
            state[0] = 0
            high_since = None
        if not state[2] and not state[0]:
            if pin.value() == 1:
                if high_since is None:
                    high_since = time.ticks_ms()
                elif time.ticks_diff(time.ticks_ms(), high_since) >= C.BUTTON_DEBOUNCE_MS:
                    state[2] = 1
            else:
                high_since = None
        time.sleep_ms(2)


if __name__ == "__main__":
    main()
