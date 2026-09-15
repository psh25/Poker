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
import os
import time
from machine import LED, Pin, UART
import cards_fast_config as C
import cards_hybrid_core as V


# ----------------------------- Deployment settings -----------------------------
TEMPLATE_ROOT = "/flash/cards_fast_v1"
TRIGGER_PIN = "P6"
TRIGGER_FILTER_MS = 2
REARM_HIGH_MS = 5

UART_ID = 3
# ESP32-S3 子板使用 Serial2：GPIO46/RX <- OpenMV P4/TX，
# GPIO10/TX -> OpenMV P5/RX；两板必须共地，使用3.3 V逻辑。
# 子板GPIO20 -> P6，空闲高、低脉冲200 ms；GPIO20与子板原生USB D+冲突。
# 参数依据 SubBoard/include/{app_config.h,pins_config.h}，详见通信协议文档。
UART_BAUD = 115200
UART_TIMEOUT_MS = 25
UART_RX_BUFFER_BYTES = 64
UART_MAX_LINE_BYTES = 32

BOOT_LED_MS = 1000
CAPTURE_LED_PRELIGHT_MS = 35

RESULT_BUDGET_MS = 900
# 子板在200 ms触发脉冲结束后才开始1200 ms结果等待计时；
# 本程序900 ms预算仍从P6下降沿计算，二者并非同一个计时起点。
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


def leds_on(leds):
    for led in leds:
        led.on()


def leds_off(leds):
    for led in leds:
        led.off()


def boot_led_signal(leds):
    leds_on(leds)
    try:
        time.sleep_ms(BOOT_LED_MS)
    finally:
        leds_off(leds)


def read_uart_lines(uart, state):
    """Return complete ASCII lines without blocking the recognition loop."""
    lines = []
    available = uart.any()
    if not available:
        return lines
    data = uart.read(available)
    if not data:
        return lines
    for value in data:
        if value == 10:  # LF completes one line.
            if state["overflow"]:
                lines.append(None)
            else:
                raw = bytes(state["buffer"])
                if raw.endswith(b"\r"):
                    raw = raw[:-1]
                try:
                    lines.append(raw.decode("ascii"))
                except Exception:
                    lines.append(None)
            state["buffer"] = bytearray()
            state["overflow"] = False
        elif not state["overflow"]:
            if len(state["buffer"]) >= UART_MAX_LINE_BYTES:
                state["overflow"] = True
                state["buffer"] = bytearray()
            else:
                state["buffer"].append(value)
    return lines


def _remove_if_present(path):
    if V.exists(path):
        os.remove(path)


def recover_camera_config():
    """Recover an interrupted atomic camera-config replacement."""
    path = C.CAMERA_CONFIG_PATH
    new_path = path + ".new"
    backup_path = path + ".bak"
    if V.exists(path):
        _remove_if_present(new_path)
        _remove_if_present(backup_path)
    elif V.exists(new_path):
        os.rename(new_path, path)
        _remove_if_present(backup_path)
    elif V.exists(backup_path):
        os.rename(backup_path, path)


def save_camera_config(settings):
    """Replace camera.json with recovery points for unexpected power loss."""
    path = C.CAMERA_CONFIG_PATH
    new_path = path + ".new"
    backup_path = path + ".bak"
    _remove_if_present(new_path)
    _remove_if_present(backup_path)
    V.write_json(new_path, settings)
    os.sync()
    if V.exists(path):
        os.rename(path, backup_path)
    try:
        os.rename(new_path, path)
        os.sync()
    except Exception:
        recover_camera_config()
        raise
    _remove_if_present(backup_path)
    os.sync()


def lock_camera(cam, settings):
    cam.auto_exposure(False, exposure_us=int(settings["exposure_us"]))
    cam.auto_gain(False, gain_db=settings["gain_db"])
    cam.auto_whitebal(False, rgb_gain_db=tuple(settings["rgb_gain_db"]))


def calibrate_camera(cam, camera, leds):
    """Run auto controls, persist the result, and leave controls locked."""
    previous = {"exposure_us": cam.exposure_us(),
                "gain_db": cam.gain_db(),
                "rgb_gain_db": list(cam.rgb_gain_db())}
    leds_on(leds)
    try:
        cam.auto_exposure(True)
        cam.auto_gain(True)
        cam.auto_whitebal(True)
        cam.snapshot(time=C.CAMERA_SETTLE_MS)
        settings = {"exposure_us": cam.exposure_us(),
                    "gain_db": cam.gain_db(),
                    "rgb_gain_db": list(cam.rgb_gain_db()),
                    "calibration_id": time.ticks_ms()}
        lock_camera(cam, settings)
        cam.snapshot(time=C.CAMERA_APPLY_SETTLE_MS)
        save_camera_config(settings)
        camera.clear()
        camera.update(settings)
        return settings
    except Exception:
        # A failed command must not leave automatic controls changing later
        # recognition frames.
        lock_camera(cam, previous)
        raise
    finally:
        leds_off(leds)


def recognize_once(cam, bank, trigger_ms, leds):
    """Take exactly one trigger image and refine only that same image."""
    budget = V.Budget(trigger_ms, RESULT_BUDGET_MS - OUTPUT_RESERVE_MS)
    result = {"label": "UNKNOWN", "reason": "NO_RESULT", "groups": {}}
    capture_start = time.ticks_ms()
    try:
        budget.check()
        leds_on(leds)
        try:
            time.sleep_ms(CAPTURE_LED_PRELIGHT_MS)
            budget.check()
            frame = cam.snapshot()  # The only snapshot caused by this trigger.
        finally:
            leds_off(leds)
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


def reply_calibration(uart, cam, camera, leds):
    """Handle one parsed CALIBRATE command and emit exactly one UART reply."""
    line = "RESULT:CALIBRATED"
    error_text = None
    try:
        settings = calibrate_camera(cam, camera, leds)
    except Exception as error:
        line = "RESULT:ERROR"
        error_text = repr(error)
    try:
        uart_write_line(uart, line)
    except Exception as error:
        print("UART_ERROR:" + repr(error))
    print(line)
    if error_text is not None:
        print("CALIBRATE_ERROR:" + error_text)
    else:
        print("CALIBRATION:", settings)


def run():
    # Force this deployment entry to use the SD bank, regardless of the
    # development value currently stored in cards_fast_config.py.
    C.USE_SD_CARD = False
    C.ROOT = TEMPLATE_ROOT
    V.validate_config(RECOGNITION_VISION)
    uart = UART(UART_ID, baudrate=UART_BAUD, bits=8, parity=None, stop=1,
                timeout=UART_TIMEOUT_MS, timeout_char=5,
                rxbuf=UART_RX_BUFFER_BYTES)
    leds = [LED("LED_RED"), LED("LED_GREEN"), LED("LED_BLUE")]
    boot_led_signal(leds)
    V.ensure_storage()
    recover_camera_config()
    cam, camera, unused_core_leds = V.start_camera(False, enable_leds=False)
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
    rx_state = {"buffer": bytearray(), "overflow": False}
    gc.collect()
    while True:
        now = time.ticks_ms()

        # UART commands are consumed whenever the main loop is idle. A command
        # arriving during recognition remains in the RX buffer until that
        # recognition has returned its result.
        for command in read_uart_lines(uart, rx_state):
            state[2] = False
            if command == "CALIBRATE" and not state[0]:
                reply_calibration(uart, cam, camera, leds)
            else:
                try:
                    uart_write_line(uart, "RESULT:ERROR")
                except Exception as error:
                    print("UART_ERROR:" + repr(error))
                print("RESULT:ERROR")
                print("COMMAND_ERROR:" + repr(command))
            high_since = None
            now = time.ticks_ms()

        if state[0] and time.ticks_diff(now, state[1]) >= TRIGGER_FILTER_MS:
            trigger_ms = state[1]
            state[0] = False
            try:
                result, capture_ms = recognize_once(
                    cam, bank, trigger_ms, leds)
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
