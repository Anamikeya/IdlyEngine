#!/usr/bin/env python3
"""Regenerates the binary texture fixtures under Tests/Assets. Deterministic; run from anywhere.

    python -I Tests/Assets/tools/generate_textures.py
"""

import pathlib
import struct

ROOT = pathlib.Path(__file__).resolve().parents[1]


def checker(width, height, cell):
    """RGBA rows top to bottom: white/red checker with alpha 255 on white, 128 on red."""
    rows = []
    for y in range(height):
        row = []
        for x in range(width):
            if ((x // cell) + (y // cell)) % 2 == 0:
                row.append((255, 255, 255, 255))
            else:
                row.append((255, 0, 0, 128))
        rows.append(row)
    return rows


def tga_header(image_type, width, height, bpp, descriptor):
    return struct.pack("<BBBHHBHHHHBB", 0, 0, image_type, 0, 0, 0, 0, 0, width, height, bpp, descriptor)


def write_uncompressed_rgba_bottom_left(path, rows):
    height, width = len(rows), len(rows[0])
    data = bytearray(tga_header(2, width, height, 32, 8))  # 8 alpha bits, bottom-left origin.
    for row in reversed(rows):
        for r, g, b, a in row:
            data += bytes((b, g, r, a))
    path.write_bytes(bytes(data))


def write_rle_rgb_top_left(path, rows):
    height, width = len(rows), len(rows[0])
    data = bytearray(tga_header(10, width, height, 24, 0x20))  # Top-left origin.
    pixels = [p for row in rows for p in row]
    i = 0
    while i < len(pixels):
        run = 1
        while i + run < len(pixels) and run < 128 and pixels[i + run][:3] == pixels[i][:3]:
            run += 1
        r, g, b, _ = pixels[i]
        if run > 1:
            data += bytes((0x80 | (run - 1), b, g, r))
            i += run
        else:
            data += bytes((0, b, g, r))  # Raw packet of one pixel.
            i += 1
    path.write_bytes(bytes(data))


def write_ppm(path, width, height):
    data = bytearray(f"P6\n# gradient fixture\n{width} {height}\n255\n".encode("ascii"))
    for y in range(height):
        for x in range(width):
            data += bytes((x * 255 // (width - 1), y * 255 // (height - 1), 64))
    path.write_bytes(bytes(data))


def main():
    textures = ROOT / "editor" / "textures"
    textures.mkdir(parents=True, exist_ok=True)
    write_uncompressed_rgba_bottom_left(textures / "checker.tga", checker(8, 8, 2))
    write_rle_rgb_top_left(textures / "checker_rle.tga", checker(8, 8, 4))
    write_ppm(textures / "gradient.ppm", 4, 2)

    invalid = ROOT / "invalid"
    invalid.mkdir(parents=True, exist_ok=True)
    full = (textures / "checker.tga").read_bytes()
    (invalid / "truncated.tga").write_bytes(full[: len(full) - 7])
    (invalid / "bad_maxval.ppm").write_bytes(b"P6\n1 1\n65535\n\x00\x00\x00\x00\x00\x00")


if __name__ == "__main__":
    main()
