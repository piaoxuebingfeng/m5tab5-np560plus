#!/usr/bin/env python3
"""Step 2: build the upper-half NP560+ body skin as a little-endian RGB565 raw.

Pipeline (offline only; no firmware change, no flashing):
  crop y=86..780 (1254x694) -> fill LCD hole with Web --lcd-off (#b7c8a1)
  -> strip green chroma-key residue around the hole -> alpha-composite on black
  -> quantise to RGB565 LE -> app/assets/skin.raw

The hole is deliberately filled slightly beyond its measured rectangle: the PNG
hole edge is feathered (alpha falls off over ~10 px), so a hard rectangle would
leave a grey/green halo after the skin is scaled onto the Tab5 canvas. Web's
.lcd-window uses the same trick (an lcd-off background slightly larger than the
transparent hole).

Also prints a static coordinate audit for step 3 (full-screen compositing).

Usage:
  /usr/bin/python3 tools/build_skin_raw.py
  /usr/bin/python3 tools/build_skin_raw.py --src <png> --out <raw> --json
"""
from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_SRC = Path(
    "/media/qiao/3b56c13d-bedc-441c-8da8-b6030fb755cb/github/Noah/"
    "np560-plus-emulator/scripts/web/assets/np560plus-open-cutout.png"
)
DEFAULT_OUT = ROOT / "app" / "assets" / "skin.raw"

SRC_SIZE = (1254, 1254)
CROP_TOP, CROP_BOTTOM = 86, 780          # -> 1254 x 694, near 16:9
HOLE_X0, HOLE_Y0, HOLE_X1, HOLE_Y1 = 349, 258, 930, 638   # source-image coords
HOLE_PAD = 6                             # cover the feathered hole edge
GREEN_RING = 24                          # only clean chroma key near the hole
GREEN_DELTA = 12                         # G > R+12 and G > B+12 => greenish

LCD_OFF = (0xB7, 0xC8, 0xA1)            # Web styles.css --lcd-off
COMPOSITE_BG = (0x00, 0x00, 0x00)       # Tab5 full-screen canvas background

SCREEN_W, SCREEN_H = 1280, 720
FRAME_W, FRAME_H = 240, 132             # 120 main + 12 annunciator rows


def to_rgb565(r: int, g: int, b: int) -> int:
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def build(src: Path, out: Path) -> dict:
    if not src.is_file():
        raise SystemExit(f"source PNG not found: {src}")
    im = Image.open(src)
    if im.size != SRC_SIZE:
        raise SystemExit(f"unexpected source size {im.size}, expected {SRC_SIZE}")
    rgba = np.asarray(im.convert("RGBA"), dtype=np.uint8)

    crop = rgba[CROP_TOP:CROP_BOTTOM].copy()   # (694, 1254, 4)
    h, w = crop.shape[:2]
    hx0, hy0 = HOLE_X0, HOLE_Y0 - CROP_TOP
    hx1, hy1 = HOLE_X1, HOLE_Y1 - CROP_TOP

    fx0, fx1 = max(0, hx0 - HOLE_PAD), min(w, hx1 + HOLE_PAD)
    fy0, fy1 = max(0, hy0 - HOLE_PAD), min(h, hy1 + HOLE_PAD)

    # 1. Remove green chroma-key residue in the ring OUTSIDE the hole fill.
    #    This must run BEFORE filling: LCD_OFF itself is slightly green
    #    dominant (G-R=17, G-B=39) and would otherwise be counted as residue.
    gx0, gx1 = max(0, hx0 - GREEN_RING), min(w, hx1 + GREEN_RING)
    gy0, gy1 = max(0, hy0 - GREEN_RING), min(h, hy1 + GREEN_RING)
    ring = crop[gy0:gy1, gx0:gx1]
    rr = ring[:, :, 0].astype(np.int16)
    gg = ring[:, :, 1].astype(np.int16)
    bb = ring[:, :, 2].astype(np.int16)
    aa = ring[:, :, 3]
    already_filled = np.zeros((gy1 - gy0, gx1 - gx0), dtype=bool)
    already_filled[fy0 - gy0:fy1 - gy0, fx0 - gx0:fx1 - gx0] = True
    green = ((gg > rr + GREEN_DELTA) & (gg > bb + GREEN_DELTA) &
             (aa > 40) & ~already_filled)
    green_count = int(green.sum())
    ring[:, :, 0][green] = LCD_OFF[0]
    ring[:, :, 1][green] = LCD_OFF[1]
    ring[:, :, 2][green] = LCD_OFF[2]

    # 2. Fill hole (plus a small margin) with LCD-off, fully opaque. The PNG
    #    hole edge is feathered; the small overlap removes the grey/green halo.
    crop[fy0:fy1, fx0:fx1, 0] = LCD_OFF[0]
    crop[fy0:fy1, fx0:fx1, 1] = LCD_OFF[1]
    crop[fy0:fy1, fx0:fx1, 2] = LCD_OFF[2]
    crop[fy0:fy1, fx0:fx1, 3] = 255

    # 3. Alpha-composite onto black, then quantise to RGB565 little-endian.
    rgb = crop[:, :, :3].astype(np.float32)
    alpha = (crop[:, :, 3:4].astype(np.float32) / 255.0)
    bg = np.asarray(COMPOSITE_BG, dtype=np.float32).reshape(1, 1, 3)
    comp = (rgb * alpha + bg * (1.0 - alpha)).clip(0, 255).astype(np.uint8)
    r = comp[:, :, 0].astype(np.uint16)
    g = comp[:, :, 1].astype(np.uint16)
    b = comp[:, :, 2].astype(np.uint16)
    pixels = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)

    out.parent.mkdir(parents=True, exist_ok=True)
    pixels.astype("<u2").tofile(out)
    raw = np.fromfile(out, dtype="<u2").reshape(h, w)

    # 4. Static audit -------------------------------------------------------
    expected_off = to_rgb565(*LCD_OFF)
    samples = {
        "hole_center": int(raw[(hy0 + hy1) // 2, (hx0 + hx1) // 2]),
        "hole_corner": int(raw[hy0 + 3, hx0 + 3]),
        "case_top": int(raw[20, 627]),
        "case_left": int(raw[300, 100]),
    }
    assert samples["hole_center"] == expected_off, "hole centre is not LCD-off"
    assert samples["hole_corner"] == expected_off, "hole corner is not LCD-off"

    # Coordinate map for step 3: source -> crop -> 1280x720 canvas -> frame.
    scale = SCREEN_W / w                       # 1280/1254
    skin_h = h * scale
    off_y = (SCREEN_H - skin_h) / 2.0
    hole_x0s, hole_x1s = hx0 * scale, hx1 * scale
    hole_y0s = hy0 * scale + off_y
    hole_y1s = hy1 * scale + off_y
    hole_ws, hole_hs = hole_x1s - hole_x0s, hole_y1s - hole_y0s
    frame_scale = hole_ws / FRAME_W
    frame_ws = FRAME_W * frame_scale
    frame_hs = FRAME_H * frame_scale
    frame_xs = hole_x0s
    frame_ys = hole_y0s + (hole_hs - frame_hs) / 2.0

    sha = hashlib.sha256(out.read_bytes()).hexdigest()
    return {
        "src": str(src),
        "out": str(out),
        "src_size": list(im.size),
        "crop": [0, CROP_TOP, w, CROP_BOTTOM],
        "skin_size": [w, h],
        "hole_in_crop": [hx0, hy0, hx1, hy1],
        "hole_fill_rect": [fx0, fy0, fx1, fy1],
        "green_pixels_removed": green_count,
        "lcd_off_rgb565": f"0x{expected_off:04x}",
        "samples": {k: f"0x{v:04x}" for k, v in samples.items()},
        "out_bytes": out.stat().st_size,
        "out_sha256": sha,
        "layout": {
            "skin_scale": round(scale, 6),
            "skin_on_screen": [0, round(off_y, 2), SCREEN_W, round(skin_h, 2)],
            "hole_on_screen": [round(v, 2) for v in
                               (hole_x0s, hole_y0s, hole_x1s, hole_y1s)],
            "hole_size_on_screen": [round(hole_ws, 2), round(hole_hs, 2)],
            "frame_scale": round(frame_scale, 4),
            "frame_size_on_screen": [round(frame_ws, 2), round(frame_hs, 2)],
            "frame_origin_on_screen": [round(frame_xs, 2), round(frame_ys, 2)],
            "hole_top_bottom_margin": round((hole_hs - frame_hs) / 2.0, 2),
        },
        # Documented old values in NP560_SKIN_ASSET_EVALUATION.md 11.1 forgot
        # to subtract CROP_TOP from the hole's source y (258), hence 263.3/294.2.
        "doc_old_hole_y": 263.3,
        "doc_old_frame_y": 294.2,
    }


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--src", type=Path, default=DEFAULT_SRC)
    ap.add_argument("--out", type=Path, default=DEFAULT_OUT)
    ap.add_argument("--json", action="store_true", help="emit machine-readable report")
    args = ap.parse_args()

    report = build(args.src, args.out)
    if args.json:
        print(json.dumps(report, indent=2, ensure_ascii=False))
        return

    lay = report["layout"]
    print(f"skin raw      : {report['out']}")
    print(f"size          : {report['skin_size'][0]}x{report['skin_size'][1]} "
          f"({report['out_bytes']} bytes)")
    print(f"sha256        : {report['out_sha256']}")
    print(f"hole in crop  : {report['hole_in_crop']}")
    print(f"hole fill rect: {report['hole_fill_rect']}")
    print(f"green removed : {report['green_pixels_removed']} px")
    print(f"LCD-off       : {report['lcd_off_rgb565']} (samples {report['samples']})")
    print("--- static layout audit (step 3 inputs) ---")
    print(f"skin scale    : {lay['skin_scale']}  -> on screen {lay['skin_on_screen']}")
    print(f"hole on screen: {lay['hole_on_screen']} size {lay['hole_size_on_screen']}")
    print(f"frame scale   : {lay['frame_scale']}x size {lay['frame_size_on_screen']}")
    print(f"frame origin  : {lay['frame_origin_on_screen']} "
          f"hole margin {lay['hole_top_bottom_margin']}")
    print(f"NOTE: doc 11.1 hole/frame y ({report['doc_old_hole_y']}/"
          f"{report['doc_old_frame_y']}) omitted crop offset 86; "
          f"correct values are {lay['hole_on_screen'][1]}/"
          f"{lay['frame_origin_on_screen'][1]}.")


if __name__ == "__main__":
    main()
