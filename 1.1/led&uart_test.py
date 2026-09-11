"""OpenMV Cam H7 Plus: RGB LEDs alternate once per second."""

import time
from machine import LED, UART


red_led = LED("LED_RED")
green_led = LED("LED_GREEN")
blue_led = LED("LED_BLUE")

# OpenMV Cam H7 Plus UART3: P4=TX, P5=RX.
uart = UART(3, baudrate=115200)

CHANGE_INTERVAL_MS = 1000

led_sequence = (
    ("RED", red_led),
    ("GREEN", green_led),
    ("BLUE", blue_led),
)


def all_leds_off():
    red_led.off()
    green_led.off()
    blue_led.off()


def report_status(now, color_name, change_count):
    message = "[{} ms] LED {}  change={}".format(
        now, color_name, change_count
    )
    print(message)
    uart.write(message + "\r\n")


def main():
    color_index = 0
    change_count = 0

    all_leds_off()
    color_name, current_led = led_sequence[color_index]
    current_led.on()

    print("OpenMV Cam H7 Plus RGB LED test started")
    print("Color changes every {} ms".format(CHANGE_INTERVAL_MS))
    print("UART3 started: 115200 baud, P4=TX, P5=RX")
    report_status(time.ticks_ms(), color_name, change_count)

    next_change = time.ticks_add(time.ticks_ms(), CHANGE_INTERVAL_MS)

    try:
        while True:
            now = time.ticks_ms()

            if time.ticks_diff(now, next_change) >= 0:
                all_leds_off()
                color_index = (color_index + 1) % len(led_sequence)
                color_name, current_led = led_sequence[color_index]
                current_led.on()
                change_count += 1

                report_status(now, color_name, change_count)

                # Use the previous deadline to keep the interval stable.
                next_change = time.ticks_add(
                    next_change, CHANGE_INTERVAL_MS
                )

            time.sleep_ms(10)

    except KeyboardInterrupt:
        all_leds_off()
        print("LED test stopped; all LEDs are OFF")


main()
