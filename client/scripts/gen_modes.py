#!/usr/bin/env python3
"""
Sinh ảnh thẻ chế độ ở Sảnh (classic, 2v2, side, custom) vào client/public/images/modes/.

Chạy:   python client/scripts/gen_modes.py [--preview]
Cần:    Pillow, font Arial Black, và bộ bài đã sinh sẵn (python client/scripts/gen_cards.py).
--preview  xuất thêm client/scripts/modes_preview.png.

Dùng lại helper vẽ của gen_cards.py. Vẽ ở SS lần rồi thu nhỏ về 600x900 (tỉ lệ 2:3).
"""
import math
import sys
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw, ImageFilter

from gen_cards import (BLACK, COLORS, WHITE, colorize, compose, crop_alpha, dilate, font,
                       mix, rotate, solid, vgrad, wild_oval_art)

ROOT = Path(__file__).resolve().parents[1]
OUT_DIR = ROOT / 'public' / 'images' / 'modes'
CARDS_DIR = ROOT / 'public' / 'images' / 'cards'

OUT_W, OUT_H = 600, 900
SS = 3
W, H = OUT_W * SS, OUT_H * SS

RADIUS = int(W * 0.074)        # khớp border-radius 22px của thẻ 300px trong lobby.css
BORDER = int(W * 0.034)
STROKE = int(W * 0.022)
SHADOW = (int(W * 0.010), int(W * 0.014), W * 0.010, 0.5)
SMALL_SHADOW = (int(W * 0.005), int(W * 0.007), W * 0.005, 0.45)

GOLD_TOP, GOLD_BOTTOM = (255, 240, 90), (255, 160, 0)
SKIN = (255, 214, 170)


# ---------------------------------------------------------------- helpers

def card_img(name, w):
    im = Image.open(CARDS_DIR / f'{name}.png').convert('RGBA')
    return im.resize((int(w), int(w * im.height / im.width)), Image.LANCZOS)


def sunburst(size, center, color, rays=18, alpha=46):
    """Các tia sáng xen kẽ tỏa từ center."""
    m = Image.new('L', size, 0)
    d = ImageDraw.Draw(m)
    R = max(size) * 1.5
    cx, cy = center
    step = 360 / rays
    for i in range(rays):
        a0, a1 = math.radians(i * step), math.radians(i * step + step / 2)
        d.polygon([(cx, cy), (cx + R * math.cos(a0), cy + R * math.sin(a0)),
                   (cx + R * math.cos(a1), cy + R * math.sin(a1))], fill=alpha)
    return solid(size, color, m.filter(ImageFilter.GaussianBlur(W * 0.004)))


def radial(size, center, radius, color, alpha=255):
    m = Image.new('L', size, 0)
    cx, cy = center
    ImageDraw.Draw(m).ellipse([cx - radius, cy - radius, cx + radius, cy + radius], fill=alpha)
    return solid(size, color, m.filter(ImageFilter.GaussianBlur(radius * 0.45)))


def sparkle_art(s, color=WHITE):
    """Ngôi sao 4 cánh lấp lánh."""
    m = Image.new('L', (s, s), 0)
    c, r, k = s / 2, s / 2, s * 0.11
    ImageDraw.Draw(m).polygon([(c, 0), (c + k, c - k), (s, c), (c + k, c + k),
                               (c, s), (c - k, c + k), (0, c), (c - k, c - k)], fill=255)
    return colorize(m, color)


def title_art(lines, max_w, max_size):
    """Khối chữ nhiều dòng, mỗi dòng co cho vừa max_w, tô gradient vàng."""
    masks = []
    for txt, rel in lines:
        f = font(max_size * rel)
        l, t, r, b = f.getbbox(txt)
        m = Image.new('L', (r - l + 20, b - t + 20), 0)
        ImageDraw.Draw(m).text((10 - l, 10 - t), txt, font=f, fill=255)
        m = m.crop(m.getbbox())
        if m.width > max_w:
            m = m.resize((int(max_w), int(m.height * max_w / m.width)), Image.LANCZOS)
        masks.append(m)
    gap = int(max_size * 0.10)
    bw = max(m.width for m in masks)
    bh = sum(m.height for m in masks) + gap * (len(masks) - 1)
    block = Image.new('L', (bw, bh), 0)
    y = 0
    for m in masks:
        block.paste(m, ((bw - m.width) // 2, y))
        y += m.height + gap
    art = vgrad(block.size, GOLD_TOP, GOLD_BOTTOM).convert('RGBA')
    art.putalpha(block)
    # vệt bóng sáng nửa trên mỗi chữ
    gloss = Image.linear_gradient('L').resize(block.size).point(lambda v: max(0, 110 - v))
    art.alpha_composite(solid(block.size, WHITE, ImageChops.multiply(gloss, block)))
    return art


def add_title(canvas, lines, center, angle=-7, max_w=W * 0.78, max_size=W * 0.24):
    art = rotate(title_art(lines, max_w, max_size), angle)
    compose(canvas, art, center, STROKE, shadow=SHADOW, extrude=int(W * 0.022))


def fan(canvas, names, center, card_w, spread, lift=0.10):
    """Xòe quạt các lá bài thật quanh center."""
    n = len(names)
    for i, name in enumerate(names):
        t = i - (n - 1) / 2
        art = rotate(card_img(name, card_w), -t * spread)
        cx = center[0] + t * card_w * 0.52
        cy = center[1] + abs(t) ** 1.6 * card_w * lift
        compose(canvas, art, (cx, cy), int(W * 0.006), shadow=SMALL_SHADOW)


def person_art(s, shirt):
    """Nhân vật tròn trịa: đầu + vai, nét viền đen."""
    im = Image.new('RGBA', (s, int(s * 1.25)), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    hw = int(s * 0.25)
    body_top = int(s * 0.55)
    d.rounded_rectangle([int(s * 0.08), body_top, int(s * 0.92), int(s * 1.25) + s],
                        radius=int(s * 0.4), fill=shirt)
    d.ellipse([s // 2 - hw, int(s * 0.06), s // 2 + hw, int(s * 0.06) + 2 * hw], fill=SKIN)
    # mắt + miệng cười
    ey, er = int(s * 0.29), max(2, int(s * 0.028))
    for ex in (s // 2 - int(s * 0.09), s // 2 + int(s * 0.09)):
        d.ellipse([ex - er, ey - er, ex + er, ey + er], fill=BLACK)
    mw = int(s * 0.10)
    d.arc([s // 2 - mw, ey - int(s * 0.02), s // 2 + mw, ey + int(s * 0.13)], 20, 160,
          fill=BLACK, width=max(2, int(s * 0.025)))
    return im


def gear_art(s, teeth=9, color=(245, 245, 250)):
    m = Image.new('L', (s, s), 0)
    d = ImageDraw.Draw(m)
    c, ro, ri = s / 2, s * 0.5, s * 0.40
    pts = []
    for i in range(teeth * 4):
        a = math.radians(i * 360 / (teeth * 4))
        r = ro if i % 4 in (1, 2) else ri
        pts.append((c + r * math.cos(a), c + r * math.sin(a)))
    d.polygon(pts, fill=255)
    hole = s * 0.17
    d.ellipse([c - hole, c - hole, c + hole, c + hole], fill=0)
    return colorize(m, color)


# ---------------------------------------------------------------- card

def new_mode(color, burst_center):
    top, bottom = mix(color, WHITE, 0.22), mix(color, BLACK, 0.35)
    inner = vgrad((W, H), top, bottom).convert('RGBA')
    inner.alpha_composite(radial((W, H), burst_center, W * 0.5, mix(color, WHITE, 0.45), 150))
    inner.alpha_composite(sunburst((W, H), burst_center, WHITE))
    return inner


def finish(inner, sparkles=()):
    for x, y, s in sparkles:
        compose(inner, sparkle_art(int(W * s)), (W * x, H * y))
    base = Image.new('RGBA', (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(base)
    d.rounded_rectangle([0, 0, W - 1, H - 1], radius=RADIUS, fill=(206, 206, 206))
    e = int(W * 0.006)
    d.rounded_rectangle([e, e, W - 1 - e, H - 1 - e], radius=RADIUS - e, fill=WHITE)
    mask = Image.new('L', (W, H), 0)
    ImageDraw.Draw(mask).rounded_rectangle([BORDER, BORDER, W - 1 - BORDER, H - 1 - BORDER],
                                           radius=RADIUS - BORDER, fill=255)
    # tối nhẹ mép trong cho có chiều sâu
    edge = ImageChops.subtract(mask, dilate(ImageChops.invert(mask), int(W * 0.05)))
    vign = ImageChops.subtract(mask, edge).filter(ImageFilter.GaussianBlur(W * 0.04))
    inner.alpha_composite(solid((W, H), BLACK, vign.point(lambda v: int(v * 0.35))))
    inner.putalpha(mask)
    base.alpha_composite(inner)
    return base.resize((OUT_W, OUT_H), Image.LANCZOS)


def classic():
    inner = new_mode((232, 33, 46), (W * 0.5, H * 0.34))
    fan(inner, ['blue_7', 'green_reverse', 'yellow_skip', 'red_draw2', 'wild_wild4'],
        (W * 0.5, H * 0.33), W * 0.27, 13)
    add_title(inner, [('CLASSIC', 1.0), ('MODE', 0.85)], (W * 0.5, H * 0.73))
    return finish(inner, [(0.16, 0.14, 0.07), (0.85, 0.20, 0.05), (0.84, 0.56, 0.06)])


TEAM = (255, 204, 0)
RIVAL = (70, 110, 200)
FELT = (20, 110, 60)


def felt_table(canvas, box):
    """Mặt bàn nỉ xanh viền gỗ, nhìn từ trên xuống."""
    x0, y0, x1, y1 = box
    rim = int(W * 0.022)
    m = Image.new('L', (W, H), 0)
    ImageDraw.Draw(m).ellipse(box, fill=255)
    sh = ImageChops.offset(m, 0, int(W * 0.018)).filter(ImageFilter.GaussianBlur(W * 0.015))
    canvas.alpha_composite(solid((W, H), BLACK, sh.point(lambda v: int(v * 0.45))))
    canvas.alpha_composite(solid((W, H), (120, 70, 35), m))
    inner = Image.new('L', (W, H), 0)
    ImageDraw.Draw(inner).ellipse([x0 + rim, y0 + rim, x1 - rim, y1 - rim], fill=255)
    felt = vgrad((W, H), mix(FELT, WHITE, 0.15), mix(FELT, BLACK, 0.25)).convert('RGBA')
    felt.putalpha(inner)
    canvas.alpha_composite(felt)


def pile(canvas, center, w):
    """Chồng bài giữa bàn: lá úp + lá vừa đánh."""
    compose(canvas, rotate(card_img('back', w), 14), (center[0] - w * 0.35, center[1]),
            int(W * 0.005), shadow=SMALL_SHADOW)
    compose(canvas, rotate(card_img('red_7', w), -10), (center[0] + w * 0.35, center[1]),
            int(W * 0.005), shadow=SMALL_SHADOW)


def seat(canvas, pos, teammate, cards_at, spread=12):
    """Người chơi + bài trên tay: đồng đội áo vàng bài ngửa, đối thủ áo xanh bài úp."""
    ps = int(W * 0.20)
    compose(canvas, person_art(ps, TEAM if teammate else RIVAL), (W * pos[0], H * pos[1]),
            int(W * 0.011), shadow=SHADOW)
    names = ['blue_skip', 'green_9', 'yellow_2'] if teammate else ['back'] * 3
    fan(canvas, names, (W * cards_at[0], H * cards_at[1]), W * 0.085, spread, 0.06)


# vị trí người chơi + vị trí bài trên tay quanh bàn tròn
SEATS = {
    'top': ((0.5, 0.13), (0.73, 0.14)),
    'bottom': ((0.5, 0.53), (0.73, 0.54)),
    'left': ((0.17, 0.33), (0.17, 0.43)),
    'right': ((0.83, 0.33), (0.83, 0.43)),
}
LINK = (255, 240, 120)


def round_table(inner, teammates, link):
    """Bàn tròn 4 ghế; `teammates` = 2 ghế đồng đội, `link` vẽ đường nối 2 người."""
    felt_table(inner, [W * 0.30, H * 0.235, W * 0.70, H * 0.435])
    pile(inner, (W * 0.5, H * 0.335), W * 0.075)
    link(ImageDraw.Draw(inner), int(W * 0.014))
    for key in teammates:
        (x, y), _ = SEATS[key]
        inner.alpha_composite(radial((W, H), (W * x, H * y), W * 0.16, (255, 230, 90), 220))
    for key, (pos, cards) in SEATS.items():
        seat(inner, pos, key in teammates, cards)


def two_vs_two():
    inner = new_mode((31, 166, 74), (W * 0.5, H * 0.33))

    def link(d, lw):  # nét đứt thẳng qua bàn: đồng đội ngồi đối diện
        x = W * 0.5
        for y in range(int(H * 0.19), int(H * 0.49), int(H * 0.03)):
            d.line([(x, y), (x, y + H * 0.016)], fill=LINK, width=lw)

    round_table(inner, ('top', 'bottom'), link)
    add_title(inner, [('2 VS 2', 1.0)], (W * 0.5, H * 0.76), max_size=W * 0.30)
    return finish(inner, [(0.15, 0.10, 0.06), (0.14, 0.60, 0.05), (0.88, 0.64, 0.05)])


def side_to_side():
    inner = new_mode((23, 104, 216), (W * 0.5, H * 0.33))

    def link(d, lw):  # cung nét đứt men theo mép bàn: đồng đội ngồi cạnh nhau
        box = [W * 0.25, H * 0.205, W * 0.75, H * 0.465]
        for a in range(94, 180, 12):
            d.arc(box, a, a + 7, fill=LINK, width=lw)

    round_table(inner, ('bottom', 'left'), link)
    add_title(inner, [('SIDE', 1.0), ('TO SIDE', 0.72)], (W * 0.5, H * 0.78), max_size=W * 0.21)
    return finish(inner, [(0.15, 0.10, 0.06), (0.14, 0.60, 0.05), (0.88, 0.64, 0.05)])


def custom():
    inner = new_mode((240, 168, 0), (W * 0.5, H * 0.32))
    big = rotate(gear_art(int(W * 0.50)), 8)
    compose(inner, big, (W * 0.47, H * 0.32), STROKE, shadow=SHADOW)
    oval = rotate(wild_oval_art(int(W * 0.16), int(W * 0.22), int(W * 0.012)), -28)
    compose(inner, oval, (W * 0.47, H * 0.32), int(W * 0.01))
    small = gear_art(int(W * 0.24), 7, (255, 250, 225))
    compose(inner, small, (W * 0.76, H * 0.18), int(W * 0.014), shadow=SMALL_SHADOW)
    # thanh trượt tùy chỉnh
    for i, (y, t, col) in enumerate(((0.515, 0.7, 'blue'), (0.565, 0.35, 'red'))):
        bar = Image.new('RGBA', (int(W * 0.46), int(W * 0.05)), (0, 0, 0, 0))
        d = ImageDraw.Draw(bar)
        d.rounded_rectangle([0, 0, bar.width - 1, bar.height - 1], radius=bar.height // 2,
                            fill=(255, 255, 255))
        d.rounded_rectangle([0, 0, int(bar.width * t), bar.height - 1], radius=bar.height // 2,
                            fill=COLORS[col])
        cx = W * 0.27 + bar.width / 2
        compose(inner, bar, (cx, H * y), int(W * 0.008))
        k = int(W * 0.075)
        knob = Image.new('RGBA', (k, k), (0, 0, 0, 0))
        ImageDraw.Draw(knob).ellipse([0, 0, k - 1, k - 1], fill=WHITE)
        compose(inner, knob, (W * 0.27 + bar.width * t, H * y), int(W * 0.010), shadow=SMALL_SHADOW)
    add_title(inner, [('CUSTOM', 1.0), ('ROOM', 0.8)], (W * 0.5, H * 0.77))
    return finish(inner, [(0.15, 0.13, 0.06), (0.20, 0.44, 0.05), (0.86, 0.46, 0.06)])


# ---------------------------------------------------------------- main

def main():
    if not (CARDS_DIR / 'back.png').exists():
        sys.exit('Chưa có bộ bài - chạy gen_cards.py trước.')
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    modes = {'classic': classic(), '2v2': two_vs_two(), 'side': side_to_side(), 'custom': custom()}
    for name, img in modes.items():
        img.save(OUT_DIR / f'{name}.png', optimize=True)
    print(f'Generated {len(modes)} images in {OUT_DIR}')

    if '--preview' in sys.argv:
        tw, th = OUT_W // 2, OUT_H // 2
        sheet = Image.new('RGBA', (len(modes) * (tw + 16) + 16, th + 32), (22, 26, 48, 255))
        for i, img in enumerate(modes.values()):
            sheet.alpha_composite(img.resize((tw, th), Image.LANCZOS), (16 + i * (tw + 16), 16))
        path = Path(__file__).with_name('modes_preview.png')
        sheet.save(path)
        print(f'Preview: {path}')


if __name__ == '__main__':
    main()
