#!/usr/bin/env python3
"""
Sinh ảnh bộ bài UNO (58 mặt trước gồm discard_all + back.png) vào client/public/images/cards/.

Chạy:   python client/scripts/gen_cards.py [--preview]
Cần:    Pillow (pip install pillow), font Arial Black (sẵn trên Windows).
--preview  xuất thêm client/scripts/cards_preview.png (bảng xem nhanh toàn bộ lá).

Ảnh được vẽ ở độ phân giải SS lần rồi thu nhỏ về 480x720 để viền mịn.
"""
import math
import sys
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

ROOT = Path(__file__).resolve().parents[1]
OUT_DIR = ROOT / 'public' / 'images' / 'cards'

OUT_W, OUT_H = 480, 720
SS = 4
W, H = OUT_W * SS, OUT_H * SS

FONT_CANDIDATES = [
    'C:/Windows/Fonts/ariblk.ttf',
    '/Library/Fonts/Arial Black.ttf',
    '/usr/share/fonts/truetype/msttcorefonts/Arial_Black.ttf',
]
FONT_PATH = next((p for p in FONT_CANDIDATES if Path(p).exists()), None)
if FONT_PATH is None:
    sys.exit('Arial Black font not found - edit FONT_CANDIDATES.')

COLORS = {
    'red': (227, 48, 28),
    'yellow': (248, 182, 0),
    'green': (18, 180, 91),
    'blue': (30, 117, 235),
}
WILD_BG = (26, 26, 26)
BLACK = (0, 0, 0)
WHITE = (255, 255, 255)

BORDER = int(W * 0.055)        # viền trắng
RADIUS = int(W * 0.11)         # bo góc lá bài
OVAL_W, OVAL_H = int(W * 0.74), int(H * 0.90)
OVAL_ANGLE = -28               # âm = nghiêng theo chiều kim đồng hồ (đỉnh lệch phải)
CORNER = (W * 0.19, H * 0.135) # tâm ký hiệu góc trên-trái
CENTER = (W / 2, H / 2)

STROKE = int(W * 0.028)
SHADOW = (int(W * 0.012), int(W * 0.016), W * 0.012, 0.55)
CORNER_STROKE = int(W * 0.018)
CORNER_SHADOW = (int(W * 0.006), int(W * 0.008), W * 0.006, 0.45)


# ---------------------------------------------------------------- helpers

def font(size):
    return ImageFont.truetype(FONT_PATH, int(size))


def mix(c1, c2, t):
    return tuple(round(a + (b - a) * t) for a, b in zip(c1, c2))


def vgrad(size, top, bottom):
    g = Image.linear_gradient('L').resize(size)
    return Image.composite(Image.new('RGB', size, bottom), Image.new('RGB', size, top), g)


def solid(size, color, alpha):
    im = Image.new('RGBA', size, color + (255,))
    im.putalpha(alpha)
    return im


def colorize(mask, color):
    return solid(mask.size, color, mask)


def crop_alpha(img):
    return img.crop(img.getchannel('A').getbbox())


def shear(img, k=0.14):
    """Nghiêng chữ kiểu italic (đỉnh lệch phải)."""
    w, h = img.size
    extra = int(k * h) + 1
    out = img.transform((w + extra, h), Image.AFFINE, (1, k, -extra, 0, 1, 0),
                        resample=Image.BICUBIC)
    return crop_alpha(out)


def rotate(img, deg):
    return crop_alpha(img.rotate(deg, resample=Image.BICUBIC, expand=True))


def dilate(alpha, r):
    """Nở mask r pixel (xấp xỉ bằng blur + ngưỡng mềm) — dùng làm viền đen."""
    if r <= 0:
        return alpha
    b = alpha.filter(ImageFilter.GaussianBlur(r / 2.2))
    return b.point(lambda v: 0 if v < 2 else min(255, (v - 2) * 42))


def compose(canvas, art, center, stroke=0, stroke_color=BLACK, shadow=None, extrude=0):
    """Dán art vào canvas tại center, kèm bóng đổ, khối 3D (extrude) và viền."""
    layer = Image.new('RGBA', canvas.size, (0, 0, 0, 0))
    cx, cy = center
    layer.paste(art, (round(cx - art.width / 2), round(cy - art.height / 2)), art)
    a = layer.getchannel('A')
    ol = dilate(a, stroke)
    if shadow:
        dx, dy, blur, op = shadow
        sh = ImageChops.offset(ol, dx + extrude, dy + extrude).filter(ImageFilter.GaussianBlur(blur))
        canvas.alpha_composite(solid(canvas.size, BLACK, sh.point(lambda v: int(v * op))))
    for i in range(extrude, 0, -max(1, extrude // 12)):
        canvas.alpha_composite(solid(canvas.size, mix(stroke_color, BLACK, 0.3), ImageChops.offset(ol, i, i)))
    if stroke:
        canvas.alpha_composite(solid(canvas.size, stroke_color, ol))
    canvas.alpha_composite(layer)


# ---------------------------------------------------------------- art

def text_art(txt, size, color, underline=False, italic=0.12):
    f = font(size)
    l, t, r, b = f.getbbox(txt)
    pad = int(size * 0.3)
    m = Image.new('L', (r - l + 2 * pad, b - t + 2 * pad + int(size * 0.25)), 0)
    d = ImageDraw.Draw(m)
    d.text((pad - l, pad - t), txt, font=f, fill=255)
    if underline:
        uw, uh = int((r - l) * 0.85), int(size * 0.075)
        cx, y = m.width // 2, pad + (b - t) + int(size * 0.07)
        d.rounded_rectangle([cx - uw // 2, y, cx + uw // 2, y + uh], radius=uh // 2, fill=255)
    art = crop_alpha(colorize(m, color))
    return shear(art, italic) if italic else art


def skip_art(s, color):
    m = Image.new('L', (s, s), 0)
    d = ImageDraw.Draw(m)
    t = int(s * 0.17)
    d.ellipse([0, 0, s - 1, s - 1], outline=255, width=t)
    c = s / 2
    off = (s / 2 - t / 2) * math.cos(math.pi / 4)
    d.line([(c - off, c - off), (c + off, c + off)], fill=255, width=t)
    return colorize(m, color)


def reverse_art(s, color):
    m = Image.new('L', (s, s), 0)
    d = ImageDraw.Draw(m)
    c, R, t = s / 2, s * 0.30, int(s * 0.14)
    head_len, head_w = s * 0.19, s * 0.16
    for a0, a1 in ((195, 318), (15, 138)):
        d.arc([c - R - t / 2, c - R - t / 2, c + R + t / 2, c + R + t / 2], a0, a1, fill=255, width=t)
        th = math.radians(a1)
        px, py = c + R * math.cos(th), c + R * math.sin(th)
        tx, ty = -math.sin(th), math.cos(th)   # hướng đi (chiều kim đồng hồ)
        nx, ny = math.cos(th), math.sin(th)
        px, py = px - tx * 2, py - ty * 2
        d.polygon([(px + tx * head_len, py + ty * head_len),
                   (px + nx * head_w, py + ny * head_w),
                   (px - nx * head_w, py - ny * head_w)], fill=255)
    return rotate(colorize(m, color), 30)


def mini_card_art(w, color):
    h = int(w * 1.5)
    r, b = int(w * 0.14), int(w * 0.09)
    im = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.rounded_rectangle([0, 0, w - 1, h - 1], radius=r, fill=WHITE)
    d.rounded_rectangle([b, b, w - 1 - b, h - 1 - b], radius=r - b // 2, fill=color)
    return im


def oval_mask(w, h, angle, size=(W, H), center=None):
    m = Image.new('L', size, 0)
    cx, cy = center or (size[0] / 2, size[1] / 2)
    ImageDraw.Draw(m).ellipse([cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2], fill=255)
    return m.rotate(angle, resample=Image.BICUBIC, center=(cx, cy))


def wild_oval_art(w, h, ring):
    """Oval 4 màu (đỏ, xanh dương, xanh lá, vàng) viền trắng, chưa xoay."""
    im = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.ellipse([0, 0, w - 1, h - 1], fill=WHITE)
    box = [ring, ring, w - 1 - ring, h - 1 - ring]
    for start, key in ((180, 'red'), (270, 'blue'), (0, 'green'), (90, 'yellow')):
        d.pieslice(box, start, start + 90, fill=COLORS[key])
    return im


# ---------------------------------------------------------------- card

def new_card(top, bottom):
    """Trả về (nền lá bài trắng, lớp nội dung bên trong, mask vùng trong)."""
    base = Image.new('RGBA', (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(base)
    d.rounded_rectangle([0, 0, W - 1, H - 1], radius=RADIUS, fill=(206, 206, 206))
    e = int(W * 0.006)
    d.rounded_rectangle([e, e, W - 1 - e, H - 1 - e], radius=RADIUS - e, fill=WHITE)
    inner_mask = Image.new('L', (W, H), 0)
    ImageDraw.Draw(inner_mask).rounded_rectangle(
        [BORDER, BORDER, W - 1 - BORDER, H - 1 - BORDER], radius=int(RADIUS * 0.62), fill=255)
    inner = vgrad((W, H), top, bottom).convert('RGBA')
    return base, inner, inner_mask


def finish(base, inner, inner_mask):
    inner.putalpha(inner_mask)
    base.alpha_composite(inner)
    return base.resize((OUT_W, OUT_H), Image.LANCZOS)


def add_corners(inner, art):
    compose(inner, art, CORNER, CORNER_STROKE, shadow=CORNER_SHADOW)
    flipped = art.rotate(180)
    compose(inner, flipped, (W - CORNER[0], H - CORNER[1]), CORNER_STROKE, shadow=CORNER_SHADOW)


def add_white_oval(inner):
    m = oval_mask(OVAL_W, OVAL_H, OVAL_ANGLE)
    inner.alpha_composite(solid((W, H), WHITE, m))


def mini_cards(inner, colors, center, spread, card_w):
    """Xòe quạt các lá bài nhỏ quanh center."""
    n = len(colors)
    for i, col in enumerate(colors):
        t = i - (n - 1) / 2
        ang = -t * spread
        art = rotate(mini_card_art(card_w, col), ang)
        cx = center[0] + t * card_w * 0.42
        cy = center[1] + abs(t) * card_w * 0.08
        compose(inner, art, (cx, cy), int(W * 0.014), shadow=CORNER_SHADOW)


def arc_arrow_art(s, a0, a1, color):
    """Mũi tên cong theo cung tròn từ góc a0 tới a1 (độ, chiều kim đồng hồ), đầu mũi ở a1."""
    m = Image.new('L', (s, s), 0)
    d = ImageDraw.Draw(m)
    c, R, t = s / 2, s * 0.36, int(s * 0.10)
    head_len, head_w = s * 0.20, s * 0.13
    d.arc([c - R - t / 2, c - R - t / 2, c + R + t / 2, c + R + t / 2], a0, a1, fill=255, width=t)
    th = math.radians(a1)
    px, py = c + R * math.cos(th), c + R * math.sin(th)
    tx, ty = -math.sin(th), math.cos(th)
    nx, ny = math.cos(th), math.sin(th)
    d.polygon([(px + tx * head_len, py + ty * head_len),
               (px + nx * head_w, py + ny * head_w),
               (px - nx * head_w, py - ny * head_w)], fill=255)
    return crop_alpha(colorize(m, color))


def card_stack_art(w, color, n=4):
    """Chồng bài nằm phẳng (phối cảnh nghiêng), mặt trên mang màu lá."""
    dh, sk, th = int(w * 0.46), int(w * 0.16), int(w * 0.075)
    lw = max(2, int(w * 0.022))
    im = Image.new('RGBA', (w + sk + lw * 2, dh + n * th + lw * 2), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)

    def face(y0, inset=0):
        return [(lw + sk + inset, y0 + inset), (lw + w + sk - inset, y0 + inset),
                (lw + w - inset, y0 + dh - inset), (lw + inset, y0 + dh - inset)]

    for i in range(n):
        y0 = lw + (n - 1 - i) * th
        d.polygon(face(y0), fill=WHITE, outline=BLACK, width=lw)
    d.polygon(face(lw, int(w * 0.06)), fill=color)
    return im


def discard_corner_art(color):
    """Ký hiệu góc: quạt 3 lá nhỏ + mũi tên cong."""
    s = int(W * 0.32)
    canvas = Image.new('RGBA', (s, s), (0, 0, 0, 0))
    cw = int(s * 0.24)
    for i, ang in enumerate((18, 4, -10)):
        art = rotate(mini_card_art(cw, color), ang)
        compose(canvas, art, (s * 0.26 + i * cw * 0.42, s * 0.44 + abs(i - 1) * cw * 0.06),
                int(W * 0.008))
    compose(canvas, arc_arrow_art(int(s * 0.46), 200, 330, color), (s * 0.70, s * 0.30), int(W * 0.008))
    return crop_alpha(canvas)


def color_card(color_key, value):
    c = COLORS[color_key]
    base, inner, im = new_card(mix(c, WHITE, 0.12), mix(c, BLACK, 0.18))
    add_white_oval(inner)

    if value.isdigit():
        ul = value in ('6', '9')
        compose(inner, text_art(value, W * 0.74, c, underline=ul), CENTER, STROKE, shadow=SHADOW)
        corner = text_art(value, W * 0.26, WHITE, underline=ul)
    elif value == 'skip':
        compose(inner, skip_art(int(W * 0.52), c), CENTER, STROKE, shadow=SHADOW)
        corner = skip_art(int(W * 0.17), WHITE)
    elif value == 'reverse':
        compose(inner, reverse_art(int(W * 0.60), c), CENTER, STROKE, shadow=SHADOW)
        corner = reverse_art(int(W * 0.21), WHITE)
    elif value == 'draw2':
        mini_cards(inner, [c, c], (W * 0.50, H * 0.39), 14, int(W * 0.25))
        compose(inner, text_art('+2', W * 0.30, c), (W * 0.52, H * 0.66), STROKE, shadow=SHADOW)
        corner = text_art('+2', W * 0.20, WHITE)
    elif value == 'discard_all':
        mini_cards(inner, [c] * 4, (W * 0.57, H * 0.33), 11, int(W * 0.20))
        compose(inner, arc_arrow_art(int(W * 0.36), 330, 120, BLACK), (W * 0.60, H * 0.52),
                shadow=CORNER_SHADOW)
        compose(inner, card_stack_art(int(W * 0.36), c), (W * 0.40, H * 0.70),
                int(W * 0.012), shadow=SHADOW)
        corner = discard_corner_art(WHITE)
    else:
        raise ValueError(value)

    add_corners(inner, corner)
    return finish(base, inner, im)


def wild_card(value):
    base, inner, im = new_card(mix(WILD_BG, WHITE, 0.08), mix(WILD_BG, BLACK, 0.4))
    ring = int(W * 0.035)
    if value == 'wild':
        oval = wild_oval_art(int(OVAL_W * 0.86), int(OVAL_H * 0.86), ring)
        compose(inner, rotate(oval, OVAL_ANGLE), CENTER, shadow=SHADOW)
        corner = rotate(wild_oval_art(int(W * 0.13), int(W * 0.19), int(W * 0.014)), OVAL_ANGLE)
        add_corners(inner, corner)
    else:
        colors = [COLORS[k] for k in ('blue', 'green', 'red', 'yellow')]
        mini_cards(inner, colors, (W * 0.50, H * 0.40), 16, int(W * 0.24))
        compose(inner, text_art('+4', W * 0.34, WHITE), (W * 0.52, H * 0.67), STROKE, shadow=SHADOW,
                extrude=int(W * 0.018))
        add_corners(inner, text_art('+4', W * 0.20, WHITE))
    return finish(base, inner, im)


def back_card():
    base, inner, im = new_card((38, 38, 42), (12, 12, 14))
    ow, oh = int(OVAL_W * 0.94), int(OVAL_H * 0.86)
    om = oval_mask(ow, oh, -26)
    glow = dilate(om, int(W * 0.03)).filter(ImageFilter.GaussianBlur(W * 0.025))
    inner.alpha_composite(solid((W, H), (255, 200, 40), glow))
    inner.alpha_composite(solid((W, H), (255, 236, 170), dilate(om, int(W * 0.012))))
    red = vgrad((W, H), (245, 70, 45), (170, 18, 18)).convert('RGBA')
    red.putalpha(om)
    inner.alpha_composite(red)
    # vệt bóng sáng trên oval
    gloss = oval_mask(int(ow * 0.8), int(oh * 0.42), -26, center=(W * 0.47, H * 0.36))
    gloss = ImageChops.multiply(gloss, om).filter(ImageFilter.GaussianBlur(W * 0.03))
    inner.alpha_composite(solid((W, H), WHITE, gloss.point(lambda v: int(v * 0.18))))

    logo = rotate(text_art('UNO', W * 0.36, (255, 214, 0), italic=0.16), 24)
    scale = (W * 0.68) / logo.width
    logo = logo.resize((int(logo.width * scale), int(logo.height * scale)), Image.LANCZOS)
    compose(inner, logo, CENTER, int(W * 0.03), shadow=SHADOW, extrude=int(W * 0.03))
    return finish(base, inner, im)


# ---------------------------------------------------------------- main

VALUES = [str(i) for i in range(10)] + ['skip', 'reverse', 'draw2', 'discard_all']


def main():
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    cards = {'back': back_card()}
    for key in COLORS:
        for v in VALUES:
            cards[f'{key}_{v}'] = color_card(key, v)
    cards['wild_wild'] = wild_card('wild')
    cards['wild_wild4'] = wild_card('wild4')

    for name, img in cards.items():
        img.save(OUT_DIR / f'{name}.png', optimize=True)
    print(f'Generated {len(cards)} images in {OUT_DIR}')

    if '--preview' in sys.argv:
        cols, tw, th = 14, OUT_W // 3, OUT_H // 3
        imgs = list(cards.values())
        rows = math.ceil(len(imgs) / cols)
        sheet = Image.new('RGBA', (cols * (tw + 8) + 8, rows * (th + 8) + 8), (60, 70, 90, 255))
        for i, img in enumerate(imgs):
            t = img.resize((tw, th), Image.LANCZOS)
            sheet.alpha_composite(t, (8 + (i % cols) * (tw + 8), 8 + (i // cols) * (th + 8)))
        path = Path(__file__).with_name('cards_preview.png')
        sheet.save(path)
        print(f'Preview: {path}')


if __name__ == '__main__':
    main()
