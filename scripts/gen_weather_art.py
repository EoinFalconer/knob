#!/usr/bin/env python3
"""Builds the weather face art: painted skies and full-color icons.

    python3 -m venv /tmp/wx && /tmp/wx/bin/pip install numpy pillow
    /tmp/wx/bin/python scripts/gen_weather_art.py

Skies are painted here and stored as 360x360 baseline JPEGs (TJpgDec can't
read progressive ones). The firmware decodes the one it needs with ordered
dithering, so the gradients don't band on the RGB565 panel. Everything is
seeded, so reruns produce identical files.

Icons are Meteocons "fill" by Bas Milius (MIT, github.com/basmilius/meteocons),
rendered with rsvg-convert and stored as RGB565A8.
"""

import io
import subprocess
import tarfile
import tempfile
import urllib.request
from pathlib import Path

import numpy as np
from PIL import Image

SIZE = 360
ICON_SIZES = {"lg": 44, "sm": 30}
ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "apps/radio/main/ui"
PREVIEW = ROOT / "apps/radio/build/weather_art"
METEOCONS = "https://registry.npmjs.org/@meteocons/svg-static/-/svg-static-0.1.0.tgz"

ICONS = {
    "clear_day": "clear-day",
    "clear_night": "clear-night",
    "fair_day": "mostly-clear-day",
    "fair_night": "mostly-clear-night",
    "partly_day": "partly-cloudy-day",
    "partly_night": "partly-cloudy-night",
    "cloudy": "cloudy",
    "fog": "fog",
    "drizzle": "drizzle",
    "rain": "rain",
    "showers_day": "partly-cloudy-day-rain",
    "showers_night": "partly-cloudy-night-rain",
    "thunder": "thunderstorms-rain",
    "snow": "snow",
    "snow_showers_day": "partly-cloudy-day-snow",
    "snow_showers_night": "partly-cloudy-night-snow",
    "sleet": "sleet",
}


def blur(a, sigma):
    r = int(sigma * 3)
    k = np.exp(-(np.arange(-r, r + 1) ** 2) / (2 * sigma * sigma))
    k /= k.sum()
    a = np.apply_along_axis(
        lambda v: np.convolve(np.pad(v, r, "edge"), k, "valid"), 0, a
    )
    return np.apply_along_axis(
        lambda v: np.convolve(np.pad(v, r, "edge"), k, "valid"), 1, a
    )


def fbm(seed, octaves=4, base=3, persistence=0.5, stretch=(1.0, 1.0)):
    """Fractal value noise in [0, 1]. stretch scales detail per axis (y, x)."""
    rng = np.random.default_rng(seed)
    acc = np.zeros((SIZE, SIZE))
    amp, total = 1.0, 0.0
    for o in range(octaves):
        n = base * 2**o
        gh = max(2, int(n * stretch[0]))
        gw = max(2, int(n * stretch[1]))
        grid = rng.random((gh, gw)).astype(np.float32)
        layer = Image.fromarray(grid, mode="F").resize((SIZE, SIZE), Image.BICUBIC)
        acc += amp * np.asarray(layer)
        total += amp
        amp *= persistence
    acc /= total
    # Value noise shows its grid as a crumpled texture; a slight blur hides it.
    acc = blur(acc, 2.5)
    return (acc - acc.min()) / (acc.max() - acc.min())


def smoothstep(a, b, x):
    t = np.clip((x - a) / (b - a), 0, 1)
    return t * t * (3 - 2 * t)


def rgb(hex_color):
    v = int(hex_color.lstrip("#"), 16)
    return np.array([(v >> 16) & 255, (v >> 8) & 255, v & 255], dtype=np.float64)


def vertical(top, bottom, curve=1.0):
    t = np.linspace(0, 1, SIZE)[:, None, None] ** curve
    return np.broadcast_to(rgb(top) * (1 - t) + rgb(bottom) * t, (SIZE, SIZE, 3)).copy()


def glow(img, cx, cy, radius, color, strength):
    y, x = np.mgrid[0:SIZE, 0:SIZE]
    d = np.sqrt((x - cx) ** 2 + (y - cy) ** 2) / radius
    a = (np.exp(-d * d) * strength)[..., None]
    return img + (rgb(color) - img) * a


def clouds(
    img,
    seed,
    cover,
    light,
    shadow,
    softness=0.12,
    height=None,
    stretch=(1.0, 1.0),
    opacity=1.0,
):
    """Cumulus lit from above: tops catch the light, bellies go blue-grey."""
    d = fbm(seed, stretch=stretch)
    if height is not None:
        y = np.linspace(0, 1, SIZE)[:, None]
        d = d * (0.55 + 0.45 * smoothstep(height[0], height[1], y))
    threshold = 1 - cover
    mask = smoothstep(threshold, threshold + softness, d)
    # How much cloud sits just above each pixel decides how lit it is.
    above = np.vstack([np.repeat(d[:1], 10, axis=0), d[:-10]])
    lit = np.clip(0.55 + (d - above) * 4.0, 0, 1)
    body = np.clip((d - threshold) / max(1e-3, 1 - threshold), 0, 1)
    shade = np.clip(lit * 0.7 + (1 - body) * 0.45, 0, 1)[..., None]
    col = rgb(shadow) * (1 - shade) + rgb(light) * shade
    a = (mask * opacity)[..., None]
    return img * (1 - a) + col * a


def stars(img, seed, count, max_y=1.0):
    rng = np.random.default_rng(seed)
    out = img.copy()
    for _ in range(count):
        x = rng.integers(0, SIZE)
        y = rng.integers(0, int(SIZE * max_y))
        b = rng.uniform(0.35, 1.0)
        out[y, x] = out[y, x] * (1 - b) + 255 * b
        if b > 0.85 and 0 < x < SIZE - 1 and 0 < y < SIZE - 1:
            for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                out[y + dy, x + dx] = out[y + dy, x + dx] * 0.7 + 255 * 0.3
    return out


def streaks(img, seed, count, length, angle, color, opacity):
    """Rain: short, slanted, faint strokes."""
    rng = np.random.default_rng(seed)
    out = img.copy()
    dx, dy = np.sin(angle), np.cos(angle)
    c = rgb(color)
    for _ in range(count):
        x0, y0 = rng.uniform(0, SIZE), rng.uniform(0, SIZE)
        a = rng.uniform(0.4, 1.0) * opacity
        for s in range(length):
            x, y = int(x0 + dx * s), int(y0 + dy * s)
            if 0 <= x < SIZE and 0 <= y < SIZE:
                out[y, x] = out[y, x] * (1 - a) + c * a
    return out


def flakes(img, seed, count, opacity):
    rng = np.random.default_rng(seed)
    out = img.copy()
    y, x = np.mgrid[-3:4, -3:4]
    for _ in range(count):
        cx, cy = rng.integers(3, SIZE - 3, size=2)
        r = rng.uniform(0.8, 2.2)
        a = (np.clip(1 - np.sqrt(x * x + y * y) / r, 0, 1) * opacity)[..., None]
        patch = out[cy - 3 : cy + 4, cx - 3 : cx + 4]
        out[cy - 3 : cy + 4, cx - 3 : cx + 4] = patch * (1 - a) + 255 * a
    return out


def grain(img, seed, amount=2.0):
    rng = np.random.default_rng(seed)
    return img + rng.normal(0, amount, img.shape)


# ─── Skies ──────────────────────────────────────────────────────────────────


def clear_day():
    img = vertical("#1B5FC4", "#8CC4F0", 0.9)
    img = glow(img, 300, 40, 150, "#FFF6D8", 0.75)
    img = glow(img, 300, 40, 40, "#FFFFFF", 0.6)
    # A few fair-weather clouds low on the horizon, under the forecast row.
    return clouds(img, 4, 0.42, "#FFFFFF", "#9DB9D8", 0.12, height=(0.6, 1.0))


def partly_day():
    img = vertical("#1F67CC", "#94C8F0", 0.9)
    img = glow(img, 290, 30, 130, "#FFF4D0", 0.5)
    return clouds(img, 7, 0.42, "#E8F0F8", "#6F8DB3", 0.14, height=(0.2, 0.9))


def cloudy_day():
    # Kept mid-tone so the big white temperature still reads on top.
    img = vertical("#506A89", "#8A9FB6")
    img = clouds(img, 11, 0.75, "#B9C6D3", "#5F7290", 0.25)
    return clouds(img, 12, 0.4, "#D3DCE5", "#6E8199", 0.15, height=(0.0, 1.0))


def rain_day():
    img = vertical("#3E4E62", "#6D7E92")
    img = clouds(img, 3, 0.8, "#A3B0BF", "#4A586A", 0.3)
    return streaks(img, 21, 260, 16, 0.25, "#D6E2EE", 0.35)


def thunder():
    img = vertical("#2A2840", "#55546E")
    img = clouds(img, 5, 0.8, "#8C8AA6", "#34324A", 0.3)
    img = glow(img, 120, 90, 90, "#E9DFFF", 0.35)
    return streaks(img, 22, 220, 16, 0.25, "#CFCBE6", 0.3)


def snow_day():
    img = vertical("#617791", "#A3B2C3")
    img = clouds(img, 9, 0.7, "#CBD5DF", "#71839B", 0.3)
    return flakes(img, 23, 140, 0.85)


def fog():
    img = vertical("#6E7B88", "#A7AFB8")
    return clouds(
        img, 13, 0.7, "#C2C9D0", "#7F8A96", 0.4, stretch=(1.6, 0.3), opacity=0.8
    )


def clear_night():
    img = vertical("#040B1F", "#1A3466", 1.2)
    img = stars(img, 31, 160, 0.9)
    img = glow(img, 270, 70, 110, "#3A5A90", 0.35)
    return glow(img, 270, 70, 22, "#F2F4FF", 0.9)


def cloudy_night():
    img = vertical("#0A1226", "#2A3A57")
    img = stars(img, 32, 70, 0.6)
    return clouds(img, 14, 0.5, "#5B6B86", "#1C263A", 0.16, height=(0.2, 1.0))


def rain_night():
    img = vertical("#0E1622", "#2B3646")
    img = clouds(img, 15, 0.8, "#4C5A6C", "#18212D", 0.3)
    return streaks(img, 24, 240, 16, 0.25, "#9FB0C3", 0.3)


def snow_night():
    img = vertical("#1A2234", "#3E4A60")
    img = clouds(img, 16, 0.7, "#5E6B82", "#262F42", 0.3)
    return flakes(img, 25, 140, 0.7)


SKIES = {
    "clear_day": clear_day,
    "partly_day": partly_day,
    "cloudy_day": cloudy_day,
    "rain_day": rain_day,
    "thunder": thunder,
    "snow_day": snow_day,
    "fog": fog,
    "clear_night": clear_night,
    "cloudy_night": cloudy_night,
    "rain_night": rain_night,
    "snow_night": snow_night,
}


def scrim(img):
    """Darkens behind the time at the top and the forecast at the bottom, the
    way iOS keeps white text legible on photos."""
    y = np.linspace(0, 1, SIZE)[:, None, None]
    shade = 1 - 0.28 * smoothstep(0.55, 1.0, y) - 0.12 * smoothstep(0.25, 0.0, y)
    return img * shade


def to_jpeg(img, seed):
    pixels = np.clip(grain(scrim(img), seed), 0, 255).astype(np.uint8)
    buf = io.BytesIO()
    Image.fromarray(pixels, "RGB").save(
        buf, "JPEG", quality=88, progressive=False, optimize=True, subsampling="4:2:0"
    )
    return buf.getvalue()


def c_bytes(data):
    return ",\n".join(
        "  " + ",".join(f"0x{b:02x}" for b in data[i : i + 16])
        for i in range(0, len(data), 16)
    )


def rgb565a8(png_path):
    """LVGL RGB565A8: a little-endian RGB565 plane, then an alpha plane."""
    px = np.asarray(Image.open(png_path).convert("RGBA")).astype(np.uint16)
    r, g, b, a = (px[..., i] for i in range(4))
    c = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
    return c.astype("<u2").tobytes() + a.astype(np.uint8).tobytes()


def fetch_icons(workdir):
    tgz = workdir / "meteocons.tgz"
    urllib.request.urlretrieve(METEOCONS, tgz)
    with tarfile.open(tgz) as t:
        t.extractall(workdir, filter="data")
    pngs = {}
    for size_name, size in ICON_SIZES.items():
        for name, svg in ICONS.items():
            png = PREVIEW / f"icon_{size_name}_{name}.png"
            subprocess.run(
                ["rsvg-convert", "-w", str(size), "-h", str(size),
                 workdir / "package/fill" / f"{svg}.svg", "-o", png],
                check=True,
            )
            pngs[f"{size_name}_{name}"] = (png, size)
    return pngs


def main():
    PREVIEW.mkdir(parents=True, exist_ok=True)
    header = [
        "// Generated by scripts/gen_weather_art.py. Do not edit.",
        "#pragma once",
        "",
        "#include \"lvgl.h\"",
        "",
        "#include <cstddef>",
        "#include <cstdint>",
        "",
    ]
    body = [
        "// Generated by scripts/gen_weather_art.py. Do not edit.",
        "#include \"ui/weather_assets.h\"",
        "",
    ]
    for i, (name, paint) in enumerate(SKIES.items()):
        jpg = to_jpeg(paint(), seed=100 + i)
        (PREVIEW / f"sky_{name}.jpg").write_bytes(jpg)
        header.append(f"extern const uint8_t sky_{name}_jpg[];")
        header.append(f"extern const size_t sky_{name}_jpg_len;")
        body.append(f"const uint8_t sky_{name}_jpg[] = {{\n{c_bytes(jpg)}\n}};")
        body.append(f"const size_t sky_{name}_jpg_len = sizeof(sky_{name}_jpg);")
        print(f"sky_{name}: {len(jpg) / 1024:.1f} KB")

    with tempfile.TemporaryDirectory() as tmp:
        for name, (png, size) in fetch_icons(Path(tmp)).items():
            data = rgb565a8(png)
            header.append(f"extern const lv_image_dsc_t icon_{name};")
            body.append(f"static const uint8_t icon_{name}_map[] = {{\n{c_bytes(data)}\n}};")
            body.append(
                f"const lv_image_dsc_t icon_{name} = {{\n"
                f"    .header = {{.magic = LV_IMAGE_HEADER_MAGIC,\n"
                f"               .cf = LV_COLOR_FORMAT_RGB565A8,\n"
                f"               .flags = 0,\n"
                f"               .w = {size},\n"
                f"               .h = {size},\n"
                f"               .stride = {size * 2},\n"
                f"               .reserved_2 = 0}},\n"
                f"    .data_size = sizeof(icon_{name}_map),\n"
                f"    .data = icon_{name}_map,\n"
                f"    .reserved = nullptr,\n"
                f"    .reserved_2 = nullptr,\n"
                f"}};"
            )
        print(f"{len(ICONS)} icons at {list(ICON_SIZES.values())} px")

    (OUT / "weather_assets.h").write_text("\n".join(header) + "\n")
    (OUT / "weather_assets.cpp").write_text("\n".join(body) + "\n")


if __name__ == "__main__":
    main()
