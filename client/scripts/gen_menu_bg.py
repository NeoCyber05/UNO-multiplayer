#!/usr/bin/env python3
"""
Sinh ảnh nền màn hình menu vào client/public/images/backgrounds/menu.jpg (1920x1200, 16:10).

Chạy:   python client/scripts/gen_menu_bg.py
Cần:    Pillow.

Tối giản, không dính UNO: trời xanh dương sáng dần xuống dưới, hành tinh lớn mờ (depth of field)
nhô lên ở giữa-dưới với hào quang khí quyển, vài dải mây mềm, sao mờ phía trên.
Vùng giữa-trên thoáng để carousel chế độ chơi nổi lên.
"""
import random
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw, ImageFilter

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'public' / 'images' / 'backgrounds' / 'menu.jpg'

OUT_W, OUT_H = 1920, 1200
SS = 2
W, H = OUT_W * SS, OUT_H * SS

PLANET_C = (W * 0.5, H * 1.06)
PLANET_R = H * 0.54

# trên -> dưới
SKY = [(0.00, (14, 36, 110)), (0.45, (30, 86, 190)), (0.75, (52, 134, 226)), (1.00, (74, 160, 238))]


def mix(c1, c2, t):
    t = max(0.0, min(1.0, t))
    return tuple(int(a + (b - a) * t) for a, b in zip(c1, c2))


def solid(size, color, alpha):
    im = Image.new('RGBA', size, color + (0,))
    im.putalpha(alpha)
    return im


def sky():
    col = Image.new('RGB', (1, 256))
    for y in range(256):
        t = y / 255
        for (t0, c0), (t1, c1) in zip(SKY, SKY[1:]):
            if t <= t1:
                col.putpixel((0, y), mix(c0, c1, (t - t0) / (t1 - t0)))
                break
    return col.resize((W, H), Image.BICUBIC).convert('RGBA')


def glow(center, rx, ry, color, strength, blur):
    m = Image.new('L', (W, H), 0)
    cx, cy = center
    ImageDraw.Draw(m).ellipse([cx - rx, cy - ry, cx + rx, cy + ry], fill=strength)
    return solid((W, H), color, m.filter(ImageFilter.GaussianBlur(blur)))


def stars(rng):
    layer = Image.new('RGBA', (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    for _ in range(140):
        x, y = rng.uniform(0, W), rng.uniform(0, H) ** 1.6 / H ** 0.6 * 0.55
        r = rng.uniform(1.2, 3.2) * SS
        d.ellipse([x - r, y - r, x + r, y + r], fill=(255, 255, 255, int(rng.uniform(40, 150))))
    return layer.filter(ImageFilter.GaussianBlur(1.2 * SS))


def planet(rng):
    cx, cy = PLANET_C
    r = PLANET_R
    disk = Image.new('L', (W, H), 0)
    ImageDraw.Draw(disk).ellipse([cx - r, cy - r, cx + r, cy + r], fill=255)

    # đại dương: sáng ở đỉnh (hướng sáng), tối dần xuống
    body = Image.new('RGB', (1, 256))
    for y in range(256):
        body.putpixel((0, y), mix((70, 170, 240), (14, 56, 140), y / 255))
    top = int(cy - r)
    body = body.resize((W, int(r * 2)), Image.BICUBIC)
    ocean = Image.new('RGBA', (W, H), (0, 0, 0, 0))
    ocean.paste(body, (0, top))

    # lục địa: cụm elip ngẫu nhiên, gộp lại rồi làm mềm
    land = Image.new('L', (W, H), 0)
    d = ImageDraw.Draw(land)
    for _ in range(6):
        ax = cx + rng.uniform(-0.85, 0.85) * r
        ay = cy - r + rng.uniform(0.06, 0.55) * r
        for _ in range(18):
            ex, ey = ax + rng.gauss(0, r * 0.10), ay + rng.gauss(0, r * 0.06)
            er = rng.uniform(0.03, 0.09) * r
            d.ellipse([ex - er * 1.4, ey - er, ex + er * 1.4, ey + er], fill=255)
    land = land.filter(ImageFilter.GaussianBlur(r * 0.015)).point(lambda v: 255 if v > 140 else 0)
    land = ImageChops.multiply(land, disk).filter(ImageFilter.GaussianBlur(r * 0.004))
    green = Image.new('RGB', (1, 256))
    for y in range(256):
        green.putpixel((0, y), mix((96, 176, 110), (26, 84, 74), y / 255))
    green = green.resize((W, int(r * 2)), Image.BICUBIC)
    greens = Image.new('RGB', (W, H))
    greens.paste(green, (0, top))
    ocean.alpha_composite(Image.merge('RGBA', (*greens.split(), land)))

    ocean.putalpha(disk)

    # tối viền (limb darkening) + mây trắng mảnh trên bề mặt
    limb = Image.new('L', (W, H), 255)
    ImageDraw.Draw(limb).ellipse([cx - r * 0.86, cy - r * 0.86, cx + r * 0.86, cy + r * 0.86], fill=0)
    limb = ImageChops.multiply(limb.filter(ImageFilter.GaussianBlur(r * 0.10)), disk).point(lambda v: int(v * 0.55))
    ocean.alpha_composite(solid((W, H), (8, 24, 80), limb))

    clouds = Image.new('L', (W, H), 0)
    d = ImageDraw.Draw(clouds)
    for _ in range(30):
        ex = cx + rng.uniform(-0.95, 0.95) * r
        ey = cy - r + rng.uniform(0.02, 0.45) * r
        ew, eh = rng.uniform(0.08, 0.22) * r, rng.uniform(0.012, 0.03) * r
        d.ellipse([ex - ew, ey - eh, ex + ew, ey + eh], fill=int(rng.uniform(60, 130)))
    clouds = ImageChops.multiply(clouds.filter(ImageFilter.GaussianBlur(r * 0.012)), disk)
    ocean.alpha_composite(solid((W, H), (240, 248, 255), clouds))

    # depth of field: hành tinh ở xa, mờ hẳn
    return ocean.filter(ImageFilter.GaussianBlur(16 * SS))


def atmosphere():
    cx, cy = PLANET_C
    r = PLANET_R
    layer = glow(PLANET_C, r * 1.14, r * 1.14, (150, 215, 255), 170, r * 0.12)
    inner = Image.new('L', (W, H), 0)
    ImageDraw.Draw(inner).ellipse([cx - r, cy - r, cx + r, cy + r], fill=255)
    rim = Image.new('L', (W, H), 0)
    ImageDraw.Draw(rim).ellipse([cx - r * 1.015, cy - r * 1.015, cx + r * 1.015, cy + r * 1.015], fill=255)
    rim = ImageChops.subtract(rim, inner.filter(ImageFilter.GaussianBlur(r * 0.01))).filter(ImageFilter.GaussianBlur(r * 0.03))
    layer.alpha_composite(solid((W, H), (200, 240, 255), rim.point(lambda v: min(255, int(v * 1.4)))))
    return layer


def sky_clouds(rng):
    """Dải mây mềm hai bên chân trời."""
    m = Image.new('L', (W, H), 0)
    d = ImageDraw.Draw(m)
    for side in (-1, 1):
        for _ in range(16):
            x = W * 0.5 + side * rng.uniform(0.28, 0.62) * W
            y = H * rng.uniform(0.62, 0.95)
            rw, rh = rng.uniform(0.06, 0.14) * W, rng.uniform(0.03, 0.06) * H
            d.ellipse([x - rw, y - rh, x + rw, y + rh], fill=int(rng.uniform(50, 110)))
    return solid((W, H), (225, 238, 255), m.filter(ImageFilter.GaussianBlur(W * 0.02)))


def vignette():
    m = Image.new('L', (W, H), 255)
    ImageDraw.Draw(m).ellipse([-W * 0.1, -H * 0.15, W * 1.1, H * 1.15], fill=0)
    m = m.filter(ImageFilter.GaussianBlur(W * 0.06)).point(lambda v: int(v * 0.30))
    return solid((W, H), (6, 16, 60), m)


def main():
    rng = random.Random(3)
    bg = sky()
    bg.alpha_composite(stars(rng))
    bg.alpha_composite(glow((W * 0.5, H * 0.45), W * 0.45, H * 0.35, (120, 190, 255), 90, W * 0.08))
    bg.alpha_composite(atmosphere())
    bg.alpha_composite(planet(rng))
    bg.alpha_composite(sky_clouds(rng))
    bg.alpha_composite(vignette())
    OUT.parent.mkdir(parents=True, exist_ok=True)
    bg.convert('RGB').resize((OUT_W, OUT_H), Image.LANCZOS).save(OUT, quality=90, optimize=True)
    print(f'Generated {OUT}')


if __name__ == '__main__':
    main()
