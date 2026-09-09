"""OpenMV H7 Plus triggered card recognition, no CNN, no no-card class.

Deploy with cards_fast_core.py, cards_fast_config.py and newly captured bank.
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
import cards_fast_core as V


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
        if time.ticks_diff(time.ticks_ms(), start) >= C.UART_TIMEOUT_MS:
            raise OSError("UART write timeout")
        if not count:
            time.sleep_ms(1)
    # txdone means the final byte has actually left the UART, not only queued.
    while not uart.txdone():
        if time.ticks_diff(time.ticks_ms(), start) >= C.UART_TIMEOUT_MS:
            raise OSError("UART drain timeout")
        time.sleep_ms(1)


def recognize_trigger(cam, bank, trigger_ms):
    budget = V.Budget(trigger_ms, C.RESULT_BUDGET_MS - C.OUTPUT_RESERVE_MS)
    result = {"label": "UNKNOWN", "reason": "NO_RESULT", "groups": {}}
    profile = {"attempts": 0, "capture_ms": 0, "passes": []}
    try:
        for attempt in range(C.MAX_ATTEMPTS):
            budget.check()
            start = time.ticks_ms()
            if attempt == 0:
                for unused in range(C.DISCARD_AFTER_TRIGGER):
                    cam.snapshot()
                    budget.check()
            frame = cam.snapshot()  # Acquired AFTER trigger/debounce, not preview.
            budget.check()
            profile["capture_ms"] += time.ticks_diff(time.ticks_ms(), start)
            profile["attempts"] += 1
            records = {k: {} for k in ("rank", "suit", "joker", "back")}
            result = V.process_pass(frame, bank, records, C.COARSE_ANGLES, budget=budget)
            coarse_ms = result["pass_ms"]
            profile["passes"].append((attempt + 1, "coarse", result["locate_ms"], result["pass_ms"]))
            if result["label"] != "UNKNOWN":
                break
            # Refine rotation on SAME frame. All labels remain eligible so an
            # initially missed competitor does not produce an artificial margin.
            if C.REFINE_ANGLES and budget.remaining() > max(80, coarse_ms * 2):
                result = V.process_pass(frame, bank, records, C.REFINE_ANGLES, budget=budget)
                profile["passes"].append((attempt + 1, "angle", result["locate_ms"], result["pass_ms"]))
            if result["label"] != "UNKNOWN":
                break
            for offset in C.REFINE_THRESHOLD_OFFSETS:
                if budget.remaining() <= max(100, coarse_ms * 2):
                    break
                # Re-evaluate all coarse angles after threshold adjustment.
                result = V.process_pass(frame, bank, records, C.COARSE_ANGLES, offset, budget)
                profile["passes"].append((attempt + 1, "threshold", result["locate_ms"], result["pass_ms"]))
                if result["label"] != "UNKNOWN":
                    break
            if result["label"] != "UNKNOWN":
                break
            if attempt + 1 >= C.MAX_ATTEMPTS:
                break
            # Admit a retry only if observed capture+coarse cost fits; this is
            # an estimate, with deadline checks around each subsequent stage.
            needed = max(120, profile["capture_ms"] // profile["attempts"] + coarse_ms + C.RETRY_SETTLE_MS + 30)
            if budget.remaining() <= needed:
                result["reason"] = "RETRY_BUDGET_EXHAUSTED"
                break
            del frame
            time.sleep_ms(C.RETRY_SETTLE_MS)
        budget.check()
    except V.BudgetExceeded:
        result = {"label": "UNKNOWN", "reason": "TIMEOUT", "groups": {}}
    profile["processing_ms"] = time.ticks_diff(time.ticks_ms(), trigger_ms)
    return result, profile


def main():
    V.validate_config()
    V.ensure_storage()
    cam, camera, leds = V.start_camera(False)
    bank = V.load_bank(camera)
    uart = UART(C.UART_ID, baudrate=C.UART_BAUD, bits=8, parity=None, stop=1,
                timeout=C.UART_TIMEOUT_MS) if C.UART_ENABLED else None
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
                try:
                    result, profile = recognize_trigger(cam, bank, trigger_ms)
                except Exception as error:
                    result = {"label": "ERROR", "reason": repr(error), "groups": {}}
                    profile = {}
                # Ensure no success label is emitted after the processing deadline.
                if time.ticks_diff(time.ticks_ms(), trigger_ms) >= C.RESULT_BUDGET_MS - C.OUTPUT_RESERVE_MS:
                    result["label"], result["reason"] = "UNKNOWN", "TIMEOUT"
                tx_start = time.ticks_ms()
                try:
                    send_result(uart, result["label"])
                    profile["uart_ms"] = time.ticks_diff(time.ticks_ms(), tx_start)
                    profile["total_ms"] = time.ticks_diff(time.ticks_ms(), trigger_ms)
                    profile["over_budget"] = profile["total_ms"] > C.RESULT_BUDGET_MS
                except Exception as error:
                    profile["transport_error"] = repr(error)
                # USB printing is outside the UART result critical path.
                if C.PRINT_PROFILE:
                    print("RESULT:" + result["label"] if uart is not None else "RESULT_SENT")
                    print("PROFILE", sequence, profile)
                    print("DECISION", result["reason"], result["groups"])
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
