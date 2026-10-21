#!/usr/bin/env python3
"""
Mjolnir VR's pictures, drawn from nothing but shapes and the
port's Overpass font (port/assets/fonts, SIL Open Font License):

    python3 tools/mjolnir_art.py

writes
- port/android/art/android-icon.png, the launcher icon's artwork (then run
  tools/android_icon.py for the app's icon);
- port/android/steam_frame_grid/*.png, the Steam library's pictures of the
  Steam Frame's shortcut (port/android/STEAM_FRAME.md): library.png
  (920x430), libraryp.png (600x900), library_hero.png (1920x620) and
  library_logo.png (800x300, transparent).

The emblem: a tilted ring, broken where a streak falls through it.
"""

import math
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = Path(__file__).resolve().parent.parent
FONT = ROOT / "port/assets/fonts/Overpass-900.ttf"
ICON = ROOT / "port/android/art/android-icon.png"
GRID = ROOT / "port/android/steam_frame_grid"

# the launcher icon's background (tools/android_icon.py keeps it)
ICON_BACKGROUND = (30, 83, 138)
LIGHT = (240, 246, 252)
ACCENT = (255, 176, 64)
TOP = (10, 34, 58)
BOTTOM = (6, 12, 24)
SUPERSAMPLE = 4


def gradient(size, top, bottom):
    width, height = size
    image = Image.new("RGBA", size)
    draw = ImageDraw.Draw(image)
    for y in range(height):
        t = y / max(1, height - 1)
        draw.line([(0, y), (width, y)], fill=tuple(int(a + (b - a) * t) for a, b in zip(top, bottom)) + (255,))
    return image


def emblem(size, ring=LIGHT, streak=ACCENT):
    """the emblem on a transparent square of the size given"""
    scale = size * SUPERSAMPLE
    layer = Image.new("RGBA", (scale, scale), (0, 0, 0, 0))
    draw = ImageDraw.Draw(layer)
    center = scale / 2
    radius_x, radius_y = scale * 0.40, scale * 0.17
    thickness = scale * 0.055
    tilt = math.radians(-18)
    # the ring as a band of quads along a tilted ellipse, with a gap at the
    # top right where the streak passes
    steps = 360
    gap = (math.radians(35), math.radians(75))
    def point(angle, grow):
        x = (radius_x + grow) * math.cos(angle)
        y = (radius_y + grow * radius_y / radius_x) * math.sin(angle)
        return (center + x * math.cos(tilt) - y * math.sin(tilt), center + x * math.sin(tilt) + y * math.cos(tilt))
    for step in range(steps):
        a0 = 2 * math.pi * step / steps
        a1 = 2 * math.pi * (step + 1.2) / steps
        # (angles measured clockwise from the right, screen y down: the gap
        # at the upper right is at negative angles)
        if gap[0] <= (2 * math.pi - a0) % (2 * math.pi) <= gap[1]:
            continue
        draw.polygon([point(a0, -thickness / 2), point(a1, -thickness / 2), point(a1, thickness / 2),
            point(a0, thickness / 2)], fill=ring)
    # the streak: a tapering bar falling from the upper right through the gap
    start = (center + scale * 0.40, center - scale * 0.42)
    end = (center + scale * 0.02, center + scale * 0.12)
    dx, dy = end[0] - start[0], end[1] - start[1]
    length = math.hypot(dx, dy)
    nx, ny = -dy / length, dx / length
    head = scale * 0.050
    draw.polygon([start, (end[0] + nx * head, end[1] + ny * head), (end[0] - nx * head, end[1] - ny * head)],
        fill=streak)
    draw.ellipse([end[0] - head, end[1] - head, end[0] + head, end[1] + head], fill=streak)
    return layer.resize((size, size), Image.LANCZOS)


def italic_text(text, height, fill):
    """the text in Overpass Black, slanted, cropped to itself"""
    font = ImageFont.truetype(str(FONT), int(height * SUPERSAMPLE))
    left, top, right, bottom = font.getbbox(text)
    width = right - left + int(height * SUPERSAMPLE * 0.4)
    layer = Image.new("RGBA", (width, bottom - top + 8), (0, 0, 0, 0))
    ImageDraw.Draw(layer).text((-left + int(height * SUPERSAMPLE * 0.2), -top), text, font=font, fill=fill)
    slant = 0.18
    layer = layer.transform((layer.width + int(layer.height * slant), layer.height), Image.AFFINE,
        (1, slant, -layer.height * slant, 0, 1, 0), Image.BICUBIC)
    layer = layer.crop(layer.getbbox())
    return layer.resize((max(1, layer.width // SUPERSAMPLE), max(1, layer.height // SUPERSAMPLE)), Image.LANCZOS)


def logo(width):
    """"MJOLNIR" over "VR", as wide as asked, transparent"""
    title = italic_text("MJOLNIR", 200, LIGHT)
    title = title.resize((width, int(title.height * width / title.width)), Image.LANCZOS)
    subtitle = italic_text("VR", 200, ACCENT)
    sub_width = int(width * 0.30)
    subtitle = subtitle.resize((sub_width, int(subtitle.height * sub_width / subtitle.width)), Image.LANCZOS)
    gap = int(title.height * 0.16)
    image = Image.new("RGBA", (width, title.height + gap + subtitle.height), (0, 0, 0, 0))
    image.alpha_composite(title, (0, 0))
    image.alpha_composite(subtitle, (width - subtitle.width, title.height + gap))
    return image


def shadowed(base, layer, position, blur=12):
    shadow = Image.new("RGBA", base.size, (0, 0, 0, 0))
    mask = Image.new("RGBA", layer.size, (0, 0, 0, 170))
    mask.putalpha(layer.getchannel("A").point(lambda a: a * 2 // 3))
    shadow.alpha_composite(mask, position)
    base.alpha_composite(shadow.filter(ImageFilter.GaussianBlur(blur)))
    base.alpha_composite(layer, position)


def faded(layer, opacity):
    layer = layer.copy()
    layer.putalpha(layer.getchannel("A").point(lambda a: int(a * opacity)))
    return layer


def main():
    # the launcher icon: the light emblem on its solid background
    icon = Image.new("RGBA", (512, 512), ICON_BACKGROUND + (255,))
    icon.alpha_composite(emblem(400, LIGHT, LIGHT), (56, 56))
    ICON.parent.mkdir(parents=True, exist_ok=True)
    icon.convert("RGB").save(ICON, optimize=True)
    print(ICON.relative_to(ROOT))

    GRID.mkdir(parents=True, exist_ok=True)

    # the wide capsule
    image = gradient((920, 430), TOP, BOTTOM)
    image.alpha_composite(faded(emblem(520), 0.35), (500, -40))
    shadowed(image, logo(600), (70, 120))
    image.save(GRID / "library.png", optimize=True)

    # the tall capsule
    image = gradient((600, 900), TOP, BOTTOM)
    image.alpha_composite(emblem(420), (90, 130))
    shadowed(image, logo(480), (60, 560))
    image.save(GRID / "libraryp.png", optimize=True)

    # the hero, under the logo: the emblem large and faint at the right
    image = gradient((1920, 620), TOP, BOTTOM)
    image.alpha_composite(faded(emblem(900), 0.45), (1080, -140))
    image.save(GRID / "library_hero.png", optimize=True)

    # the logo, over the hero
    image = Image.new("RGBA", (800, 300), (0, 0, 0, 0))
    mark = logo(560)
    image.alpha_composite(mark, ((800 - mark.width) // 2, (300 - mark.height) // 2))
    image.save(GRID / "library_logo.png", optimize=True)
    for name in ("library.png", "libraryp.png", "library_hero.png", "library_logo.png"):
        print((GRID / name).relative_to(ROOT))


if __name__ == "__main__":
    main()
