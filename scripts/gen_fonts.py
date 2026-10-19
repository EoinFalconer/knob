#!/usr/bin/env python3
"""Generates the apps' LVGL fonts from Apple's system fonts on this Mac.

    uv run --with fonttools scripts/gen_fonts.py [--force]   (npx for lv_font_conv)

scripts/gen_assets.sh runs this before each build. Fonts that already exist
are kept unless --force is given.

SF Pro is a variable font with an optical size axis: each size here is cut
at its own opsz, so small text gets SF Pro Text's spacing and big text SF Pro
Display's, as on Apple's platforms. Big numerals use SF Pro Rounded. Fonts
can have OpenType features baked into their character map, since
lv_font_conv ignores them: "tnum" for tabular figures where numbers must
hold still, "case" for the colon that sits at the figures' middle instead of
on the baseline.

The fonts are Apple's and licensed for Apple platforms; this is for a private
build, so keep the generated files out of public releases.
"""

import os
import subprocess
import sys
import tempfile

from fontTools import subset
from fontTools.ttLib import TTFont
from fontTools.varLib.instancer import instantiateVariableFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PRO = "/System/Library/Fonts/SFNS.ttf"
ROUNDED = "/System/Library/Fonts/SFNSRounded.ttf"

# SF's named instances.
WEIGHTS = {
    "light": 274,
    "regular": 400,
    "medium": 510,
    "semibold": 590,
    "bold": 700,
    "black": 1000,
}

TEXT = "0x20-0x7F,0xB0,0x2026,0x2013,0x2014,0x2018,0x2019,0x201C,0x201D,0xC5,0xD8,0xC6,0xE5,0xF8,0xE6"
DIGITS = "0x30-0x3A"
# Names, of people or rooms: all of Latin-1, and a real minus for balances.
NAMES = "0x20-0x7E,0xA0-0xFF,0x2026,0x2013,0x2014,0x2018,0x2019,0x201C,0x201D,0x2212"
INITIALS = "0x3F,0x41-0x5A,0xC0-0xDE"

# What the shared components in components/knob_ui draw in (sheets, lists,
# pills, captions): every app on them gets this same cut, so they look the
# same everywhere.
SHARED_FONTS = [
    ("sf_regular_16", PRO, "regular", 16, NAMES, ()),
    ("sf_regular_18", PRO, "regular", 18, NAMES, ()),
    ("sf_regular_22", PRO, "regular", 22, NAMES, ()),
    ("sf_medium_15", PRO, "medium", 15, NAMES, ()),
    ("sf_semibold_28", PRO, "semibold", 28, NAMES, ()),
]

# name, family, weight, size, glyph range, OpenType features to bake in
FONTS = {
    "radio": SHARED_FONTS
    + [
        ("sf_medium_13", PRO, "medium", 13, TEXT, ()),
        ("sf_semibold_16", PRO, "semibold", 16, TEXT, ()),
        ("sf_semibold_20", PRO, "semibold", 20, TEXT, ()),
        # Weather temperature and clock numerals: big, so rounded.
        (
            "sf_rounded_regular_132",
            ROUNDED,
            "regular",
            132,
            "0x2D,0x30-0x39,0xB0",
            ("tnum", "case"),
        ),
        (
            "sf_rounded_regular_200",
            ROUNDED,
            "regular",
            200,
            "0x31-0x33,0x36,0x39",
            (),
        ),
        ("sf_rounded_semibold_84", ROUNDED, "semibold", 84, DIGITS, ("case",)),
        # The boot screen's KN / OB.
        ("sf_rounded_black_200", ROUNDED, "black", 200, "0x42,0x4B,0x4E,0x4F", ()),
    ],
    "kaffi": SHARED_FONTS
    + [
        # A person's initial on their tile while there's no photo.
        ("sf_rounded_semibold_48", ROUNDED, "semibold", 48, INITIALS, ()),
    ],
    "calendar": [
        ("sf_regular_16", PRO, "regular", 16, TEXT, ()),
        ("sf_regular_18", PRO, "regular", 18, TEXT, ()),
        ("sf_regular_20", PRO, "regular", 20, TEXT, ()),
        ("sf_semibold_28", PRO, "semibold", 28, TEXT, ()),
        (
            "sf_rounded_semibold_52",
            ROUNDED,
            "semibold",
            52,
            "0x20-0x3A",
            ("tnum", "case"),
        ),
    ],
    "spotify": [
        ("sf_regular_16", PRO, "regular", 16, TEXT, ()),
        ("sf_regular_22", PRO, "regular", 22, TEXT, ()),
        ("sf_semibold_28", PRO, "semibold", 28, TEXT, ()),
    ],
}


def substitutions(font, feature):
    """Glyph-to-glyph replacements of a single-substitution GSUB feature."""
    gsub = font["GSUB"].table
    out = {}
    for record in gsub.FeatureList.FeatureRecord:
        if record.FeatureTag != feature:
            continue
        for index in record.Feature.LookupListIndex:
            for sub in gsub.LookupList.Lookup[index].SubTable:
                sub = getattr(sub, "ExtSubTable", sub)
                out.update(getattr(sub, "mapping", None) or {})
    return out


def bake(font, features):
    """Points the cmap at the alternates, since lv_font_conv ignores GSUB."""
    swaps = {}
    for feature in features:
        swaps.update(substitutions(font, feature))
    for table in font["cmap"].tables:
        if table.isUnicode():
            for code, glyph in list(table.cmap.items()):
                table.cmap[code] = swaps.get(glyph, glyph)


def codepoints(glyphs):
    out = []
    for part in glyphs.split(","):
        lo, _, hi = part.partition("-")
        out.extend(range(int(lo, 16), int(hi or lo, 16) + 1))
    return out


# lv_font_conv can't parse the full system font (AAT tables, thousands of
# glyphs), so it gets just the characters and the kerning.
def trim(font, glyphs):
    options = subset.Options()
    options.layout_features = ["kern"]
    options.drop_tables += [
        "morx",
        "kerx",
        "trak",
        "STAT",
        "feat",
        "ankr",
        "prop",
        "opbd",
        "lcar",
        "just",
        "bsln",
        "meta",
    ]
    options.notdef_outline = True
    trimmer = subset.Subsetter(options)
    trimmer.populate(unicodes=codepoints(glyphs))
    trimmer.subset(font)


def instance(family, weight, size, glyphs, features):
    font = TTFont(family)
    axes = {a.axisTag: a for a in font["fvar"].axes}
    location = {"wght": WEIGHTS[weight]}
    if "opsz" in axes:
        location["opsz"] = max(axes["opsz"].minValue, min(axes["opsz"].maxValue, size))
    for tag in ("wdth", "GRAD"):
        if tag in axes:
            location[tag] = axes[tag].defaultValue
    font = instantiateVariableFont(font, location)
    bake(font, features)
    trim(font, glyphs)
    path = tempfile.mktemp(suffix=".ttf")
    font.save(path)
    return path


def main():
    force = "--force" in sys.argv
    for app, fonts in FONTS.items():
        out_dir = os.path.join(ROOT, "apps", app, "main", "fonts")
        os.makedirs(out_dir, exist_ok=True)
        for name, family, weight, size, glyphs, features in fonts:
            out = os.path.join(out_dir, name + ".c")
            if os.path.exists(out) and not force:
                continue
            ttf = instance(family, weight, size, glyphs, features)
            subprocess.run(
                [
                    "npx",
                    "--yes",
                    "lv_font_conv",
                    "--bpp",
                    "4",
                    "--size",
                    str(size),
                    "--font",
                    ttf,
                    "--range",
                    glyphs,
                    "--format",
                    "lvgl",
                    "--no-compress",
                    "--lv-include",
                    "lvgl.h",
                    "-o",
                    out,
                ],
                check=True,
            )
            os.remove(ttf)
            print(f"{app}: {name}")


if __name__ == "__main__":
    main()
