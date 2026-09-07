# PS1 TIM Tool

A command-line converter between standard image formats and Sony's official **TIM** texture format for the original PlayStation (PS1).

Convert PNG/JPG/BMP/TGA/WebP and more into `.tim`, inspect and validate existing `.tim` files, extract textures back out to PNG, and diff two `.tim` files to check whether a modified texture is safe to inject back into a game.

Built directly from the documented TIM header layout — no game-specific hacks, no dependency on any emulator or SDK.

## Why this exists

Most PS1 texture tools are either GUI-only, Windows-only, or long abandoned. This is a single, dependency-light Python script that:

- Supports all four real TIM pixel formats (there is no 32-bit mode on PS1 — see [Format notes](#format-notes))
- Computes `TPAGE` and `CLUT ID` the same way the original SDK did, so the values it prints are ready to drop into your draw calls
- Handles the PS1 GPU's "black is transparent" rule correctly instead of silently corrupting pure black pixels
- Applies proper rounding (not bit-truncation) and optional Floyd-Steinberg dithering when reducing to 15-bit color, to avoid visible banding
- Works entirely offline, with no telemetry, no network calls, no external services

## Installation

### Option 1 — Prebuilt binaries (no Python required)

Prebuilt executables for Windows (x86_64 / x86 / arm64), Linux (x86_64 / arm64), and macOS (arm64) are published on the [Releases](https://github.com/PS2HomeDeveloper/ps1-tim-tool/releases) page. Download the binary for your platform and run it directly — no installation, no dependencies.

### Option 2 — From source

Requires Python 3.9+ and Pillow.

```bash
git clone https://github.com/PS2HomeDeveloper/ps1-tim-tool.git
cd ps1-tim-tool
pip install Pillow
python3 ps1_tim_tool.py --list-formats
```

## Quick start

```bash
# Convert a single image
python3 ps1_tim_tool.py hero.png --format 8bit

# Convert with dithering to reduce color banding
python3 ps1_tim_tool.py sky.png --format 16bit --dither

# Batch convert multiple files
python3 ps1_tim_tool.py *.png --format 8bit

# Place a texture at a specific VRAM location and get its TPAGE/CLUT ID
python3 ps1_tim_tool.py sprite.png --format 8bit --vram-x 320 --vram-y 0 --clut-x 320 --clut-y 480

# Inspect an existing TIM file
python3 ps1_tim_tool.py texture.tim --info

# Validate a TIM file's structure
python3 ps1_tim_tool.py texture.tim --verify

# Extract a TIM back to PNG
python3 ps1_tim_tool.py texture.tim --extract png

# Compare two TIM files before injecting a modified one back into a game
python3 ps1_tim_tool.py --diff --original original.tim --modified edited.tim
```

## Supported formats

| Format  | Colors           | CLUT | Transparency        | Typical use                          |
|---------|------------------|------|----------------------|---------------------------------------|
| `4bit`  | 16 (per palette) | Yes  | 1-bit (opaque/transparent) | UI, fonts, small icons           |
| `8bit`  | 256 (per palette)| Yes  | 1-bit (opaque/transparent) | Most in-game textures            |
| `16bit` | Direct RGBA5551  | No   | 1-bit (opaque/transparent) | Textures needing full color range |
| `24bit` | Direct RGB888    | No   | None                 | Splash screens, static backgrounds   |

## Command reference

```
ps1_tim_tool.py <image(s)> --format <fmt> [options]
ps1_tim_tool.py <file.tim> --info
ps1_tim_tool.py <file.tim> --verify
ps1_tim_tool.py <file.tim> --extract <ext>
ps1_tim_tool.py --list <file.txt>
ps1_tim_tool.py --diff --original <a.tim> --modified <b.tim>
```

| Option                  | Description                                                              |
|--------------------------|---------------------------------------------------------------------------|
| `--format`               | `4bit` \| `8bit` \| `16bit` \| `24bit`                                    |
| `--output`               | Output file path (single file only)                                       |
| `--output-dir`           | Output directory for batch conversions                                    |
| `--resize`               | `up` \| `down` \| `WxH` — resize before conversion                        |
| `--dither`               | Floyd-Steinberg dithering (4bit/8bit palette + 16bit channel quantization) |
| `--no-premult`           | Disable alpha premultiplication                                           |
| `--vram-x`, `--vram-y`   | Image position in VRAM (auto-computes `TPAGE`)                            |
| `--clut-x`, `--clut-y`   | CLUT position in VRAM (auto-computes `CLUT ID`)                           |
| `--palette-count`        | Embed multiple CLUT rows for runtime recoloring (4bit/8bit)                |
| `--palette-index`        | With `--extract`, choose which palette to render from a multi-palette file |
| `--info`                 | Print header details of an existing `.tim` file                           |
| `--verify`               | Validate a `.tim` file's structural integrity                             |
| `--extract <ext>`        | Convert a `.tim` back to an image (`png`, `bmp`, `tga`, `tiff`, `webp`, `ppm`) |
| `--diff`                 | Compare two `.tim` files or two folders of `.tim` files                   |
| `--list <file.txt>`      | Batch convert from a text file (`filename format` per line)               |
| `--list-formats`         | Print the supported formats and full option list                          |

## Format notes

The PS1 GPU is a different chip with a different memory model than the PS2's Graphics Synthesizer, and this tool reflects that rather than papering over it:

- **No 32-bit mode.** The PS1 has no full 8-bit alpha channel — only a single semi-transparency bit (`STP`) per pixel. 24-bit direct color exists but carries no transparency at all.
- **No mipmaps, no VRAM swizzle.** The PS1 GPU reads texture memory linearly; these concepts are specific to the PS2's GS and don't apply here.
- **`0x0000` means fully transparent.** This is a real hardware rule, not a bug: a pixel value of exactly zero (black, STP off) is always transparent regardless of intent. The tool detects pixels that would collide with this rule (opaque pure black) and nudges them by one bit so they render correctly.
- **VRAM placement is manual**, same as on real hardware. `--vram-x/y` and `--clut-x/y` let you set the exact position Sony's own tools exposed, and the tool computes the resulting `TPAGE` and `CLUT ID` for you.
- **Power-of-2 dimensions are not mandatory** on PS1 (unlike PS2), but the tool still warns when a texture isn't power-of-2, since that convention avoided wasting VRAM in most PS1-era pipelines.

## Contributing

Issues and pull requests are welcome. If you're fixing a conversion accuracy issue, please include a before/after sample and, if possible, confirmation on real hardware or a cycle-accurate emulator (e.g. DuckStation in hardcore/accuracy mode).

## License

MIT — see [LICENSE](LICENSE).
