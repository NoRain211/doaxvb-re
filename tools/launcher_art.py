# SPDX-License-Identifier: GPL-3.0-or-later
"""Copy launcher art and the program icon from a user-owned disc into private/.

Textures are found by their runtime replacement key (format, size, SHA-256 of
the original data), so no game file names are recorded. Existing files are kept.
"""
import argparse
import math
from pathlib import Path
import struct
import sys

from afs_extract import ROOT, read_entries
from texture_rip import decode, png, texture_key, texture_payloads

ART = {
    "button-a.png": "0b-64x64-m1-d35c90e26dd7e0077ab4a68b684b654a7812fa847df37b20601c410717ccc1a7",
    "button-b.png": "0b-64x64-m1-7e15b915efc9562f5796dcd3ccd60c25b03620f7b13e91739626f85c1ee0d063",
    "logo.png": "0b-512x256-m1-d24d2c8fa5a80224077bfab17809e52661717ed3de29643b54d0eecd5e99a31b",
    # A sunny beach location preview; the launcher scales it to fill the window.
    "beach.png": "0f-256x256-m1-d3adcb372ace051ecdb86e8e67ec72d7c684e9a51fc58fe1750dd5a194cfe69b",
}
EMBLEM = (190, 47, 330, 155)  # left, top, right, bottom of the logo's wave curl
ICON_SIZES = (256, 48, 32, 16)


def find_textures(disc, keys):
    """Return {key: (width, height, rgba)} for keys found in the disc's AFS archives."""
    found = {}
    for archive in sorted(p for p in Path(disc).rglob("*") if p.suffix.lower() == ".afs"):
        with archive.open("rb") as source:
            for entry in read_entries(source):
                source.seek(entry.offset)
                head = source.read(16)
                if head[:4] != b"XPR0" and head[12:16] != struct.pack("<I", 0x80000000):
                    continue
                source.seek(entry.offset)
                try:
                    for _, texture, payload, palette, error in texture_payloads(source.read(entry.size), True):
                        if error:
                            continue
                        key = texture_key(texture, payload, palette)
                        if key in keys and key not in found:
                            found[key] = (texture.width, texture.height, decode(texture, payload, palette))
                except ValueError:
                    continue
                if len(found) == len(keys):
                    return found
    return found


def square_crop(width, rgba, box):
    """Center box on a transparent square canvas."""
    left, top, right, bottom = box
    side = max(right - left, bottom - top)
    x0, y0 = (side - (right - left)) // 2, (side - (bottom - top)) // 2
    out = bytearray(side * side * 4)
    for y in range(top, bottom):
        row = rgba[(y * width + left) * 4:(y * width + right) * 4]
        start = ((y - top + y0) * side + x0) * 4
        out[start:start + len(row)] = row
    return side, bytes(out)


def resize(side, rgba, size):
    """Downscale a square RGBA image by averaging premultiplied samples."""
    samples = max(1, math.ceil(side / size))
    step = side / size / samples
    out = bytearray(size * size * 4)
    for y in range(size):
        for x in range(size):
            r = g = b = a = 0
            for j in range(samples):
                sy = int((y * samples + j + 0.5) * step)
                for i in range(samples):
                    p = (sy * side + int((x * samples + i + 0.5) * step)) * 4
                    alpha = rgba[p + 3]
                    r += rgba[p] * alpha; g += rgba[p + 1] * alpha; b += rgba[p + 2] * alpha; a += alpha
            if a:
                out[(y * size + x) * 4:(y * size + x + 1) * 4] = bytes(
                    (r // a, g // a, b // a, a // (samples * samples)))
    return bytes(out)


def ico(images):
    """Pack {size: png bytes} as a Windows .ico with PNG entries."""
    data = struct.pack("<HHH", 0, 1, len(images))
    offset = 6 + 16 * len(images)
    for size, image in images.items():
        data += struct.pack("<BBBBHHII", size % 256, size % 256, 0, 0, 1, 32, len(image), offset)
        offset += len(image)
    return data + b"".join(images.values())


def install(disc, private=ROOT / "private"):
    """Write missing launcher art and doaxbv.ico; return the files written."""
    art, icon = Path(private) / "launcher", Path(private) / "doaxbv.ico"
    missing = {name: key for name, key in ART.items() if not (art / name).exists()}
    keys = set(missing.values()) | ({ART["logo.png"]} if not icon.exists() else set())
    if not keys:
        return []
    found = find_textures(disc, keys)
    written = []
    for name, key in missing.items():
        if key in found:
            art.mkdir(parents=True, exist_ok=True)
            (art / name).write_bytes(png(*found[key]))
            written.append(art / name)
    if not icon.exists() and ART["logo.png"] in found:
        width, _, rgba = found[ART["logo.png"]]
        side, square = square_crop(width, rgba, EMBLEM)
        icon.write_bytes(ico({size: png(size, size, resize(side, square, size)) for size in ICON_SIZES}))
        written.append(icon)
    return written


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("disc", type=Path, help="Extracted disc folder, such as private/setup-*/imported-disc/disc")
    args = parser.parse_args()
    try:
        written = install(args.disc)
    except (OSError, ValueError) as error:
        print(f"Launcher art failed: {error}", file=sys.stderr)
        return 1
    print(f"Wrote {len(written)} launcher art files." if written else "Launcher art is already in place.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
