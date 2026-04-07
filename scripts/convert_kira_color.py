#!/usr/bin/env python3
from PIL import Image
import os

BASE_DIR = os.path.dirname(__file__)

FACE_DIR = os.path.join(BASE_DIR, "..", "data")
FACE_PATH = os.path.join(FACE_DIR, "kira_face_360.jpg")

SRC_DIR = os.path.join(
    BASE_DIR,
    "imgs", "annimations", "360", "bouches"
)

DST_DIR = os.path.join(BASE_DIR, "mouth_comp")

FILES = [
    "m0.png",
    "m1.png",
    "m2.png",
    "m4.png",
]

# Size of the mouth patch
W = 115
H = 95

# Position of the mouth area in kira_face_360.jpg
MOUTH_X = 110
MOUTH_Y = 155

os.makedirs(DST_DIR, exist_ok=True)


def remove_green_and_decontaminate(img: Image.Image) -> Image.Image:
    img = img.convert("RGBA")
    px = img.load()

    for y in range(img.height):
        for x in range(img.width):
            r, g, b, a = px[x, y]

            # green background -> transparent
            if g > 100 and g > r + 15 and g > b + 15:
                px[x, y] = (0, 0, 0, 0)
                continue

            # decontamination of greenish edges
            if g > r + 5 and g > b + 5:
                excess = min(80, g - max(r, b))
                g2 = max(0, g - excess)
                px[x, y] = (r, g2, b, a)

    return img


def trim_alpha(img: Image.Image, pad: int = 1) -> Image.Image:
    alpha = img.getchannel("A")
    bbox = alpha.getbbox()
    if not bbox:
        return img

    x0, y0, x1, y1 = bbox
    x0 = max(0, x0 - pad)
    y0 = max(0, y0 - pad)
    x1 = min(img.width, x1 + pad)
    y1 = min(img.height, y1 + pad)
    return img.crop((x0, y0, x1, y1))


def main():
    if not os.path.exists(FACE_PATH):
        raise FileNotFoundError(f"Face not found: {FACE_PATH}")

    face = Image.open(FACE_PATH).convert("RGB")
    base_patch = face.crop((MOUTH_X, MOUTH_Y, MOUTH_X + W, MOUTH_Y + H)).convert("RGBA")

    # neutral mouth background
    base_patch.convert("RGB").save(os.path.join(DST_DIR, "bg_mouth.png"))
    print("OK: bg_mouth.png")

    for src_name in FILES:
        src_path = os.path.join(SRC_DIR, src_name)
        if not os.path.exists(src_path):
            print(f"SKIP: not found {src_path}")
            continue

        lips = Image.open(src_path)
        lips = remove_green_and_decontaminate(lips)
        lips = trim_alpha(lips, pad=1)

        canvas = base_patch.copy()

        # centering
        x = (W - lips.width) // 2
        y = (H - lips.height) // 2

        out_name = src_name.replace(".png", "_comp.png")

        canvas.alpha_composite(lips, (x, y))
        canvas.convert("RGB").save(os.path.join(DST_DIR, out_name))

        print(f"OK: {src_name} -> {out_name} ({lips.width}x{lips.height}) pos=({x},{y})")

    print("Composition completed.")


if __name__ == "__main__":
    main()