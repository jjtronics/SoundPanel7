#!/usr/bin/env python3

from __future__ import annotations

import re
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
FONT_PATH = ROOT / "assets" / "fonts" / "Montserrat-800.ttf"
OUTPUT_PATH = ROOT / "src" / "ui" / "fonts" / "sp7_font_live_260.c"
POINT_SIZE = 260
FONT_NAME = "sp7_font_live_260"
SYMBOLS = ["E", "I", "L", "V"]


def run(*args: str) -> subprocess.CompletedProcess[str] | subprocess.CompletedProcess[bytes]:
    return subprocess.run(args, check=True, capture_output=True)


def parse_metrics(char: str) -> tuple[int, int]:
    result = subprocess.run(
        [
            "magick",
            "-background",
            "black",
            "-fill",
            "white",
            "-font",
            str(FONT_PATH),
            "-pointsize",
            str(POINT_SIZE),
            f"label:{char}",
            "-debug",
            "annotate",
            "null:",
        ],
        check=True,
        capture_output=True,
        text=True,
    )
    match = re.search(r"height: (\d+); ascent: (\d+); descent: (-?\d+);", result.stderr)
    if not match:
        raise RuntimeError(f"Unable to parse metrics for {char!r}")
    line_height = int(match.group(1))
    base_line = abs(int(match.group(3)))
    return line_height, base_line


def parse_pgm(data: bytes) -> tuple[int, int, list[int]]:
    if not data.startswith(b"P5"):
        raise RuntimeError("Expected binary PGM output")

    idx = 2

    def skip_ws_and_comments() -> None:
        nonlocal idx
        while idx < len(data):
            if data[idx] in b" \t\r\n":
                idx += 1
                continue
            if data[idx] == ord("#"):
                while idx < len(data) and data[idx] != ord("\n"):
                    idx += 1
                continue
            break

    def read_token() -> bytes:
        nonlocal idx
        skip_ws_and_comments()
        start = idx
        while idx < len(data) and data[idx] not in b" \t\r\n":
            idx += 1
        return data[start:idx]

    width = int(read_token())
    height = int(read_token())
    maxval = int(read_token())
    if maxval != 255:
        raise RuntimeError(f"Unexpected max value {maxval}")

    skip_ws_and_comments()
    pixels = list(data[idx : idx + (width * height)])
    if len(pixels) != width * height:
        raise RuntimeError("Unexpected pixel payload length")
    return width, height, pixels


def glyph_pixels(char: str) -> tuple[int, int, list[int]]:
    result = subprocess.run(
        [
            "magick",
            "-background",
            "black",
            "-fill",
            "white",
            "-font",
            str(FONT_PATH),
            "-pointsize",
            str(POINT_SIZE),
            f"label:{char}",
            "-alpha",
            "off",
            "-colorspace",
            "Gray",
            "-depth",
            "8",
            "pgm:-",
        ],
        check=True,
        capture_output=True,
    )
    return parse_pgm(result.stdout)


def trim_glyph(width: int, height: int, pixels: list[int]) -> tuple[int, int, int, int, list[int]]:
    rows = [pixels[y * width : (y + 1) * width] for y in range(height)]
    non_empty_y = [y for y, row in enumerate(rows) if any(row)]
    if not non_empty_y:
        return 0, 0, 0, 0, []

    top = non_empty_y[0]
    bottom = non_empty_y[-1]

    non_empty_x = [
        x
        for x in range(width)
        if any(rows[y][x] for y in range(top, bottom + 1))
    ]
    left = non_empty_x[0]
    right = non_empty_x[-1]

    trimmed: list[int] = []
    for y in range(top, bottom + 1):
        trimmed.extend(rows[y][left : right + 1])

    return left, top, right - left + 1, bottom - top + 1, trimmed


def pack_bpp4(pixels: list[int]) -> list[int]:
    nibs = [min(15, max(0, round(p * 15 / 255))) for p in pixels]
    out: list[int] = []
    for i in range(0, len(nibs), 2):
        hi = nibs[i]
        lo = nibs[i + 1] if i + 1 < len(nibs) else 0
        out.append((hi << 4) | lo)
    return out


def format_bytes(values: list[int], per_line: int = 16) -> str:
    lines: list[str] = []
    for i in range(0, len(values), per_line):
        chunk = values[i : i + per_line]
        lines.append("    " + ", ".join(f"0x{value:02x}" for value in chunk))
    return ",\n".join(lines)


def generate() -> str:
    _, base_line = parse_metrics("L")
    rendered = [(char, *glyph_pixels(char)) for char in SYMBOLS]
    line_height = max(height for _, _, height, _ in rendered)

    bitmap_chunks: list[int] = []
    glyph_dsc_rows: list[str] = [
        "    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0} /* id = 0 reserved */"
    ]

    bitmap_index = 0
    for glyph_id, (char, width, height, pixels) in enumerate(rendered, start=1):
        left, top, box_w, box_h, trimmed = trim_glyph(width, height, pixels)
        packed = pack_bpp4(trimmed)
        baseline_y = height - base_line - 1
        ofs_y = baseline_y - (top + box_h - 1)
        bitmap_chunks.extend(packed)
        glyph_dsc_rows.append(
            f"    {{.bitmap_index = {bitmap_index}, .adv_w = {width * 16}, .box_w = {box_w}, .box_h = {box_h}, .ofs_x = {left}, .ofs_y = {ofs_y}}}"
        )
        bitmap_index += len(packed)
    return f"""/*******************************************************************************
 * Size: {POINT_SIZE} px
 * Bpp: 4
 * Opts: generated locally from {FONT_PATH.name} for symbols {''.join(SYMBOLS)}
 ******************************************************************************/

#ifdef LV_LVGL_H_INCLUDE_SIMPLE
#include "lvgl.h"
#else
#include "lvgl/lvgl.h"
#endif

#ifndef SP7_FONT_LIVE_260
#define SP7_FONT_LIVE_260 1
#endif

#if SP7_FONT_LIVE_260

/*-----------------
 *    BITMAPS
 *----------------*/

static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {{
{format_bytes(bitmap_chunks)}
}};

/*---------------------
 *  GLYPH DESCRIPTION
 *--------------------*/

static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {{
{",\n".join(glyph_dsc_rows)}
}};

/*---------------------
 *  CHARACTER MAPPING
 *--------------------*/

static const uint16_t unicode_list_0[] = {{
    0x0, 0x4, 0x7, 0x11
}};

static const lv_font_fmt_txt_cmap_t cmaps[] = {{
    {{
        .range_start = 69, .range_length = 18, .glyph_id_start = 1,
        .unicode_list = unicode_list_0, .glyph_id_ofs_list = NULL, .list_length = 4, .type = LV_FONT_FMT_TXT_CMAP_SPARSE_TINY
    }}
}};

/*--------------------
 *  ALL CUSTOM DATA
 *--------------------*/

#if LVGL_VERSION_MAJOR == 8
static lv_font_fmt_txt_glyph_cache_t cache;
#endif

#if LVGL_VERSION_MAJOR >= 8
static const lv_font_fmt_txt_dsc_t font_dsc = {{
#else
static lv_font_fmt_txt_dsc_t font_dsc = {{
#endif
    .glyph_bitmap = glyph_bitmap,
    .glyph_dsc = glyph_dsc,
    .cmaps = cmaps,
    .kern_dsc = NULL,
    .kern_scale = 0,
    .cmap_num = 1,
    .bpp = 4,
    .kern_classes = 0,
    .bitmap_format = 0,
#if LVGL_VERSION_MAJOR == 8
    .cache = &cache
#endif
}};

/*-----------------
 *  PUBLIC FONT
 *----------------*/

#if LVGL_VERSION_MAJOR >= 8
const lv_font_t {FONT_NAME} = {{
#else
lv_font_t {FONT_NAME} = {{
#endif
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,
    .line_height = {line_height},
    .base_line = {base_line},
#if !(LVGL_VERSION_MAJOR == 6 && LVGL_VERSION_MINOR == 0)
    .subpx = LV_FONT_SUBPX_NONE,
#endif
#if LV_VERSION_CHECK(7, 4, 0) || LVGL_VERSION_MAJOR >= 8
    .underline_position = -33,
    .underline_thickness = 13,
#endif
    .dsc = &font_dsc,
#if LV_VERSION_CHECK(8, 2, 0) || LVGL_VERSION_MAJOR >= 9
    .fallback = NULL,
#endif
    .user_data = NULL,
}};

#endif /*#if SP7_FONT_LIVE_260*/
"""


def main() -> None:
    OUTPUT_PATH.write_text(generate(), encoding="utf-8")
    print(f"Generated {OUTPUT_PATH.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
