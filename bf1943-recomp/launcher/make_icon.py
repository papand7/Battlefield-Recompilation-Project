"""Draws the launcher's icon (icon.png 1024 px, icon.ico 16-256 px): the
launcher's "BF" badge, orange (#e8a03c) on its dark blue-black (#0b0f14),
lettered in Stardos Stencil (data/fonts, OFL), with a thin orange frame.

  python make_icon.py        (needs Pillow; writes next to itself)
"""
import os

from PIL import Image, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
FONT = os.path.join(HERE, '..', 'data', 'fonts', 'StardosStencil-Bold.ttf')
S = 1024
BG, ACCENT, ACCENT_DARK = (11, 15, 20), (232, 160, 60), (170, 104, 30)


def draw(size):
    k = size / S
    img = Image.new('RGBA', (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    r = int(190 * k)  # (rounded corners, as the taskbar's other icons)
    # The plate: dark, a little lighter towards the top.
    plate = Image.new('RGBA', (size, size), BG + (255,))
    shade = Image.new('L', (size, size))
    ImageDraw.Draw(shade).rectangle([0, 0, size, size // 2], fill=40)
    shade = shade.filter(ImageFilter.GaussianBlur(size / 6))
    plate.paste((40, 52, 66, 255), (0, 0), shade)
    mask = Image.new('L', (size, size))
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, size - 1, size - 1], r, fill=255)
    img.paste(plate, (0, 0), mask)
    # The orange frame.
    w = max(2, int(34 * k))
    m = int(70 * k)
    d.rounded_rectangle([m, m, size - 1 - m, size - 1 - m], int(120 * k), outline=ACCENT, width=w)
    # "BF": the badge's letters, a shadow under them.
    font = ImageFont.truetype(FONT, int(560 * k))
    text = 'BF'
    box = d.textbbox((0, 0), text, font=font)
    tx = (size - (box[2] - box[0])) / 2 - box[0]
    ty = (size - (box[3] - box[1])) / 2 - box[1] - 20 * k
    shadow = Image.new('RGBA', (size, size), (0, 0, 0, 0))
    ImageDraw.Draw(shadow).text((tx + 14 * k, ty + 18 * k), text, font=font, fill=(0, 0, 0, 170))
    img.alpha_composite(shadow.filter(ImageFilter.GaussianBlur(max(1, 10 * k))))
    # (lighter at the top, as metal in the light)
    letters = Image.new('L', (size, size))
    ImageDraw.Draw(letters).text((tx, ty), text, font=font, fill=255)
    grad = Image.linear_gradient('L').resize((size, size))
    fill = Image.composite(Image.new('RGBA', (size, size), ACCENT_DARK + (255,)),
                           Image.new('RGBA', (size, size), (248, 190, 96, 255)), grad)
    img.paste(fill, (0, 0), letters)
    # Three stars under the letters (the games' title cards).
    for i in (-1, 0, 1):
        cx, cy, rr = size / 2 + i * 120 * k, size - 175 * k, 34 * k
        import math
        pts = [(cx + (rr if j % 2 == 0 else rr * 0.42) * math.sin(j * math.pi / 5),
                cy - (rr if j % 2 == 0 else rr * 0.42) * math.cos(j * math.pi / 5)) for j in range(10)]
        d.polygon(pts, fill=ACCENT)
    return img


big = draw(S)
big.save(os.path.join(HERE, 'icon.png'))
# The Windows icon: each size drawn on its own (crisp small sizes).
sizes = [16, 24, 32, 48, 64, 128, 256]
imgs = [draw(n) if n >= 48 else draw(256).resize((n, n), Image.LANCZOS) for n in sizes]
imgs[-1].save(os.path.join(HERE, 'icon.ico'), sizes=[(n, n) for n in sizes], append_images=imgs[:-1])
print('icon.png, icon.ico')
