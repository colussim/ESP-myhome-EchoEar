#!/usr/bin/env python3
from PIL import Image
import os

BASE_DIR = os.path.dirname(__file__)

SRC_DIR = os.path.join(BASE_DIR, "mouth_comp")
DST_DIR = os.path.join(BASE_DIR, "..", "data")

FILES = [
    ("bg_mouth.png", "bg_mouth.raw"),
    ("m0_comp.png", "mouth_1_small.raw"),
    ("m1_comp.png", "mouth_2_medium.raw"),
    ("m2_comp.png", "mouth_3_smile.raw"),
    ("m4_comp.png", "mouth_4_small2.raw"),
]

W = 115
H = 95

os.makedirs(DST_DIR, exist_ok=True)

def rgb888_to_rgb565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)

def convert_png_to_raw(src_path, dst_path):
    img = Image.open(src_path).convert("RGB")

    if img.width != W or img.height != H:
        raise ValueError(
            f"{os.path.basename(src_path)} invalid size {img.width}x{img.height}, expected {W}x{H}"
        )

    with open(dst_path, "wb") as f:
        for y in range(H):
            for x in range(W):
                r, g, b = img.getpixel((x, y))
                v = rgb888_to_rgb565(r, g, b)
                f.write(bytes([(v >> 8) & 0xFF, v & 0xFF]))

def main():
    for png_name, raw_name in FILES:
        src = os.path.join(SRC_DIR, png_name)
        dst = os.path.join(DST_DIR, raw_name)

        if not os.path.exists(src):
            raise FileNotFoundError(f"File not found: {src}")

        convert_png_to_raw(src, dst)
        size = os.path.getsize(dst)
        print(f"OK: {png_name} -> {raw_name} ({size} bytes)")

    print("Conversion completed.")

if __name__ == "__main__":
    main()