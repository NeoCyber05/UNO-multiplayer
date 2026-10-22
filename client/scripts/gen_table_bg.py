#!/usr/bin/env python3
"""
Sinh ảnh nền bàn chơi vào client/public/images/backgrounds/table.jpg (1920x1200, 16:10).

Chạy:   python client/scripts/gen_table_bg.py
Cần:    Pillow, font Arial Black.

Lớp nền tĩnh cho TableBackground.jsx: gradient đỏ-cam, chữ UNO chìm, dải màu góc phải,
confetti, vignette. Tia sáng xoay + đèn sân khấu vẫn là CSS động vẽ đè lên trên.
"""
import math
import random
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw, ImageFilter

from gen_cards import mix, rotate, solid, text_art

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'public' / 'images' / 'backgrounds' / 'table.jpg'

OUT_W, OUT_H = 1920, 1200
SS = 2
W, H = OUT_W * SS, OUT_H * SS
CENTER = (W * 0.5, H * 0.46)

# cùng các mốc màu với radial-gradient của .table-bg
STOPS = [(0.00, (255, 179, 71)), (0.18, (255, 125, 34)), (0.42, (236, 61, 29)),
         (0.68, (189, 21, 29)), (1.00, (94, 6, 16))]


def radial_gradient():
    """Gradient elip quanh CENTER, bán trục 62% x 58% như CSS."""
    sw, sh = W // 8, H // 8
    im = Image.new('RGB', (sw, sh))
    px = im.load()
    cx, cy = CENTER[0] / 8, CENTER[1] / 8
    rx, ry = sw * 0.62, sh * 0.58
    for y in range(sh):
        for x in range(sw):
            t = min(1.0, math.hypot((x - cx) / rx, (y - cy) / ry))
            for (t0, c0), (t1, c1) in zip(STOPS, STOPS[1:]):
                if t <= t1:
                    px[x, y] = mix(c0, c1, (t - t0) / (t1 - t0))
                    break
    return im.resize((W, H), Image.BICUBIC).convert('RGBA')


def watermark():
    art = text_art('UNO', W * 0.20, (255, 90, 40), italic=0.12)
    art = rotate(art, 10)
    a = art.getchannel('A').point(lambda v: int(v * 0.22))
    layer = Image.new('RGBA', (W, H), (0, 0, 0, 0))
    pos = (int(CENTER[0] - art.width / 2), int(CENTER[1] - art.height / 2))
    # khối bóng đậm phía dưới + viền sáng mảnh
    shadow = Image.new('L', (W, H), 0)
    shadow.paste(a, (pos[0], pos[1] + int(W * 0.012)))
    layer.alpha_composite(solid((W, H), (120, 0, 10), shadow))
    body = Image.new('L', (W, H), 0)
    body.paste(a, pos)
    rim = body.filter(ImageFilter.MaxFilter(7)).filter(ImageFilter.GaussianBlur(2))
    layer.alpha_composite(solid((W, H), (255, 210, 120), ImageChops.subtract(rim, body).point(lambda v: v // 3)))
    layer.alpha_composite(solid((W, H), (255, 90, 40), body))
    return layer


def stripes():
    """Dải vàng-đỏ chéo ở góc phải dưới như .table-bg__stripes."""
    band = Image.new('RGBA', (int(W * 0.34), int(H * 0.15)), (0, 0, 0, 0))
    d = ImageDraw.Draw(band)
    h = band.height
    d.rectangle([0, 0, band.width, int(h * 0.46)], fill=(255, 204, 0, 230))
    d.rectangle([0, int(h * 0.54), band.width, h], fill=(227, 18, 29, 230))
    band = band.rotate(24, resample=Image.BICUBIC, expand=True)
    layer = Image.new('RGBA', (W, H), (0, 0, 0, 0))
    layer.alpha_composite(band, (int(W * 0.72), int(H * 0.74)))
    return layer


def confetti(canvas, rng):
    cols = [(255, 59, 48), (255, 204, 0), (53, 211, 153), (76, 134, 255), (255, 255, 255)]
    layer = Image.new('RGBA', (W, H), (0, 0, 0, 0))
    for _ in range(70):
        x, y = rng.uniform(0, W), rng.uniform(0, H)
        # tránh vùng giữa bàn
        if math.hypot((x - CENTER[0]) / (W * 0.30), (y - CENTER[1]) / (H * 0.32)) < 1:
            continue
        s = rng.uniform(10, 26) * SS
        piece = Image.new('RGBA', (int(s), int(s * 0.4)), rng.choice(cols) + (200,))
        piece = piece.rotate(rng.uniform(0, 180), resample=Image.BICUBIC, expand=True)
        layer.alpha_composite(piece, (int(x), int(y)))
    glow = layer.filter(ImageFilter.GaussianBlur(4 * SS))
    canvas.alpha_composite(glow)
    canvas.alpha_composite(layer)


def vignette():
    m = Image.new('L', (W, H), 255)
    cx, cy = CENTER
    ImageDraw.Draw(m).ellipse([cx - W * 0.52, cy - H * 0.55, cx + W * 0.52, cy + H * 0.55], fill=0)
    m = m.filter(ImageFilter.GaussianBlur(W * 0.08)).point(lambda v: int(v * 0.65))
    return solid((W, H), (50, 0, 6), m)


def main():
    rng = random.Random(7)
    bg = radial_gradient()
    bg.alpha_composite(watermark())
    bg.alpha_composite(stripes())
    confetti(bg, rng)
    bg.alpha_composite(vignette())
    OUT.parent.mkdir(parents=True, exist_ok=True)
    bg.convert('RGB').resize((OUT_W, OUT_H), Image.LANCZOS).save(OUT, quality=90, optimize=True)
    print(f'Generated {OUT}')


if __name__ == '__main__':
    main()
