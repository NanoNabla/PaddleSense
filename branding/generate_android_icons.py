#!/usr/bin/env python3
"""Generate the Android launcher icons for PaddleSense from the source artwork.

Source of truth: ``branding/PaddleSense.png`` (a 1254x1254 rounded-square icon
with a transparent margin).  This script crops the transparent margin and emits:

* legacy ``mipmap-<dpi>/ic_launcher.png`` (+ ``ic_launcher_round.png``) for
  launchers that do not understand adaptive icons, and
* adaptive-icon foreground layers ``mipmap-<dpi>/ic_launcher_foreground.png``
  on the 108dp grid, with the artwork scaled into the 66dp safe zone so the
  paddle blades are never clipped by circular/squircle masks.

The adaptive background is a solid colour (see ``values/colors.xml``) sampled
from the artwork's own background blue, so the rounded corners of the artwork
blend seamlessly into the background layer.

Usage::

    python3 branding/generate_android_icons.py

Requires Pillow (``pip install Pillow``).
"""

from __future__ import annotations

import os
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
SRC = os.path.join(HERE, "PaddleSense.png")
RES = os.path.join(REPO, "android", "app", "src", "main", "res")

# density bucket -> (legacy icon px, adaptive foreground px)
DENSITIES = {
    "mdpi": (48, 108),
    "hdpi": (72, 162),
    "xhdpi": (96, 216),
    "xxhdpi": (144, 324),
    "xxxhdpi": (192, 432),
}

# Adaptive icons are a 108dp canvas; the system guarantees the central 66dp is
# always visible, so we fit the artwork inside that safe zone.
ADAPTIVE_CANVAS_DP = 108
SAFE_ZONE_DP = 66


def load_content() -> Image.Image:
    """Return the artwork cropped to its opaque content, as RGBA.

    A threshold is used rather than ``getbbox()`` so that faint anti-aliased
    pixels (e.g. a soft shadow along the bottom edge) do not inflate the crop.
    """
    im = Image.open(SRC).convert("RGBA")
    alpha = im.getchannel("A").point(lambda a: 255 if a > 128 else 0)
    bbox = alpha.getbbox()
    if bbox is None:
        raise SystemExit(f"no opaque content found in {SRC}")
    return im.crop(bbox)


def sample_background_blue(content: Image.Image) -> str:
    """Sample the artwork's background blue from the top strip of the content.

    Only fully opaque pixels are considered, and the median is used so the
    white WiFi arcs and the transparent rounded corners do not skew the result.
    """
    w, h = content.size
    strip = content.crop((0, 0, w, max(1, h // 12)))
    pixels = [
        (r, g, b)
        for (r, g, b, a) in strip.getdata()
        if a > 200
    ]
    if not pixels:
        raise SystemExit("could not sample a background colour")
    r = sorted(p[0] for p in pixels)[len(pixels) // 2]
    g = sorted(p[1] for p in pixels)[len(pixels) // 2]
    b = sorted(p[2] for p in pixels)[len(pixels) // 2]
    return "#%02X%02X%02X" % (r, g, b)


def square(img: Image.Image, size: int) -> Image.Image:
    """Resize ``img`` to ``size`` x ``size`` with high-quality resampling."""
    return img.resize((size, size), Image.LANCZOS)


def circular(img: Image.Image) -> Image.Image:
    """Return a circular-cropped copy of a square RGBA image."""
    size = img.size[0]
    mask = Image.new("L", (size, size), 0)
    from PIL import ImageDraw

    ImageDraw.Draw(mask).ellipse((0, 0, size - 1, size - 1), fill=255)
    out = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    out.paste(img, (0, 0), mask)
    return out


def make_foreground(content: Image.Image, canvas_px: int) -> Image.Image:
    """Place the artwork inside the adaptive-icon safe zone on a transparent canvas."""
    inner = round(canvas_px * SAFE_ZONE_DP / ADAPTIVE_CANVAS_DP)
    art = square(content, inner)
    canvas = Image.new("RGBA", (canvas_px, canvas_px), (0, 0, 0, 0))
    off = (canvas_px - inner) // 2
    canvas.paste(art, (off, off), art)
    return canvas


def main() -> None:
    content = load_content()
    blue = sample_background_blue(content)
    print(f"content size: {content.size[0]}x{content.size[1]}")
    print(f"sampled background blue: {blue}")

    for density, (legacy_px, fg_px) in DENSITIES.items():
        out_dir = os.path.join(RES, f"mipmap-{density}")
        os.makedirs(out_dir, exist_ok=True)

        legacy = square(content, legacy_px)
        legacy.save(os.path.join(out_dir, "ic_launcher.png"))
        circular(legacy).save(os.path.join(out_dir, "ic_launcher_round.png"))

        make_foreground(content, fg_px).save(
            os.path.join(out_dir, "ic_launcher_foreground.png")
        )
        print(f"  mipmap-{density}: legacy {legacy_px}px, foreground {fg_px}px")

    print("done")


if __name__ == "__main__":
    main()
