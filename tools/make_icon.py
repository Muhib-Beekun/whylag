"""Generate whylag.ico - waveform lag symbol on dark background.

Requires: pip install pillow
Run from repo root: python tools/make_icon.py
"""
import os
from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "whylag.ico")


def draw_icon(size):
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    pad = max(2, size // 16)
    r = size // 2 - pad
    cx, cy = size // 2, size // 2
    d.ellipse((cx - r, cy - r, cx + r, cy + r), fill=(30, 30, 36, 255))
    d.ellipse((cx - r, cy - r, cx + r, cy + r), outline=(0, 120, 212, 255), width=max(1, size // 32))

    bar_w = max(2, size // 10)
    gap = max(2, size // 14)
    base_y = cy + size // 8
    heights = [size // 5, size // 3, size // 4, size // 6]
    total_w = len(heights) * bar_w + (len(heights) - 1) * gap
    x = cx - total_w // 2
    for h in heights:
        d.rectangle((x, base_y - h, x + bar_w, base_y), fill=(0, 120, 212, 255))
        x += bar_w + gap

    arrow_y = cy - size // 6
    aw = size // 6
    d.polygon([
        (cx, arrow_y + aw),
        (cx - aw // 2, arrow_y),
        (cx + aw // 2, arrow_y),
    ], fill=(255, 185, 0, 255))

    return img


sizes = [256, 128, 64, 48, 32, 16]
images = [draw_icon(s) for s in sizes]
images[0].save(
    OUT,
    format="ICO",
    sizes=[(s, s) for s in sizes],
    append_images=images[1:],
)
print("wrote", OUT)
