"""Draws the program's icon: a spectrum with a few signals standing out of the
noise, over a waterfall, on a dark rounded square.

  python3 tools/make_icon.py app/resources

writes hf_classifier.ico (16 to 256 px) and hf_classifier.png (64 px, for the
web UI). Needs Pillow. The files are kept in the repository; run this only to
change the icon.
"""
import base64
import io
import math
import sys

from PIL import Image, ImageDraw


def draw(size):
    s = 4 * size  # drawn large, then scaled down (smooth edges)
    im = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    r = s // 6
    d.rounded_rectangle([0, 0, s - 1, s - 1], radius=r, fill=(14, 27, 46, 255))
    # Waterfall in the lower half: rows of dim noise and three signal traces.
    top, bottom = int(s * 0.52), int(s * 0.88)
    left, right = int(s * 0.12), int(s * 0.88)
    rows = 9
    for i in range(rows):
        y0 = top + (bottom - top) * i // rows
        y1 = top + (bottom - top) * (i + 1) // rows - max(1, s // 96)
        shade = 40 + 10 * ((i * 7) % 3)
        d.rectangle([left, y0, right, y1], fill=(20, 52, 96 + shade // 2, 255))
    for x, w, col in ((0.28, 0.05, (255, 196, 61)), (0.52, 0.10, (64, 214, 160)), (0.74, 0.04, (255, 120, 80))):
        cx = int(s * x)
        hw = max(1, int(s * w / 2))
        d.rectangle([cx - hw, top, cx + hw, bottom], fill=col + (255,))
    # Spectrum line above: noise with three peaks.
    base = s * 0.44
    pts = []
    n = 64
    for k in range(n + 1):
        x = left + (right - left) * k / n
        f = k / n
        y = base - s * 0.02 * (0.5 + 0.5 * math.sin(k * 2.3) * math.cos(k * 1.1))
        for px, h, wd in ((0.28, 0.22, 0.03), (0.52, 0.30, 0.06), (0.74, 0.15, 0.025)):
            y -= s * h * math.exp(-((f - (px - 0.12) / 0.76) ** 2) / (2 * wd * wd))
        pts.append((x, y))
    d.line(pts, fill=(235, 242, 250, 255), width=max(2, s // 28), joint="curve")
    return im.resize((size, size), Image.LANCZOS)


def main(out):
    sizes = [16, 20, 24, 32, 40, 48, 64, 128, 256]
    images = [draw(z) for z in sizes]
    images[-1].save(f"{out}/hf_classifier.ico", sizes=[(z, z) for z in sizes], append_images=images[:-1])
    images[sizes.index(64)].save(f"{out}/hf_classifier.png")
    buf = io.BytesIO()
    images[sizes.index(64)].save(buf, format="PNG", optimize=True)
    print("data:image/png;base64," + base64.b64encode(buf.getvalue()).decode()[:60] + "...")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else ".")
