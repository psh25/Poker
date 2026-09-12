"""OpenMV H7 Plus standalone card recognizer.

Copy this file to /flash/main.py. Keep cards_fast_config.py and
cards_hybrid_core.py in /flash, and keep the template bank on the SD card at
/sdcard/cards_fast_v1/templates.

P6 falling edge -> exactly one triggered snapshot -> recognition on that frame.
UART3: P4 TX, P5 RX, 115200 8N1. One result line is emitted per accepted
trigger: RESULT:<label>\r\n. The same result and compact diagnostics are also
printed through USB CDC.
"""
import gc
import time
from machine import Pin, UART
import cards_fast_config as C
import cards_hybrid_core as V


# ----------------------------- Deployment settings -----------------------------
TEMPLATE_ROOT = "/sdcard/cards_fast_v1"
TRIGGER_PIN = "P6"
TRIGGER_FILTER_MS = 2
REARM_HIGH_MS = 5

UART_ID = 3
UART_BAUD = 115200
UART_TIMEOUT_MS = 25

RESULT_BUDGET_MS = 900
OUTPUT_RESERVE_MS = 35
STRICT_TEMPLATE_COVERAGE = True
MAX_TEMPLATES_PER_LABEL = 2

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
    "accept_score": {"rank": 0.80, "suit": 0.80,
                     "joker": 0.84, "back": 0.86},
    "accept_margin": {"rank": 0.05, "suit": 0.015,
                      "joker": 0.0, "back": 0.0},
    "scene_margin": 0.035,
    "red_joker_label": "joker_big",
    "black_joker_label": "joker_small",
}
# -------------------------------------------------------------------------------


def compact_reason(result):
    """Produce one short USB-only explanation for UNKNOWN."""
    reason = result.get("reason", "UNKNOWN")
    if reason in ("TIMEOUT", "NO_RESULT"):
        return reason
    groups = result.get("groups", {})
    diagnostics = result.get("diagnostics", {})
    parts = []
    if diagnostics.get("layout_pairs") == 0:
        rank_count = diagnostics.get("rank", {}).get("valid_candidates", 0)
        suit_count = diagnostics.get("suit", {}).get("valid_candidates", 0)
        parts.append("NO_LAYOUT=r%d,s%d" % (rank_count, suit_count))
    for group in ("rank", "suit", "joker", "back"):
        item = groups.get(group, {})
        label = item.get("label")
        if label is None:
            continue
        score = item.get("score", -1.0)
        score_limit = RECOGNITION_VISION["accept_score"][group]
        margin = item.get("margin", 0.0)
        margin_limit = RECOGNITION_VISION["accept_margin"][group]
        if score < score_limit:
            parts.append("%s_SCORE=%.3f" % (group.upper(), score))
        elif margin < margin_limit:
            parts.append("%s_MARGIN=%.3f" % (group.upper(), margin))
    return (";".join(parts) if parts else reason)[:200]


def uart_write_line(uart, line):
    """Write and drain one complete CRLF-terminated UART record."""
    payload = (line + "\r\n").encode()
    start = time.ticks_ms()
    sent = 0
    while sent < len(payload):
        count = uart.write(payload[sent:])
        if count:
            sent += count
        elif time.ticks_diff(time.ticks_ms(), start) >= UART_TIMEOUT_MS:
            raise OSError("UART write timeout")
        else:
            time.sleep_ms(1)
    uart.flush()
    if time.ticks_diff(time.ticks_ms(), start) >= UART_TIMEOUT_MS:
        raise OSError("UART drain timeout")


def recognize_once(cam, bank, trigger_ms):
    """Take exactly one trigger image and refine only that same image."""
    budget = V.Budget(trigger_ms, RESULT_BUDGET_MS - OUTPUT_RESERVE_MS)
    result = {"label": "UNKNOWN", "reason": "NO_RESULT", "groups": {}}
    capture_start = time.ticks_ms()
    try:
        budget.check()
        frame = cam.snapshot()  # The only snapshot caused by this trigger.
        budget.check()
        capture_ms = time.ticks_diff(time.ticks_ms(), capture_start)
        records = {key: {} for key in ("rank", "suit", "joker", "back")}

        result = V.process_pass(frame, bank, records, COARSE_ANGLES,
                                RECOGNITION_VISION, budget=budget)
        coarse_ms = result["pass_ms"]
        if (result["label"] == "UNKNOWN" and REFINE_ANGLES
                and budget.remaining() > max(80, coarse_ms * 2)):
            result = V.process_pass(frame, bank, records, REFINE_ANGLES,
                                    RECOGNITION_VISION, budget=budget)
        if result["label"] == "UNKNOWN":
            for offset in REFINE_OTSU_OFFSETS:
                if budget.remaining() <= max(100, coarse_ms * 2):
                    break
                result = V.process_pass(frame, bank, records, COARSE_ANGLES,
                                        RECOGNITION_VISION, offset, budget)
                if result["label"] != "UNKNOWN":
                    break
        budget.check()
    except V.BudgetExceeded:
        result = {"label": "UNKNOWN", "reason": "TIMEOUT", "groups": {}}
        capture_ms = time.ticks_diff(time.ticks_ms(), capture_start)
    return result, capture_ms


def output_result(uart, result, trigger_ms, capture_ms):
    """UART gets one machine record; USB gets the same result plus diagnostics."""
    if time.ticks_diff(time.ticks_ms(), trigger_ms) >= (
            RESULT_BUDGET_MS - OUTPUT_RESERVE_MS):
        result["label"] = "UNKNOWN"
        result["reason"] = "TIMEOUT"

    line = "RESULT:" + result.get("label", "UNKNOWN")
    uart_error = None
    try:
        uart_write_line(uart, line)
    except Exception as error:
        uart_error = repr(error)

    # print() is routed to USB CDC/REPL when a computer is connected.
    print(line)
    if result.get("label") == "UNKNOWN":
        print("REASON:" + compact_reason(result))
    if uart_error is not None:
        print("UART_ERROR:" + uart_error)
    print("TIME_MS:%d CAPTURE_MS:%d" % (
        time.ticks_diff(time.ticks_ms(), trigger_ms), capture_ms))


def run():
    # Force this deployment entry to use the SD bank, regardless of the
    # development value currently stored in cards_fast_config.py.
    C.USE_SD_CARD = True
    C.ROOT = TEMPLATE_ROOT
    V.validate_config(RECOGNITION_VISION)
    V.ensure_storage()

    uart = UART(UART_ID, baudrate=UART_BAUD, bits=8, parity=None, stop=1,
                timeout=UART_TIMEOUT_MS, timeout_char=5)
    cam, camera, leds = V.start_camera(False)
    bank = V.load_bank(STRICT_TEMPLATE_COVERAGE, MAX_TEMPLATES_PER_LABEL)

    trigger = Pin(TRIGGER_PIN, Pin.IN, Pin.PULL_UP)
    state = [False, 0, False]  # pending, falling-edge time, armed

    def on_falling(unused_pin):
        # IRQ only latches the request. Camera, UART and allocation stay in the
        # normal interpreter context.
        if state[2] and not state[0]:
            state[0] = True
            state[1] = time.ticks_ms()
            state[2] = False

    trigger.irq(handler=on_falling, trigger=Pin.IRQ_FALLING)
    print("READY:P6_ACTIVE_LOW UART3_115200 SD_TEMPLATES")
    print("CAMERA:", camera)

    high_since = None
    gc.collect()
    while True:
        now = time.ticks_ms()
        if state[0] and time.ticks_diff(now, state[1]) >= TRIGGER_FILTER_MS:
            trigger_ms = state[1]
            state[0] = False
            try:
                result, capture_ms = recognize_once(cam, bank, trigger_ms)
            except Exception as error:
                result = {"label": "ERROR", "reason": repr(error), "groups": {}}
                capture_ms = -1
            output_result(uart, result, trigger_ms, capture_ms)
            high_since = None
            gc.collect()

        # A new trigger is accepted only after P6 has returned high stably.
        if not state[2] and not state[0]:
            if trigger.value() == 1:
                if high_since is None:
                    high_since = now
                elif time.ticks_diff(now, high_since) >= REARM_HIGH_MS:
                    state[2] = True
            else:
                high_since = None
        time.sleep_ms(1)


def main():
    try:
        run()
    except Exception as error:
        # Startup failures still remain visible over USB without an IDE.
        print("FATAL:" + repr(error))
        try:
            uart = UART(UART_ID, baudrate=UART_BAUD, bits=8,
                        parity=None, stop=1, timeout=UART_TIMEOUT_MS)
            uart_write_line(uart, "RESULT:ERROR")
        except Exception as uart_error:
            print("UART_FATAL:" + repr(uart_error))
        while True:
            time.sleep_ms(1000)


if __name__ == "__main__":
    main()
