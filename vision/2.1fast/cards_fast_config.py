"""OpenMV H7 Plus / firmware 5.x settings shared by both entry scripts.

Copy this file and the matching core module beside both entry scripts on /flash.
All coordinates are in the ORIGINAL QVGA frame. No card edges are required.
Capture/recognition locator ROIs and thresholds live in each entry script.
"""

# 默认只使用内部 Flash；修改存储设置后须复位 OpenMV，确保采集和识别路径一致。
USE_SD_CARD = False
STORAGE_NAME = "cards_fast_v1"
ROOT = ("/sdcard/" if USE_SD_CARD else "/flash/") + STORAGE_NAME
MIN_SD_FREE_BYTES = 2 * 1024 * 1024
MIN_FLASH_FREE_BYTES = 128 * 1024

# Camera calibration is tiny and written only when explicitly recalibrating.
# Keep calibration in internal flash regardless of the template medium.
# Change this path only if deliberately starting a new calibration.
CAMERA_CONFIG_PATH = "/flash/cards_fast_v1/camera.json"
PIPELINE_VERSION = 2
FRAME_SIZE = (320, 240)

# Camera mounting orientation.
# True  = camera is physically rotated 180 degrees during development.
# False = final normal camera orientation.
CAMERA_ROTATE_180 = True

PATCH_SIZES = {"rank": (32, 48), "suit": (32, 32),
               "joker": (40, 80), "back": (48, 64)}
PATCH_PADDING = 3
MIN_COMPONENT_PIXELS = 8
MAX_COMPONENTS = 32  # Dense texture is not a valid character candidate.
MIN_BOX = {"rank": (8, 24), "suit": (10, 10), "joker": (10, 25)}
MAX_BOX = {"rank": (115, 145), "suit": (110, 100), "joker": (78, 138)}
MIN_INK_FRACTION = 0.06
MAX_INK_FRACTION = 0.85
BORDER_GUARD = 1  # Reject lettering cut by a search-window boundary.
# Join adjacent components on one text row, e.g. the two digits in 10.
ROW_OVERLAP = 0.45
ROW_GAP_HEIGHT = 0.40
# Ordinary rank must lie above suit. Loosen only to cover measured motion.
PAIR_MAX_DX = 55
PAIR_MIN_DY = 20
PAIR_MAX_DY = 190

RANKS = ("A", "2", "3", "4", "5", "6", "7", "8", "9", "10", "J", "Q", "K")
SUITS = ("heart", "diamond", "club", "spade")

# RGB565 / LAB ranges. Use actual lighting to calibrate these in the IDE.
RED_THRESHOLD = (10, 95, 12, 127, -35, 127)
BLACK_THRESHOLD = (0, 50, -10, 10, -25, 25)
COLOR_MIN_PIXELS = 20
COLOR_MIN_FRACTION = 0.75  # red/(red+black), or black/(red+black).

CAMERA_SETTLE_MS = 2000
CAMERA_APPLY_SETTLE_MS = 200
TRIGGER_PIN = "P9"  # Momentary switch to GND, internal pull-up.
BUTTON_DEBOUNCE_MS = 50
