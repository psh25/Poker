"""OpenMV H7 Plus / firmware 5.x shared settings. Edit before collecting.

Copy this file and cards_fast_core.py beside both entry scripts on /flash.
All coordinates are in the ORIGINAL QVGA frame. No card edges are required.
Changing preprocessing settings invalidates previously generated templates.
"""

ROOT = "/flash/cards_fast_v1"  # Deliberately separate from /flash/templates.
PIPELINE_VERSION = 1
FRAME_SIZE = (320, 240)

# Search windows, not final template rectangles. Tune in capture preview.
# Keep the complete rank (including both digits of 10) and suit in view.
SEARCH_ROIS = {
    "rank": (70, 15, 130, 160),
    "suit": (70, 110, 130, 125),
    # This MUST contain only the stable visible JOKER lettering, no artwork.
    "joker": (95, 25, 80, 140),
    # Use a textured back patch visible throughout the allowed displacement.
    "back": (95, 45, 80, 120),
}
PATCH_SIZES = {"rank": (32, 48), "suit": (32, 32),
               "joker": (40, 80), "back": (48, 64)}
PATCH_PADDING = 3
MIN_CONTRAST = 24  # p95 - p05 of grayscale search window.
THRESHOLD_OFFSET = 0  # Added to Otsu threshold, in grayscale levels.
MIN_COMPONENT_PIXELS = 8
MAX_COMPONENTS = 32  # Dense texture is not a valid character candidate.
MAX_CANDIDATES = 2
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

# Positive foreground on black background; both acquisition and recognition
# use exactly the same mask, padding, rotation and aspect-preserving resize.
COARSE_ANGLES = (0, -6, 6)  # Correction angles in DEGREES.
REFINE_ANGLES = (-3, 3, -9, 9)
REFINE_THRESHOLD_OFFSETS = (-8, 8)  # Used only after normal same-frame pass.
BACK_REFINE_OFFSETS = ((0, 0), (-4, 0), (4, 0), (0, -4), (0, 4),
                       (-4, -4), (-4, 4), (4, -4), (4, 4))
MAX_TEMPLATES_PER_LABEL = 4
RANKS = ("A", "2", "3", "4", "5", "6", "7", "8", "9", "10", "J", "Q", "K")
SUITS = ("heart", "diamond", "club", "spade")

# SSIM thresholds are starting points, NOT calibrated accuracy claims.
ACCEPT_SCORE = {"rank": 0.80, "suit": 0.80, "joker": 0.84, "back": 0.86}
ACCEPT_MARGIN = {"rank": 0.05, "suit": 0.05, "joker": 0.0, "back": 0.0}
SCENE_MARGIN = 0.035  # Margin between threshold-adjusted scene supports.
STRICT_TEMPLATE_COVERAGE = True  # Require 13 ranks, 4 suits, joker and back.

# RGB565 / LAB ranges. Use actual lighting to calibrate these in the IDE.
RED_THRESHOLD = (10, 95, 12, 127, -35, 127)
BLACK_THRESHOLD = (0, 50, -10, 10, -25, 25)
COLOR_MIN_PIXELS = 20
COLOR_MIN_FRACTION = 0.75  # red/(red+black), or black/(red+black).
RED_JOKER_LABEL = "joker_big"
BLACK_JOKER_LABEL = "joker_small"

CAMERA_SETTLE_MS = 2000
CAMERA_APPLY_SETTLE_MS = 200
TRIGGER_PIN = "P9"  # Momentary switch to GND, internal pull-up.
BUTTON_DEBOUNCE_MS = 50
UART_ENABLED = True
UART_ID = 3  # H7 Plus P4=TX, P5=RX; common GND with receiver.
UART_BAUD = 115200
UART_TIMEOUT_MS = 25
RESULT_BUDGET_MS = 900  # From falling-edge timestamp; best-effort deadline.
OUTPUT_RESERVE_MS = 35
MAX_ATTEMPTS = 2
RETRY_SETTLE_MS = 20
DISCARD_AFTER_TRIGGER = 1  # Flush a potentially pre-trigger frame.
PRINT_PROFILE = True
