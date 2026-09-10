#!/usr/bin/env python3
"""art/make-icons.py -- regenerate every raster asset from art/omega-icon.svg.

The SVG is the source. Everything else here is derived, so a change to the mark
is one edit followed by one run of this, rather than a hunt through PNGs.

    pip install cairosvg pillow
    python3 art/make-icons.py

Produces, beside this script:

    omega-icon-1024.png        full bleed, square
    omega-icon-macos-1024.png  the same at 80% on a transparent canvas
    Omega.iconset/             the ten files iconutil wants
    omega.ico                  Windows
    omega-banner-about.png     the banner, downscaled for the About dialog

The .icns itself is NOT built here: iconutil is macOS only. On the Mac,

    iconutil -c icns art/Omega.iconset -o art/Omega.icns

WHY THE MACOS ONES ARE INSET. Apple's own icons are a rounded rect occupying
about 80% of the canvas, with the rest transparent for the shadow. An icon that
fills its canvas edge to edge sits visibly larger than everything around it in
the Dock. Windows has no such convention, so omega.ico is full bleed.
"""

import os
import cairosvg
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
SVG = os.path.join(HERE, "omega-icon.svg")
BANNER = os.path.join(HERE, "omega-banner.png")

# Rendered large and downsampled rather than rasterized per size: cairosvg's
# own antialiasing at 16px is coarser than Lanczos from 1024.
MASTER = 1024
MACOS_FILL = 0.80


def render(px):
    out = os.path.join(HERE, "_master.png")
    cairosvg.svg2png(url=SVG, write_to=out, output_width=px, output_height=px)
    img = Image.open(out).convert("RGBA")
    os.remove(out)
    return img


def inset(img, canvas, fill):
    side = int(canvas * fill)
    scaled = img.resize((side, side), Image.LANCZOS)
    out = Image.new("RGBA", (canvas, canvas), (0, 0, 0, 0))
    out.paste(scaled, ((canvas - side) // 2, (canvas - side) // 2), scaled)
    return out


def main():
    master = render(MASTER)
    master.save(os.path.join(HERE, "omega-icon-1024.png"))

    mac = inset(master, MASTER, MACOS_FILL)
    mac.save(os.path.join(HERE, "omega-icon-macos-1024.png"))

    iconset = os.path.join(HERE, "Omega.iconset")
    os.makedirs(iconset, exist_ok=True)
    for size in (16, 32, 128, 256, 512):
        mac.resize((size, size), Image.LANCZOS).save(
            os.path.join(iconset, f"icon_{size}x{size}.png"))
        mac.resize((size * 2, size * 2), Image.LANCZOS).save(
            os.path.join(iconset, f"icon_{size}x{size}@2x.png"))

    master.save(os.path.join(HERE, "omega.ico"),
                sizes=[(16, 16), (24, 24), (32, 32), (48, 48),
                       (64, 64), (128, 128), (256, 256)])

    # Compiled into the binary through app/omega.qrc, so it is sized for the
    # dialog rather than shipped at full resolution: 1.5 MB of banner in every
    # build to fill a 640px wide label is 1.5 MB nobody asked for.
    if os.path.exists(BANNER):
        banner = Image.open(BANNER).convert("RGBA")
        width = 640
        height = round(banner.height * width / banner.width)
        banner.resize((width, height), Image.LANCZOS).save(
            os.path.join(HERE, "omega-banner-about.png"))
        print(f"banner: {width}x{height}")
    else:
        print(f"banner: {BANNER} not found, skipped")

    # 256 is what the window icon uses; Qt scales down from it.
    master.resize((256, 256), Image.LANCZOS).save(
        os.path.join(HERE, "omega-icon-256.png"))

    msix(master)

    print("icons: done")


def msix(master):
    """The tile and logo assets an MSIX manifest names.

    Windows will not pack a package whose manifest points at a file that is not
    there, and it names one missing asset at a time -- so generating the whole
    set at once is the difference between one run of makeappx and six.

    Scale variants exist because Windows picks by display DPI: .scale-200 is
    what a 4K laptop actually shows, and without it Windows upscales the 100%
    asset and the tile looks soft next to everything else on the Start menu.

    FULL BLEED HERE, not the 80% macOS inset. Windows draws its own tile
    background behind these and the artwork is expected to fill the space; the
    inset that makes a Dock icon sit correctly makes a Start tile look small.
    """
    out = os.path.join(HERE, "msix")
    os.makedirs(out, exist_ok=True)

    def square(name, side, scales=(100, 200)):
        for scale in scales:
            px = round(side * scale / 100)
            suffix = "" if scale == 100 else f".scale-{scale}"
            master.resize((px, px), Image.LANCZOS).save(
                os.path.join(out, f"{name}{suffix}.png"))

    square("Square44x44Logo", 44)
    square("Square71x71Logo", 71)
    square("Square150x150Logo", 150)
    square("Square310x310Logo", 310)
    square("StoreLogo", 50)

    # The taskbar and Alt-Tab read these rather than the scale variants, and
    # unplated means no coloured square behind the icon -- which is what makes
    # a transparent-cornered icon look right on the taskbar instead of sitting
    # in a tile.
    for px in (16, 24, 32, 48, 256):
        master.resize((px, px), Image.LANCZOS).save(os.path.join(
            out, f"Square44x44Logo.targetsize-{px}_altform-unplated.png"))

    # The wide tile is the one place the banner fits: 310x150 is 2.07:1 and the
    # banner is 2.36:1, so it is a centre crop rather than a letterbox.
    if os.path.exists(BANNER):
        banner = Image.open(BANNER).convert("RGBA")
        for scale in (100, 200):
            w, h = round(310 * scale / 100), round(150 * scale / 100)
            target = w / h
            cw = round(banner.height * target)
            left = (banner.width - cw) // 2
            suffix = "" if scale == 100 else f".scale-{scale}"
            banner.crop((left, 0, left + cw, banner.height)).resize(
                (w, h), Image.LANCZOS).save(
                    os.path.join(out, f"Wide310x150Logo{suffix}.png"))

    print(f"msix: {len(os.listdir(out))} assets in art/msix/")


if __name__ == "__main__":
    main()