#!/usr/bin/env python3
"""
╔══════════════════════════════════════════════════════════════════════╗
║              PS1 TIM Converter  —  ps1_tim_tool.py                   ║
║   Convert any image (PNG, JPG, BMP, TGA, WebP …) ← → TIM (.tim)      ║
║   Supported formats: 4-bit CLUT | 8-bit CLUT | 16-bit Direct | 24-bit Direct ║
║   Built directly from Sony's official TIM texture spec for PlayStation ║
╚══════════════════════════════════════════════════════════════════════╝

Important technical note before use:
──────────────────────────────────
The PS1's TIM texture format is structurally different from TIM2 (PS2):
  • There is no "32-bit RGBA" because the GPU has no full 8-bit alpha
    channel — only a single bit per pixel called STP (Semi-Transparency).
  • The highest direct format available is 24-bit (no transparency at all).
  • There are no Mipmaps and no GS Swizzle, because the PS1 GPU reads
    memory linearly — so this tool does not include those two options
    because they simply don't exist on this hardware (not an oversight).
  • Power-of-2 dimensions are not mandatory on PS1 the way they are on
    PS2's GS, but they remained a common convention in older studios,
    so the tool only warns about it and never enforces it.
  • There is a special hardware rule: the value 0x0000 (black + STP=0)
    means a fully transparent pixel regardless of color intent. The
    tool handles this case automatically (see the color_to_5551
    function).

Usage:
  {PROG} <image(s)> --format <fmt> [options]
  {PROG} <file.tim> --info
  {PROG} <file.tim> --verify
  {PROG} <file.tim> --extract png
  {PROG} --list convert.txt
  {PROG} --diff --original a.tim --modified b.tim

Examples:
  {PROG} hero.png    --format 16bit
  {PROG} bg.jpg       --format 24bit
  {PROG} sprite.bmp   --format 8bit  --dither
  {PROG} icon.tga     --format 4bit
  {PROG} *.png *.jpg  --format 16bit
  {PROG} tex.png      --format 16bit --no-premult
  {PROG} img.png      --format 8bit  --output out.tim
  {PROG} font1.tim    --info
  {PROG} --list textures.txt
  {PROG} font1.tim    --extract png
  {PROG}              --list-formats
"""

import argparse
import struct
import sys
import warnings
from pathlib import Path
from PIL import Image

warnings.filterwarnings("ignore", category=DeprecationWarning)


def _prog_name() -> str:
    """Determines the program name based on how it was launched (Python script or executable)."""
    import os
    argv0 = sys.argv[0]
    base = os.path.basename(argv0)
    if base.endswith('.py'):
        py = os.path.basename(sys.executable)
        return f"{py} {base}"
    return base


PROG = _prog_name()


# ══════════════════════════════════════════════════════════════════════════
#  Accepted input image formats (for conversion to TIM)
# ══════════════════════════════════════════════════════════════════════════
SUPPORTED_EXTENSIONS = {
    '.png', '.jpg', '.jpeg', '.bmp', '.tga', '.tiff', '.tif',
    '.webp', '.gif', '.ppm', '.pgm', '.pbm', '.ico', '.dds',
}

# Supported output formats for --extract (TIM → image)
EXTRACT_EXTENSIONS = {'.png', '.bmp', '.tga', '.tiff', '.tif', '.webp', '.ppm'}


# ══════════════════════════════════════════════════════════════════════════
#  TIM constants — taken directly from Sony's original PlayStation spec
# ══════════════════════════════════════════════════════════════════════════

TIM_ID = 0x00000010          # The mandatory fixed value in the first 4 bytes

# ─── Pixel Mode (bits 0-1 of the FLAG field) ──────────────────────────────
PMODE_4BIT  = 0x0            # 4-bit CLUT   — up to 16 colors
PMODE_8BIT  = 0x1            # 8-bit CLUT   — up to 256 colors
PMODE_16BIT = 0x2            # 16-bit direct — RGBA5551 (single transparency bit)
PMODE_24BIT = 0x3            # 24-bit direct — RGB888   (no transparency at all)

CF_CLUT_PRESENT = 0x8        # Bit 3 of FLAG: a color lookup table (CLUT) is present

_PMODE_NAME = {
    PMODE_4BIT:  '4-bit  Indexed (16 colors, CLUT)',
    PMODE_8BIT:  '8-bit  Indexed (256 colors, CLUT)',
    PMODE_16BIT: '16-bit Direct (RGBA5551, 1-bit STP)',
    PMODE_24BIT: '24-bit Direct (RGB888, no transparency)',
}

# CLI format name → official pmode number
FORMATS = {
    '4bit':  PMODE_4BIT,
    '8bit':  PMODE_8BIT,
    '16bit': PMODE_16BIT,
    '24bit': PMODE_24BIT,
}

# Alignment requirement: image width (in pixels) must be a multiple of
# this number, because the "Width" field in the TIM header is stored in
# halfword units (halfword = 2 bytes), not in pixels directly.
_WIDTH_MULTIPLE = {
    PMODE_4BIT:  4,   # 4 pixels = 1 byte = half a halfword... i.e. 4px = 1 halfword
    PMODE_8BIT:  2,   # 2 pixels = 2 bytes = 1 halfword
    PMODE_16BIT: 1,   # 1 pixel = 1 halfword
    PMODE_24BIT: 2,   # double the width so the byte count is even (3x2=6 bytes=3 halfwords)
}


# ══════════════════════════════════════════════════════════════════════════
#  Core helper functions
# ══════════════════════════════════════════════════════════════════════════

def align_up(n: int, m: int) -> int:
    """Rounds n up to the nearest multiple of m."""
    return ((n + m - 1) // m) * m


def is_power_of_2(n: int) -> bool:
    return n > 0 and (n & (n - 1)) == 0


def prev_power_of_2(n: int) -> int:
    if n <= 0:
        return 1
    p = 1
    while p * 2 <= n:
        p *= 2
    return p


def next_power_of_2(n: int) -> int:
    if n <= 0:
        return 1
    p = 1
    while p < n:
        p *= 2
    return p


def warn_dimensions(name: str, w: int, h: int) -> None:
    """
    Optional-only warning (not mandatory as on PS2) about power-of-2 sizes.
    ┌──────────────────────────────────────────────────────────────────┐
    │  The PS1 GPU does not enforce power-of-2, but most old studios   │
    │  adopted it as an industry convention to avoid wasting the       │
    │  limited VRAM (1MB).                                             │
    └──────────────────────────────────────────────────────────────────┘
    """
    if is_power_of_2(w) and is_power_of_2(h):
        return
    print(f"  NOTE: '{name}' ({w}x{h}) is not a power of 2. "
          f"Not mandatory on PS1, but recommended for compatibility with "
          f"legacy VRAM tools "
          f"(suggestion: {prev_power_of_2(w)}x{prev_power_of_2(h)} or "
          f"{next_power_of_2(w)}x{next_power_of_2(h)}).")


def open_any_image(path: Path) -> Image.Image:
    ext = path.suffix.lower()
    if ext not in SUPPORTED_EXTENSIONS:
        supported = ', '.join(sorted(SUPPORTED_EXTENSIONS))
        raise ValueError(f"Unsupported file format '{ext}'.\n  Supported: {supported}")
    return Image.open(path)


def pad_canvas(img: Image.Image, mult_w: int) -> Image.Image:
    """
    Expands the image (with transparency) until its width is a multiple
    of mult_w. Required because the Width field in TIM is computed in
    halfword units (see _WIDTH_MULTIPLE).
    """
    w, h = img.size
    new_w = align_up(w, mult_w)
    if new_w == w:
        return img
    canvas = Image.new('RGBA', (new_w, h), (0, 0, 0, 0))
    canvas.paste(img, (0, 0))
    return canvas


def apply_resize(img: Image.Image, mode: str) -> Image.Image:
    """
    --resize up   : upscale each dimension to the nearest higher power of 2.
    --resize down : downscale each dimension to the nearest lower power of 2.
    --resize WxH  : custom dimensions (example: 128x128).
    """
    w, h = img.size
    if mode == 'up':
        nw, nh = next_power_of_2(w), next_power_of_2(h)
    elif mode == 'down':
        nw, nh = max(1, prev_power_of_2(w)), max(1, prev_power_of_2(h))
    elif 'x' in mode.lower():
        try:
            sw, sh = mode.lower().split('x')
            nw, nh = int(sw), int(sh)
        except Exception:
            raise ValueError(f"Invalid --resize value: '{mode}' (use up | down | WxH)")
    else:
        raise ValueError(f"Invalid --resize value: '{mode}' (use up | down | WxH)")
    if (nw, nh) == (w, h):
        return img
    return img.resize((nw, nh), Image.LANCZOS)


def premultiply_alpha(img: Image.Image) -> Image.Image:
    """
    Multiplies color channels by the alpha value to avoid fringing at
    partial-transparency edges after the hard reduction to a single
    transparency bit (STP), or during quantize.
    """
    img = img.convert('RGBA')
    r, g, b, a = img.split()

    def _mul(channel):
        return Image.eval(Image.merge('LA', (channel, a)).convert('LA'), lambda v: v)

    # Direct per-pixel approach via point(), at reasonable speed
    px = img.load()
    out = Image.new('RGBA', img.size)
    opx = out.load()
    w, h = img.size
    for y in range(h):
        for x in range(w):
            pr, pg, pb, pa = px[x, y]
            f = pa / 255.0
            opx[x, y] = (round(pr * f), round(pg * f), round(pb * f), pa)
    return out


# ══════════════════════════════════════════════════════════════════════════
#  Color conversion to PS1 format (RGBA5551 — same layout in CLUT and 16-bit)
# ══════════════════════════════════════════════════════════════════════════

def pack_5551(r5: int, g5: int, b5: int, a: int) -> int:
    """
    Packs already-computed 5-bit components (0-31) into the final 16-bit
    PS1 value, applying the hardware's full-transparency rule (see the
    color_to_5551 documentation below for details of this rule).
    """
    if a <= 0:
        return 0x0000
    stp = 1 if a >= 128 else 0
    val = (stp << 15) | ((b5 & 0x1F) << 10) | ((g5 & 0x1F) << 5) | (r5 & 0x1F)
    if val == 0x0000:
        val = 0x0001
    return val


def color_to_5551(r: int, g: int, b: int, a: int) -> int:
    """
    Converts RGBA8888 → PS1 GPU format (Little Endian, 16-bit):
    ┌───────────────────────────────────────────────────────────┐
    │  Bit 15      : STP  (Semi-Transparency enable flag)       │
    │  Bits 10-14  : B (5 bits)                                 │
    │  Bits 5-9    : G (5 bits)                                 │
    │  Bits 0-4    : R (5 bits)                                 │
    └───────────────────────────────────────────────────────────┘
    Rounding accuracy: we use round(v * 31 / 255) instead of the raw
    shift (v >> 3). The difference is decisive: (v >> 3) drops the
    lowest 3 bits with no rounding at all, so it always truncates every
    value downward (a systematic quantization bias of up to 7/255 per
    channel), while round() distributes the error up or down based on
    the actual nearest value, producing a color that is mathematically
    closer to the source with roughly half the average error. This is
    the same approach any professional export tool takes when color
    accuracy matters.

    Critical hardware rule actually enforced by the GPU:
      The exact value 0x0000 (all bits zero) = a fully transparent
      pixel, regardless of color intent. Therefore:
      • If the requested transparency is 0 → we return 0x0000 directly.
      • If the computed result is 0x0000 but the color is meant to be
        opaque (pure opaque black) → we shift the red channel by one
        bit (0x0001) to avoid it being misread as transparent. This is
        the same workaround used by professional TIM tools.
    """
    if a <= 0:
        return 0x0000
    r5 = min(31, max(0, round(r * 31 / 255)))
    g5 = min(31, max(0, round(g * 31 / 255)))
    b5 = min(31, max(0, round(b * 31 / 255)))
    return pack_5551(r5, g5, b5, a)


def compute_tpage(vram_x: int, vram_y: int, pmode: int, abr: int = 0) -> int:
    """
    Computes the TPAGE value — the value every real PS1 draw command
    (POLY_FT4, POLY_GT4 ...) needs to know where to read the texture
    from within VRAM.
    ┌──────────────────────────────────────────────────────────────────┐
    │  This value is NOT stored inside the TIM file itself (the        │
    │  official spec does not store it) — it is computed from the      │
    │  texture's VRAM position at load time. Sony's official tool      │
    │  (TIMTOOL) used to display it to the developer as a convenience, │
    │  and that's exactly what this function does: the same            │
    │  calculation used in the official SDK.                           │
    │                                                                  │
    │  TPAGE = (tx/64) | (ty/256)<<4 | (abr<<5) | (tp<<7)              │
    │    tx, ty : texture page position (rounded to the nearest        │
    │             64x256 page)                                         │
    │    abr    : semi-transparency rate (0-3, decided by the          │
    │             programmer at draw time)                             │
    │    tp     : 0=4bit  1=8bit  2=15bit (direct/24bit treated as 2)  │
    └──────────────────────────────────────────────────────────────────┘
    """
    tp = {PMODE_4BIT: 0, PMODE_8BIT: 1, PMODE_16BIT: 2, PMODE_24BIT: 2}[pmode]
    tx_page = (vram_x // 64) & 0xF
    ty_page = (vram_y // 256) & 0x1
    return tx_page | (ty_page << 4) | ((abr & 0x3) << 5) | ((tp & 0x3) << 7)


def compute_clut_id(clut_x: int, clut_y: int) -> int:
    """
    Computes the Clut ID — the value passed directly into the clut field
    of draw commands to indicate where the color table sits in VRAM.
    ┌──────────────────────────────────────────────────┐
    │  Clut ID = (clut_x / 16) | (clut_y << 6)         │
    └──────────────────────────────────────────────────┘
    """
    return ((clut_x // 16) & 0x3F) | ((clut_y & 0x1FF) << 6)


def warn_vram_bounds(x: int, y: int, w: int, h: int, label: str) -> None:
    """
    Checks that the requested position lies within the PS1's actual
    VRAM bounds (1024x512 pixels at 16-bit, the entire video memory
    available on the console).
    """
    if x < 0 or y < 0 or x + w > 1024 or y + h > 512:
        print(f"  WARNING: {label} position ({x},{y}) with size {w}x{h} "
              f"exceeds the PS1's actual VRAM bounds (1024x512).")


def color_from_5551(val: int):
    """Reverse conversion: PS1 16-bit value → (r, g, b, a) in the 0-255 range."""
    if val == 0x0000:
        return (0, 0, 0, 0)
    r5 = val & 0x1F
    g5 = (val >> 5) & 0x1F
    b5 = (val >> 10) & 0x1F
    r = (r5 << 3) | (r5 >> 2)
    g = (g5 << 3) | (g5 >> 2)
    b = (b5 << 3) | (b5 >> 2)
    return (r, g, b, 255)


# ══════════════════════════════════════════════════════════════════════════
#  Quantization — color reduction for the indexed formats (4-bit and 8-bit)
# ══════════════════════════════════════════════════════════════════════════

def quantize_best(img_rgba: Image.Image, num_colors: int, use_dither: bool = False):
    """Color reduction with MEDIANCUT and optional Floyd-Steinberg support."""
    dither_mode = Image.Dither.FLOYDSTEINBERG if use_dither else Image.Dither.NONE
    bg = Image.new('RGB', img_rgba.size, (0, 0, 0))
    bg.paste(img_rgba.convert('RGB'), mask=img_rgba.split()[3])
    return bg.quantize(colors=num_colors, method=1, dither=dither_mode)


def extract_alpha_per_index(img_rgba: Image.Image, img_q: Image.Image, num_colors: int) -> dict:
    """Average real alpha value for each color in the resulting palette."""
    alpha_data = list(img_rgba.split()[3].getdata())
    index_data = list(img_q.getdata())
    grouped: dict = {}
    for idx, a in zip(index_data, alpha_data):
        grouped.setdefault(idx, []).append(a)
    return {i: (round(sum(grouped[i]) / len(grouped[i])) if i in grouped else 255)
            for i in range(num_colors)}


# ══════════════════════════════════════════════════════════════════════════
#  Building the CLUT and Image Block — per the official TIM layout
# ══════════════════════════════════════════════════════════════════════════
#
#  TIM File Layout:
#  ┌──────────────────────────────────────────────────────────────┐
#  │  Offset 0   uint32   ID     = 0x00000010                    │
#  │  Offset 4   uint32   FLAG   (pmode | clut-present flag)     │
#  │  [if CLUT present]                                             │
#  │    uint32  CLUT section length (includes this 12-byte         │
#  │            sub-header)                                        │
#  │    int16   CLUT X  (VRAM position, usually 0)                 │
#  │    int16   CLUT Y                                             │
#  │    uint16  CLUT Width   (= number of colors)                  │
#  │    uint16  CLUT Height  (= number of palettes, usually 1)     │
#  │    ...CLUT colors (each color is a uint16, 5551 format)       │
#  │  then the image section:                                       │
#  │    uint32  Image section length (includes this 12-byte        │
#  │            sub-header)                                        │
#  │    int16   Image X                                            │
#  │    int16   Image Y                                            │
#  │    uint16  Image Width   (in halfword units! not pixels)      │
#  │    uint16  Image Height  (in pixels directly)                 │
#  │    ...pixel data                                              │
#  └──────────────────────────────────────────────────────────────┘

def build_clut_block(colors_5551: list, clut_x: int = 0, clut_y: int = 0,
                     num_palettes: int = 1) -> bytes:
    """
    num_palettes > 1 builds several CLUT rows in the same block (the
    Height field in the TIM header officially allows this) — used in
    real games to swap colors (recolor) on the same pixel data without
    duplicating it, e.g. different team colors for the same character.
    """
    colors_per_palette = len(colors_5551) // num_palettes
    header = struct.pack('<IhhHH', 12 + len(colors_5551) * 2,
                          clut_x, clut_y, colors_per_palette, num_palettes)
    body = b''.join(struct.pack('<H', c) for c in colors_5551)
    return header + body


def build_image_block(pixel_data: bytes, width_units: int, height: int,
                      img_x: int = 0, img_y: int = 0) -> bytes:
    header = struct.pack('<IhhHH', 12 + len(pixel_data), img_x, img_y, width_units, height)
    return header + pixel_data


def build_tim_file(pmode: int, clut_block: bytes, image_block: bytes) -> bytes:
    flag = pmode | (CF_CLUT_PRESENT if clut_block else 0)
    header = struct.pack('<II', TIM_ID, flag)
    return header + clut_block + image_block


# ══════════════════════════════════════════════════════════════════════════
#  The four format converters
# ══════════════════════════════════════════════════════════════════════════

def convert_4bit(img: Image.Image, premult=True, dither=False,
                 vram_x=0, vram_y=0, clut_x=0, clut_y=0, palette_count=1, **_) -> bytes:
    """
    4-bit CLUT — up to 16 colors per palette. Ideal for UI, fonts, icons.
    palette_count > 1: builds several color palettes (16 x N) sharing the
    same pixel data, to support runtime color swapping (an authentic
    recolor technique on PS1).
    """
    if premult:
        img = premultiply_alpha(img)
    img = img.convert('RGBA')
    img = pad_canvas(img, _WIDTH_MULTIPLE[PMODE_4BIT])
    w, h = img.size

    q = quantize_best(img, 16, use_dither=dither)
    alpha_map = extract_alpha_per_index(img, q, 16)
    palette_rgb = q.getpalette()[:16 * 3]
    base_colors = []
    for i in range(16):
        r, g, b = palette_rgb[i * 3:i * 3 + 3]
        base_colors.append(color_to_5551(r, g, b, alpha_map[i]))
    colors = base_colors * max(1, palette_count)

    indices = list(q.getdata())
    pixel_data = bytearray(w * h // 2)
    for i in range(0, w * h, 2):
        lo = indices[i] & 0xF
        hi = indices[i + 1] & 0xF
        pixel_data[i // 2] = lo | (hi << 4)

    warn_vram_bounds(clut_x, clut_y, 16 * max(1, palette_count), 1, "CLUT")
    warn_vram_bounds(vram_x, vram_y, w // 4, h, "Image")
    clut_block = build_clut_block(colors, clut_x, clut_y, max(1, palette_count))
    image_block = build_image_block(bytes(pixel_data), w // 4, h, vram_x, vram_y)
    print(f"  TPAGE = 0x{compute_tpage(vram_x, vram_y, PMODE_4BIT):03X}   "
          f"CLUT ID = 0x{compute_clut_id(clut_x, clut_y):04X}")
    return build_tim_file(PMODE_4BIT, clut_block, image_block)


def convert_8bit(img: Image.Image, premult=True, dither=False,
                 vram_x=0, vram_y=0, clut_x=0, clut_y=0, palette_count=1, **_) -> bytes:
    """8-bit CLUT — up to 256 colors per palette. Best suited for most game textures."""
    if premult:
        img = premultiply_alpha(img)
    img = img.convert('RGBA')
    img = pad_canvas(img, _WIDTH_MULTIPLE[PMODE_8BIT])
    w, h = img.size

    q = quantize_best(img, 256, use_dither=dither)
    alpha_map = extract_alpha_per_index(img, q, 256)
    palette_rgb = q.getpalette()[:256 * 3]
    base_colors = []
    for i in range(256):
        r, g, b = palette_rgb[i * 3:i * 3 + 3]
        base_colors.append(color_to_5551(r, g, b, alpha_map[i]))
    colors = base_colors * max(1, palette_count)

    pixel_data = bytes(q.getdata())

    warn_vram_bounds(clut_x, clut_y, 256, max(1, palette_count), "CLUT")
    warn_vram_bounds(vram_x, vram_y, w // 2, h, "Image")
    clut_block = build_clut_block(colors, clut_x, clut_y, max(1, palette_count))
    image_block = build_image_block(pixel_data, w // 2, h, vram_x, vram_y)
    print(f"  TPAGE = 0x{compute_tpage(vram_x, vram_y, PMODE_8BIT):03X}   "
          f"CLUT ID = 0x{compute_clut_id(clut_x, clut_y):04X}")
    return build_tim_file(PMODE_8BIT, clut_block, image_block)


def dither_direct_5bit(img: Image.Image) -> Image.Image:
    """
    Floyd-Steinberg error diffusion (serpentine pass) applied directly to
    the R/G/B channels before packing them into 5-bit form, dedicated to
    the direct 16-bit format (it has no CLUT for Pillow to rely on, so
    packing the channel directly with no error diffusion used to cause
    visible banding in smooth gradients).
    Every pixel here is rebuilt already-quantized, so that the final
    value later passed into color_to_5551 matches exactly what the
    diffusion chose here (round() on a multiple of 255/31 returns the
    same index). Transparency (alpha) passes through unmodified — it has
    no relation to color quality.
    """
    img = img.convert('RGBA')
    w, h = img.size
    src = img.load()

    # Floating-point error buffer per channel (current row + next row),
    # sized w+2 to avoid going out of bounds when diffusing left/right
    # in the serpentine pass.
    err_r = [[0.0] * (w + 2) for _ in range(2)]
    err_g = [[0.0] * (w + 2) for _ in range(2)]
    err_b = [[0.0] * (w + 2) for _ in range(2)]

    out = Image.new('RGBA', (w, h))
    dst = out.load()
    step = 255.0 / 31.0

    for y in range(h):
        cur_r, nxt_r = err_r[y % 2], err_r[(y + 1) % 2]
        cur_g, nxt_g = err_g[y % 2], err_g[(y + 1) % 2]
        cur_b, nxt_b = err_b[y % 2], err_b[(y + 1) % 2]
        for i in range(w + 2):
            nxt_r[i] = 0.0
            nxt_g[i] = 0.0
            nxt_b[i] = 0.0

        left_to_right = (y % 2 == 0)
        xs = range(w) if left_to_right else range(w - 1, -1, -1)
        for x in xs:
            r, g, b, a = src[x, y]
            r_adj = r + cur_r[x + 1]
            g_adj = g + cur_g[x + 1]
            b_adj = b + cur_b[x + 1]

            r5 = min(31, max(0, round(r_adj / step)))
            g5 = min(31, max(0, round(g_adj / step)))
            b5 = min(31, max(0, round(b_adj / step)))

            dst[x, y] = (r5, g5, b5, a)  # temporarily store the 5-bit index in the channels

            er = r_adj - r5 * step
            eg = g_adj - g5 * step
            eb = b_adj - b5 * step

            d = 1 if left_to_right else -1
            fwd, back = x + 1 + d, x + 1 - d
            cur_r[fwd] += er * 7 / 16; nxt_r[back] += er * 3 / 16
            nxt_r[x + 1] += er * 5 / 16; nxt_r[fwd] += er * 1 / 16
            cur_g[fwd] += eg * 7 / 16; nxt_g[back] += eg * 3 / 16
            nxt_g[x + 1] += eg * 5 / 16; nxt_g[fwd] += eg * 1 / 16
            cur_b[fwd] += eb * 7 / 16; nxt_b[back] += eb * 3 / 16
            nxt_b[x + 1] += eb * 5 / 16; nxt_b[fwd] += eb * 1 / 16

    return out


def convert_16bit(img: Image.Image, premult=True, dither=False, vram_x=0, vram_y=0, **_) -> bytes:
    """16-bit Direct RGBA5551 — no CLUT, single-bit transparency (opaque/transparent)."""
    if premult:
        img = premultiply_alpha(img)
    img = img.convert('RGBA')
    w, h = img.size

    pixel_data = bytearray()
    if dither:
        # The image produced by dither_direct_5bit already carries ready
        # 5-bit indices (0-31) inside its R/G/B channels, so we pack them
        # directly via pack_5551 without re-quantizing (to avoid applying
        # round() twice to the same value).
        dimg = dither_direct_5bit(img)
        draw = dimg.tobytes()
        for i in range(0, len(draw), 4):
            val = pack_5551(draw[i], draw[i + 1], draw[i + 2], draw[i + 3])
            pixel_data += struct.pack('<H', val)
    else:
        raw = img.tobytes()
        for i in range(0, len(raw), 4):
            val = color_to_5551(raw[i], raw[i + 1], raw[i + 2], raw[i + 3])
            pixel_data += struct.pack('<H', val)

    warn_vram_bounds(vram_x, vram_y, w, h, "Image")
    image_block = build_image_block(bytes(pixel_data), w, h, vram_x, vram_y)
    print(f"  TPAGE = 0x{compute_tpage(vram_x, vram_y, PMODE_16BIT):03X}   (no CLUT for this format)")
    return build_tim_file(PMODE_16BIT, b'', image_block)


def convert_24bit(img: Image.Image, vram_x=0, vram_y=0, **_) -> bytes:
    """
    24-bit Direct RGB888 — no transparency at all (no CLUT and no active
    STP). Typically used for splash screens and high-quality static
    backgrounds.
    """
    img = img.convert('RGBA')
    img = pad_canvas(img, _WIDTH_MULTIPLE[PMODE_24BIT])
    w, h = img.size
    raw = img.tobytes()

    pixel_data = bytearray(w * h * 3)
    for i in range(w * h):
        pixel_data[i * 3 + 0] = raw[i * 4 + 0]
        pixel_data[i * 3 + 1] = raw[i * 4 + 1]
        pixel_data[i * 3 + 2] = raw[i * 4 + 2]

    width_units = (w * 3) // 2
    warn_vram_bounds(vram_x, vram_y, width_units, h, "Image")
    image_block = build_image_block(bytes(pixel_data), width_units, h, vram_x, vram_y)
    print(f"  TPAGE = 0x{compute_tpage(vram_x, vram_y, PMODE_24BIT):03X}   (no CLUT for this format)")
    return build_tim_file(PMODE_24BIT, b'', image_block)


CONVERTERS = {
    '4bit':  convert_4bit,
    '8bit':  convert_8bit,
    '16bit': convert_16bit,
    '24bit': convert_24bit,
}


# ══════════════════════════════════════════════════════════════════════════
#  --info — reading and displaying info about an existing TIM file
# ══════════════════════════════════════════════════════════════════════════

def parse_tim(data: bytes) -> dict:
    if len(data) < 8:
        raise ValueError("File is too small to be a valid TIM")
    tim_id, flag = struct.unpack_from('<II', data, 0)
    if tim_id != TIM_ID:
        raise ValueError(f"Not a valid TIM file (ID = 0x{tim_id:08X}, expected 0x{TIM_ID:08X})")

    pmode = flag & 0x3
    has_clut = bool(flag & CF_CLUT_PRESENT)
    if pmode not in _PMODE_NAME:
        raise ValueError(f"Unknown pixel mode: {pmode}")

    off = 8
    clut = None
    if has_clut:
        clut_len, cx, cy, cw, ch = struct.unpack_from('<IhhHH', data, off)
        clut_colors_off = off + 12
        n_colors = cw * ch
        colors = list(struct.unpack_from(f'<{n_colors}H', data, clut_colors_off))
        clut = dict(length=clut_len, x=cx, y=cy, width=cw, height=ch, colors=colors)
        off += clut_len

    img_len, ix, iy, iw_units, ih = struct.unpack_from('<IhhHH', data, off)
    img_data_off = off + 12
    img_data = data[img_data_off: off + img_len]

    return dict(pmode=pmode, has_clut=has_clut, clut=clut,
                image=dict(length=img_len, x=ix, y=iy, width_units=iw_units,
                           height=ih, data=img_data, data_offset=img_data_off))


def pixel_width_of(pmode: int, width_units: int) -> int:
    if pmode == PMODE_4BIT:
        return width_units * 4
    if pmode == PMODE_8BIT:
        return width_units * 2
    if pmode == PMODE_16BIT:
        return width_units
    if pmode == PMODE_24BIT:
        return (width_units * 2) // 3
    raise ValueError(f"Unknown pmode: {pmode}")


def tim_info(path: Path) -> None:
    if not path.exists():
        raise FileNotFoundError(f"File not found: {path}")
    data = path.read_bytes()
    info = parse_tim(data)
    pmode = info['pmode']
    img = info['image']
    pw = pixel_width_of(pmode, img['width_units'])
    ph = img['height']

    sep = '─' * 52
    print(f"""
{sep}
  File        :  {path.name}
  File size   :  {len(data) / 1024:.2f} KB  ({len(data)} bytes)
{sep}
  Pixel mode  :  {_PMODE_NAME[pmode]}
  Dimensions  :  {pw} x {ph} px
{sep}""")
    warn_dimensions(path.name, pw, ph)
    if info['clut']:
        c = info['clut']
        print(f"""  CLUT present:  yes
  CLUT colors :  {c['width']} per palette   x  {c['height']} palette(s)  =  {c['width']*c['height']} total
  CLUT size   :  {len(c['colors']) * 2} bytes
  CLUT X/Y    :  ({c['x']}, {c['y']})
  CLUT ID     :  0x{compute_clut_id(c['x'], c['y']):04X}   [passed directly to the draw command]
{sep}""")
    else:
        print(f"  CLUT present:  no\n{sep}")
    print(f"""  Image data  :  {len(img['data'])} bytes  (declared {img['length'] - 12})
  Image X/Y   :  ({img['x']}, {img['y']})   [VRAM position]
  TPAGE       :  0x{compute_tpage(img['x'], img['y'], pmode):03X}   [passed directly to the draw command]
{sep}""")


# ══════════════════════════════════════════════════════════════════════════
#  --verify — checking the structural validity of the file
# ══════════════════════════════════════════════════════════════════════════

def verify_tim(path: Path) -> bool:
    print(f"Verifying: {path.name} ...", end=' ', flush=True)
    try:
        data = path.read_bytes()
        info = parse_tim(data)
        pmode = info['pmode']
        img = info['image']

        problems = []
        if img['length'] != 12 + len(img['data']):
            problems.append("Declared image section length does not match the actual data")
        if info['clut']:
            c = info['clut']
            if c['length'] != 12 + len(c['colors']) * 2:
                problems.append("Declared CLUT section length does not match the actual data")
            expected_colors = 16 if pmode == PMODE_4BIT else 256 if pmode == PMODE_8BIT else None
            if expected_colors and c['width'] != expected_colors:
                problems.append(f"Colors per palette ({c['width']}) unexpected for this format "
                                 f"(expected {expected_colors}) — note: width=colors/palette, "
                                 f"height=number of palettes")
        else:
            if pmode in (PMODE_4BIT, PMODE_8BIT):
                problems.append("Format is indexed but the file has no CLUT")

        pw = pixel_width_of(pmode, img['width_units'])
        mult = _WIDTH_MULTIPLE[pmode]
        if pw % mult != 0:
            problems.append(f"Width is not compatible with the required halfword alignment ({mult})")

        expected_data_len = {
            PMODE_4BIT:  pw * img['height'] // 2,
            PMODE_8BIT:  pw * img['height'],
            PMODE_16BIT: pw * img['height'] * 2,
            PMODE_24BIT: pw * img['height'] * 3,
        }[pmode]
        if len(img['data']) != expected_data_len:
            problems.append(f"Image data size ({len(img['data'])}) does not match the expected "
                             f"({expected_data_len}) based on the dimensions and format")

        if problems:
            print("FAILED")
            for p in problems:
                print(f"    - {p}")
            return False
        print("OK")
        return True
    except Exception as e:
        print(f"FAILED  ({e})")
        return False


# ══════════════════════════════════════════════════════════════════════════
#  --extract — TIM → regular image
# ══════════════════════════════════════════════════════════════════════════

def extract_tim(path: Path, ext: str, palette_index: int = 0) -> Path:
    data = path.read_bytes()
    info = parse_tim(data)
    pmode = info['pmode']
    img = info['image']
    pw = pixel_width_of(pmode, img['width_units'])
    ph = img['height']
    raw = img['data']

    out = Image.new('RGBA', (pw, ph))
    px = out.load()

    if pmode in (PMODE_4BIT, PMODE_8BIT):
        c = info['clut']
        n_palettes = c['height']
        colors_per_pal = c['width']
        if palette_index >= n_palettes:
            print(f"  NOTE: this file only contains {n_palettes} palette(s), "
                  f"palette 0 will be used instead of {palette_index}")
            palette_index = 0
        start = palette_index * colors_per_pal
        clut_colors = c['colors'][start:start + colors_per_pal]
        palette = [color_from_5551(v) for v in clut_colors]
        if pmode == PMODE_4BIT:
            for i in range(pw * ph):
                byte = raw[i // 2]
                idx = (byte & 0xF) if (i % 2 == 0) else ((byte >> 4) & 0xF)
                px[i % pw, i // pw] = palette[idx]
        else:
            for i in range(pw * ph):
                idx = raw[i]
                px[i % pw, i // pw] = palette[idx]
    elif pmode == PMODE_16BIT:
        for i in range(pw * ph):
            val = struct.unpack_from('<H', raw, i * 2)[0]
            px[i % pw, i // pw] = color_from_5551(val)
    elif pmode == PMODE_24BIT:
        for i in range(pw * ph):
            r, g, b = raw[i * 3:i * 3 + 3]
            px[i % pw, i // pw] = (r, g, b, 255)

    dst = path.with_suffix(ext)
    if ext.lower() in ('.jpg', '.jpeg'):
        out.convert('RGB').save(dst)
    else:
        out.save(dst)
    return dst


# ══════════════════════════════════════════════════════════════════════════
#  --diff — structural comparison between two TIM files
# ══════════════════════════════════════════════════════════════════════════

def diff_files(orig_path: Path, mod_path: Path, show_header: bool = True) -> dict:
    sep = '─' * 56
    if show_header:
        print(f"\n{sep}\n  Comparing:\n    original: {orig_path.name}\n    modified: {mod_path.name}\n{sep}")

    try:
        d1, d2 = orig_path.read_bytes(), mod_path.read_bytes()
        i1, i2 = parse_tim(d1), parse_tim(d2)
    except Exception as e:
        print(f"  UNSAFE — could not read one of the files: {e}")
        return dict(compatible='UNSAFE')

    notes = []
    level = 'SAFE'

    if i1['pmode'] != i2['pmode']:
        notes.append(f"Format changed: {_PMODE_NAME[i1['pmode']]}  →  {_PMODE_NAME[i2['pmode']]}")
        level = 'UNSAFE'

    pw1 = pixel_width_of(i1['pmode'], i1['image']['width_units'])
    pw2 = pixel_width_of(i2['pmode'], i2['image']['width_units'])
    ph1, ph2 = i1['image']['height'], i2['image']['height']
    if (pw1, ph1) != (pw2, ph2):
        notes.append(f"Dimensions changed: {pw1}x{ph1}  →  {pw2}x{ph2}")
        if level != 'UNSAFE':
            level = 'WARNING'

    if bool(i1['clut']) != bool(i2['clut']):
        notes.append("CLUT presence changed between the two files")
        level = 'UNSAFE'

    if len(d1) != len(d2):
        notes.append(f"File size differs: {len(d1)} bytes → {len(d2)} bytes")

    d_min = min(len(i1['image']['data']), len(i2['image']['data']))
    if d_min > 0:
        diffs = sum(1 for a, b in zip(i1['image']['data'][:d_min], i2['image']['data'][:d_min]) if a != b)
        ratio = diffs / d_min * 100
        notes.append(f"Pixel data difference: {ratio:.2f}%")
        if level == 'SAFE' and ratio > 30:
            level = 'WARNING'

    for n in notes:
        print(f"  - {n}")
    print(f"  => {level}")
    return dict(compatible=level)


def diff_folders(orig_dir: Path, mod_dir: Path) -> None:
    orig_files = {p.name: p for p in orig_dir.glob('*.tim')}
    mod_files = {p.name: p for p in mod_dir.glob('*.tim')}
    common = sorted(set(orig_files) & set(mod_files))
    if not common:
        print("No common .tim files between the two folders")
        return
    results = []
    for name in common:
        results.append(diff_files(orig_files[name], mod_files[name]))
    safe = sum(1 for r in results if r['compatible'] == 'SAFE')
    warn = sum(1 for r in results if r['compatible'] == 'WARNING')
    unsafe = sum(1 for r in results if r['compatible'] == 'UNSAFE')
    print(f"\nSummary: {len(results)} compared | SAFE {safe} | WARNING {warn} | UNSAFE {unsafe}")


def read_diff_list(list_path: Path) -> list:
    pairs = []
    with open(list_path, 'r', encoding='utf-8') as f:
        for lineno, raw in enumerate(f, 1):
            line = raw.strip()
            if not line or line.startswith('#'):
                continue
            parts = line.split()
            if len(parts) < 2:
                raise ValueError(f"Line {lineno}: expected '<original> <modified>'")
            pairs.append((Path(parts[0]), Path(parts[1])))
    return pairs


# ══════════════════════════════════════════════════════════════════════════
#  --list — batch conversion from a text file
# ══════════════════════════════════════════════════════════════════════════

def read_list_file(list_path: Path) -> list:
    if not list_path.exists():
        raise FileNotFoundError(f"List file not found: {list_path}")
    valid = set(FORMATS.keys())
    entries = []
    with open(list_path, 'r', encoding='utf-8') as f:
        for lineno, raw in enumerate(f, 1):
            line = raw.strip()
            if not line or line.startswith('#'):
                continue
            parts = line.split()
            if len(parts) < 2:
                raise ValueError(f"Line {lineno}: missing format. Expected: <filename> <format>")
            filename, fmt = parts[0], parts[1].lower()
            if fmt not in valid:
                raise ValueError(f"Line {lineno}: unknown format '{fmt}'. Valid: {', '.join(valid)}")
            entries.append((lineno, filename, fmt))
    return entries


# ══════════════════════════════════════════════════════════════════════════
#  Actual conversion of a single file (used by every mode)
# ══════════════════════════════════════════════════════════════════════════

def convert_one(src, fmt, out_arg, premult, dither, resize, output_dir=None,
                vram_x=0, vram_y=0, clut_x=0, clut_y=0, palette_count=1) -> str:
    src_path = Path(src)
    img = open_any_image(src_path).convert('RGBA')

    if resize:
        img = apply_resize(img, resize)

    print(f"  Converting: {src_path.name}  ->  [{fmt}]", end=' ... ', flush=True)
    print()

    converter = CONVERTERS[fmt]
    tim_bytes = converter(img, premult=premult, dither=dither,
                          vram_x=vram_x, vram_y=vram_y,
                          clut_x=clut_x, clut_y=clut_y, palette_count=palette_count)

    if out_arg:
        dst_path = Path(out_arg)
    elif output_dir:
        dst_path = output_dir / (src_path.stem + '.tim')
    else:
        dst_path = src_path.with_suffix('.tim')

    dst_path.write_bytes(tim_bytes)
    size_kb = len(tim_bytes) / 1024
    print(f"done  ({size_kb:.1f} KB)  ->  {dst_path.name}")
    warn_dimensions(src_path.name, *img.size)
    return str(dst_path)


# ══════════════════════════════════════════════════════════════════════════
#  --list-formats
# ══════════════════════════════════════════════════════════════════════════

def list_formats() -> None:
    sep = '═' * 60
    print(f"""
{sep}
  PS1 TIM Converter — Supported formats
{sep}
  4bit    4-bit CLUT   up to 16 colors   — UI/fonts/icons
  8bit    8-bit CLUT   up to 256 colors  — most game textures
  16bit   16-bit Direct RGBA5551         — opaque/transparent only
  24bit   24-bit Direct RGB888           — no transparency (splash/backgrounds)
{sep}
  Commands:
    --format <fmt>      choose the conversion format
    --info               show info about an existing .tim file
    --verify             check the structural integrity of a .tim file
    --extract <ext>      extract an image from a .tim file
    --diff               compare two .tim files or folders
    --list <file.txt>    batch convert from a text file
    --output <file>      output file name (single file only)
    --output-dir <dir>   output directory for batches
    --resize up|down|WxH change dimensions before conversion
    --dither             Floyd-Steinberg to reduce color banding (4bit/8bit/16bit)
    --no-premult         disable premultiplied alpha
    --vram-x/--vram-y    image position within VRAM (auto-determines TPAGE)
    --clut-x/--clut-y    color table position within VRAM (auto-determines CLUT ID)
    --palette-count N    include several color palettes for recolor support (4bit/8bit)
    --palette-index N    with --extract: which palette to use from a multi-palette file
{sep}""")


# ══════════════════════════════════════════════════════════════════════════
#  main()
# ══════════════════════════════════════════════════════════════════════════

def build_arg_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog=PROG,
        description="PS1 TIM Converter — convert images ← → Sony's official TIM texture format",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    p.add_argument('images', nargs='*', help='image file(s) or .tim file(s)')
    p.add_argument('--format', choices=list(FORMATS.keys()), help='conversion format')
    p.add_argument('--output', help='output file name (single file only)')
    p.add_argument('--output-dir', help='output directory for batches')
    p.add_argument('--resize', help='up | down | WxH')
    p.add_argument('--dither', action='store_true',
                    help='Floyd-Steinberg dithering (4bit/8bit palette + 16bit channels)')
    p.add_argument('--no-premult', action='store_true', help='disable premultiplied alpha')

    p.add_argument('--vram-x', type=int, default=0, help='X position of the image within VRAM (0-1023)')
    p.add_argument('--vram-y', type=int, default=0, help='Y position of the image within VRAM (0-511)')
    p.add_argument('--clut-x', type=int, default=0, help='X position of the color table within VRAM')
    p.add_argument('--clut-y', type=int, default=0, help='Y position of the color table within VRAM')
    p.add_argument('--palette-count', type=int, default=1,
                    help='number of color palettes to include (4bit/8bit only) for recolor support')

    p.add_argument('--info', action='store_true', help='show info about a TIM file')
    p.add_argument('--verify', action='store_true', help='check the integrity of a TIM file')
    p.add_argument('--extract', help='extract an image from TIM (output extension, e.g. png)')
    p.add_argument('--palette-index', type=int, default=0,
                    help='with --extract on a multi-palette file: which palette to use')

    p.add_argument('--diff', action='store_true', help='compare TIM files')
    p.add_argument('--original', nargs='*', help='original file/folder for --diff')
    p.add_argument('--modified', nargs='*', help='modified file/folder for --diff')
    p.add_argument('--diff-list', help='text file with original/modified pairs')

    p.add_argument('--list', help='text file for batch conversion')
    p.add_argument('--list-formats', action='store_true', help='show supported formats and commands')
    return p


def main():
    parser = build_arg_parser()
    args = parser.parse_args()

    # ─── --diff mode ─────────────────────────────────────────────────────
    if args.diff:
        pairs = []
        if args.diff_list:
            try:
                pairs += read_diff_list(Path(args.diff_list))
            except Exception as e:
                print(f'ERROR reading diff list: {e}')
                sys.exit(1)

        if args.original and args.modified:
            orig_list, mod_list = args.original, args.modified
            if (len(orig_list) == 1 and len(mod_list) == 1
                    and Path(orig_list[0]).is_dir() and Path(mod_list[0]).is_dir()):
                diff_folders(Path(orig_list[0]), Path(mod_list[0]))
                return
            if len(orig_list) != len(mod_list):
                print('ERROR: --original and --modified must contain the same number of files')
                sys.exit(1)
            pairs += list(zip([Path(p) for p in orig_list], [Path(p) for p in mod_list]))
        elif args.original or args.modified:
            print('ERROR: --diff requires both --original and --modified')
            sys.exit(1)

        if not pairs:
            print('ERROR: no files to compare. Use --original/--modified or --diff-list')
            sys.exit(1)

        results = [diff_files(o, m, show_header=True) for o, m in pairs]
        if len(results) > 1:
            safe = sum(1 for r in results if r['compatible'] == 'SAFE')
            warn = sum(1 for r in results if r['compatible'] == 'WARNING')
            unsafe = sum(1 for r in results if r['compatible'] == 'UNSAFE')
            print(f"\nSummary: {len(results)} compared | SAFE {safe} | WARNING {warn} | UNSAFE {unsafe}")
        return

    # ─── --extract mode ──────────────────────────────────────────────────
    if args.extract:
        ext = args.extract.lower()
        if not ext.startswith('.'):
            ext = '.' + ext
        if ext not in EXTRACT_EXTENSIONS:
            supported = ', '.join(sorted(e.lstrip('.') for e in EXTRACT_EXTENSIONS))
            print(f'ERROR: unsupported extract format "{args.extract}". Supported: {supported}')
            sys.exit(1)
        if not args.images:
            print('ERROR: provide one or more .tim files with --extract')
            sys.exit(1)
        ok, fail, outputs = 0, 0, []
        print(f'Extracting {len(args.images)} file(s) -> [{ext}]\n')
        for src in args.images:
            src_path = Path(src)
            print(f'  Extracting: {src_path.name}', end=' ... ', flush=True)
            try:
                out = extract_tim(src_path, ext, palette_index=args.palette_index)
                outputs.append(str(out))
                ok += 1
                print(f'done  ({Path(out).stat().st_size / 1024:.1f} KB)')
            except Exception as e:
                print(f'FAILED  ({e})')
                fail += 1
        print(f'\nDone: {ok} succeeded, {fail} failed')
        for o in outputs:
            print(f'  {o}')
        return

    # ─── --verify mode ───────────────────────────────────────────────────
    if args.verify:
        if not args.images:
            print('ERROR: provide one or more .tim files with --verify')
            sys.exit(1)
        all_ok = all([verify_tim(Path(src)) for src in args.images])
        print('All files verified successfully.' if all_ok else 'One or more files failed verification.')
        if not all_ok:
            sys.exit(1)
        return

    # ─── --info mode ─────────────────────────────────────────────────────
    if args.info:
        if not args.images:
            print('ERROR: provide a .tim file with --info')
            sys.exit(1)
        for src in args.images:
            try:
                tim_info(Path(src))
            except Exception as e:
                print(f'  FAILED: {src}  ({e})')
        return

    # ─── --list mode ─────────────────────────────────────────────────────
    if args.list:
        try:
            entries = read_list_file(Path(args.list))
        except Exception as e:
            print(f'ERROR reading list file: {e}')
            sys.exit(1)
        if not entries:
            print('WARNING: list file is empty')
            return
        premult = not args.no_premult
        output_dir = Path(args.output_dir) if args.output_dir else None
        if output_dir:
            output_dir.mkdir(parents=True, exist_ok=True)
        print(f'Reading list: {Path(args.list).name}  ({len(entries)} entries)\n')
        ok, fail, outputs = 0, 0, []
        for lineno, filename, fmt in entries:
            try:
                out = convert_one(filename, fmt, None, premult, args.dither, args.resize, output_dir,
                                  vram_x=args.vram_x, vram_y=args.vram_y,
                                  clut_x=args.clut_x, clut_y=args.clut_y,
                                  palette_count=args.palette_count)
                outputs.append(out)
                ok += 1
            except Exception as e:
                print(f'  FAILED (line {lineno}): {filename}  ({e})')
                fail += 1
        print(f'\nDone: {ok} succeeded, {fail} failed')
        return

    # ─── Default: direct conversion from the command line ────────────────
    if args.list_formats or (not args.images):
        list_formats()
        return

    if not args.format:
        print('ERROR: --format is required. Choose: 4bit | 8bit | 16bit | 24bit\n')
        list_formats()
        sys.exit(1)

    premult = not args.no_premult
    out_arg = args.output if len(args.images) == 1 else None
    output_dir = Path(args.output_dir) if args.output_dir else None
    if output_dir:
        output_dir.mkdir(parents=True, exist_ok=True)
    if args.output and len(args.images) > 1:
        print('WARNING: --output is ignored when converting multiple files.\n')
    if args.output and args.output_dir:
        output_dir = None

    print(f'Converting {len(args.images)} file(s) -> [{args.format}]\n')
    ok, fail, outputs = 0, 0, []
    for src in args.images:
        try:
            out = convert_one(src, args.format, out_arg, premult, args.dither, args.resize, output_dir,
                              vram_x=args.vram_x, vram_y=args.vram_y,
                              clut_x=args.clut_x, clut_y=args.clut_y,
                              palette_count=args.palette_count)
            outputs.append(out)
            ok += 1
        except Exception as e:
            print(f'  FAILED: {src}  ({e})')
            fail += 1
    print(f'\nDone: {ok} succeeded, {fail} failed')


if __name__ == '__main__':
    main()
