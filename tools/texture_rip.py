# SPDX-License-Identifier: GPL-3.0-or-later
"""Rip Xbox XPR textures to private PNGs using the runtime replacement key.

Format fields follow Cxbx-Reloaded's public XbD3D8Types.h. The key hashes
original mip storage, followed by the P8 palette, before any pixel conversion.
"""
import argparse
from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import struct
import sys
import zlib

from afs_extract import ROOT, private_output


LINEAR = {0x10: 2, 0x11: 5, 0x12: 6, 0x13: 0, 0x1c: 3,
          0x1d: 4, 0x1e: 7, 0x1f: 0x19, 0x20: 0x1a}
BPP = {0: 1, 2: 2, 3: 2, 4: 2, 5: 2, 6: 4, 7: 4, 11: 1,
       0x19: 1, 0x1a: 2, 0x3c: 4}
BLOCK = {0x0c: 8, 0x0e: 16, 0x0f: 16}


@dataclass(frozen=True)
class Texture:
    format: int
    width: int
    height: int
    mips: int
    pitch: int = 0

    def spans(self):
        if not (0 < self.width <= 4096 and 0 < self.height <= 4096):
            raise ValueError("Texture dimensions out of bounds")
        if not 1 <= self.mips <= max(self.width, self.height).bit_length():
            raise ValueError("Invalid mip count")
        fmt = LINEAR.get(self.format, self.format)
        if self.format in LINEAR:
            if self.mips != 1 or self.pitch < self.width * BPP[fmt]:
                raise ValueError("Invalid linear texture pitch or mip count")
            # Capture excludes unused padding after the last scanline.
            return [(self.height - 1) * self.pitch + self.width * BPP[fmt]]
        if self.pitch or self.width & (self.width - 1) or self.height & (self.height - 1):
            raise ValueError("Invalid swizzled texture shape")
        if fmt not in BPP and fmt not in BLOCK:
            raise ValueError(f"Unsupported format {fmt:02x}")
        result = []
        for level in range(self.mips):
            w, h = max(1, self.width >> level), max(1, self.height >> level)
            result.append(((w + 3) // 4) * ((h + 3) // 4) * BLOCK[fmt]
                          if fmt in BLOCK else w * h * BPP[fmt])
        return result


def describe(format_word, size_word):
    if format_word & 4 or (format_word >> 4) & 15 != 2 or format_word >> 28:
        raise ValueError("Cube and volume textures are not supported")
    fmt = (format_word >> 8) & 255
    mips = max(1, (format_word >> 16) & 15)
    if fmt in LINEAR:
        texture = Texture(fmt, (size_word & 4095) + 1,
                          ((size_word >> 12) & 4095) + 1, mips,
                          ((size_word >> 24) + 1) * 64)
    else:
        if size_word:
            raise ValueError("Nonlinear texture has a size field")
        texture = Texture(fmt, 1 << ((format_word >> 20) & 15),
                          1 << ((format_word >> 24) & 15), mips)
    texture.spans()
    return texture


def texture_key(texture, payload, palette=b""):
    if len(payload) != sum(texture.spans()):
        raise ValueError("Mip payload length mismatch")
    if len(palette) != (1024 if texture.format == 11 else 0):
        raise ValueError("P8 requires exactly one 256-entry palette")
    digest = hashlib.sha256(payload + palette).hexdigest()
    return f"{texture.format:02x}-{texture.width}x{texture.height}-m{texture.mips}-{digest}"


def resources(data, relaxed=False):
    if len(data) < 16:
        raise ValueError("Truncated XPR")
    magic, total, header = struct.unpack_from("<4sII", data)
    if not relaxed and (magic != b"XPR0" or total != len(data)):
        raise ValueError("Invalid XPR0 magic or length")
    if not 16 <= header <= len(data):
        raise ValueError("Invalid XPR header span")
    result, position = [], 12
    while position + 4 <= header:
        common = struct.unpack_from("<I", data, position)[0]
        if common == 0xffffffff:
            return header, result
        if common & 0x80000000:
            if position + 8 > header:
                raise ValueError("Truncated XPR user record")
            size = struct.unpack_from("<I", data, position + 4)[0]
            position += 8 + size
            continue
        kind = (common >> 16) & 7
        size = {0: 12, 1: 12, 2: 20, 3: 12, 4: 20, 5: 24, 6: 24}.get(kind)
        if size is None or position + size > header:
            raise ValueError("Invalid XPR resource record")
        words = struct.unpack_from("<" + "I" * (size // 4), data, position)
        if header + words[1] > len(data):
            raise ValueError("XPR resource data outside file")
        result.append((position, words))
        position += size
    raise ValueError("Missing XPR resource terminator")


def texture_payloads(data, relaxed=False):
    header, records = resources(data, relaxed)
    offsets = sorted({words[1] for _, words in records} | {len(data) - header})
    ends = dict(zip(offsets, offsets[1:]))
    for index, (position, words) in enumerate(records):
        if (words[0] >> 16) & 7 != 4:
            continue
        try:
            texture = describe(words[3], words[4])
            start, span = words[1], sum(texture.spans())
            if span > ends.get(start, start) - start:
                raise ValueError("Texture mip span overlaps another resource or EOF")
            palette = b""
            if texture.format == 11:
                # This is a pairing convention, not an XPR binding guarantee.
                # Refuse ambiguous/nonadjacent palettes instead of guessing.
                if index + 1 >= len(records):
                    raise ValueError("No following palette; explicit binding needed")
                pal = records[index + 1][1]
                if (pal[0] >> 16) & 7 != 3 or pal[0] >> 30:
                    raise ValueError("No adjacent 256-entry palette; explicit binding needed")
                if 1024 > ends.get(pal[1], pal[1]) - pal[1]:
                    raise ValueError("Palette span overlaps another resource or EOF")
                palette = data[header + pal[1]:header + pal[1] + 1024]
            yield position, texture, data[header + start:header + start + span], palette, None
        except ValueError as error:
            yield position, None, b"", b"", str(error)


def deswizzle(data, width, height, bpp):
    if (width <= 0 or height <= 0 or width & (width - 1) or height & (height - 1)
            or bpp not in (1, 2, 4) or len(data) != width * height * bpp):
        raise ValueError("Invalid Morton surface")
    xslots, yslots = [], []
    axis, slot = 1, 1
    while axis < max(width, height):
        if axis < width:
            xslots.append(slot)
            slot <<= 1
        if axis < height:
            yslots.append(slot)
            slot <<= 1
        axis <<= 1
    xs = [sum(slot for bit, slot in enumerate(xslots) if x & (1 << bit)) for x in range(width)]
    ys = [sum(slot for bit, slot in enumerate(yslots) if y & (1 << bit)) for y in range(height)]
    result = bytearray(len(data))
    for y, row in enumerate(ys):
        for x, column in enumerate(xs):
            src, dst = (row | column) * bpp, (y * width + x) * bpp
            result[dst:dst + bpp] = data[src:src + bpp]
    return bytes(result)


def rgb565(word):
    r, g, b = word >> 11, (word >> 5) & 63, word & 31
    return ((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2))


def decode_dxt(data, width, height, fmt):
    block = BLOCK[fmt]
    if width <= 0 or height <= 0 or len(data) != ((width + 3) // 4) * ((height + 3) // 4) * block:
        raise ValueError("Invalid DXT payload")
    result = bytearray(width * height * 4)
    offset = 0
    for by in range(0, height, 4):
        for bx in range(0, width, 4):
            chunk = data[offset:offset + block]
            offset += block
            c0, c1, selectors = struct.unpack_from("<HHI", chunk, 0 if fmt == 12 else 8)
            a, b = rgb565(c0), rgb565(c1)
            colors = [(*a, 255), (*b, 255)]
            if c0 > c1 or fmt != 12:
                colors += [(*( (2 * a[i] + b[i]) // 3 for i in range(3)), 255),
                           (*( (a[i] + 2 * b[i]) // 3 for i in range(3)), 255)]
            else:
                colors += [(*( (a[i] + b[i]) // 2 for i in range(3)), 255), (0, 0, 0, 0)]
            alpha = None
            if fmt == 14:
                bits = int.from_bytes(chunk[:8], "little")
                alpha = [((bits >> (i * 4)) & 15) * 17 for i in range(16)]
            elif fmt == 15:
                a0, a1 = chunk[:2]
                table = [a0, a1]
                if a0 > a1:
                    table += [((7 - i) * a0 + i * a1) // 7 for i in range(1, 7)]
                else:
                    table += [((5 - i) * a0 + i * a1) // 5 for i in range(1, 5)] + [0, 255]
                bits = int.from_bytes(chunk[2:8], "little")
                alpha = [table[(bits >> (i * 3)) & 7] for i in range(16)]
            for i in range(16):
                x, y = bx + i % 4, by + i // 4
                if x >= width or y >= height:
                    continue
                pixel = colors[(selectors >> (i * 2)) & 3]
                if alpha is not None:
                    pixel = (*pixel[:3], alpha[i])
                dst = (y * width + x) * 4
                result[dst:dst + 4] = bytes(pixel)
    return bytes(result)


def decode(texture, payload, palette=b""):
    texture_key(texture, payload, palette)
    w, h, fmt = texture.width, texture.height, texture.format
    data = payload[:texture.spans()[0]]
    if fmt in BLOCK:
        return decode_dxt(data, w, h, fmt)
    base = LINEAR.get(fmt, fmt)
    bpp = BPP[base]
    if fmt in LINEAR:
        data = b"".join(data[y * texture.pitch:y * texture.pitch + w * bpp] for y in range(h))
    else:
        data = deswizzle(data, w, h, bpp)
    result = bytearray(w * h * 4)
    for i in range(w * h):
        value = int.from_bytes(data[i * bpp:(i + 1) * bpp], "little")
        if base in (6, 7):
            pixel = ((value >> 16) & 255, (value >> 8) & 255, value & 255,
                     (value >> 24) if base == 6 else 255)
        elif base == 0x3c:
            pixel = (value >> 24, (value >> 16) & 255, (value >> 8) & 255, value & 255)
        elif base == 11:
            b, g, r, a = palette[value * 4:value * 4 + 4]
            pixel = (r, g, b, a)
        elif base == 5:
            pixel = (*rgb565(value), 255)
        elif base in (2, 3):
            components = ((value >> 10) & 31, (value >> 5) & 31, value & 31)
            pixel = (*( (c << 3) | (c >> 2) for c in components),
                     255 if base == 3 or value & 32768 else 0)
        elif base == 4:
            pixel = (((value >> 8) & 15) * 17, ((value >> 4) & 15) * 17,
                     (value & 15) * 17, (value >> 12) * 17)
        elif base == 0x19:
            pixel = (255, 255, 255, value)
        else:
            pixel = (value & 255,) * 3 + ((value >> 8) if base == 0x1a else 255,)
        result[i * 4:i * 4 + 4] = bytes(pixel)
    return bytes(result)


def png(width, height, rgba):
    if len(rgba) != width * height * 4 or width <= 0 or height <= 0:
        raise ValueError("Invalid RGBA surface")

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))

    rows = b"".join(b"\0" + rgba[y * width * 4:(y + 1) * width * 4] for y in range(height))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))


def rip(inputs, output, relaxed=False, raw=False):
    output = private_output(output)
    output.mkdir(parents=True, exist_ok=False)
    seen, manifest, errors = set(), [], []
    files = []
    for source in inputs:
        source = Path(source)
        files.extend(sorted(p for p in source.rglob("*") if p.is_file()) if source.is_dir() else [source])
    for number, path in enumerate(files):
        with path.open("rb") as source:
            prefix = source.read(16)
        candidate = prefix[:4] == b"XPR0" or (relaxed and prefix[12:16] == struct.pack("<I", 0x80000000))
        if not candidate:
            continue
        try:
            data = path.read_bytes()
            for position, texture, payload, palette, error in texture_payloads(data, relaxed):
                if error:
                    errors.append(dict(source=str(path), record=position, error=error))
                    continue
                key = texture_key(texture, payload, palette)
                if key not in seen:
                    pixels = decode(texture, payload, palette)
                    (output / f"{key}.png").write_bytes(png(texture.width, texture.height, pixels))
                    if raw:
                        (output / f"{key}.bin").write_bytes(payload + palette)
                    seen.add(key)
                manifest.append(dict(source=str(path), record=position, key=key,
                                     texture=vars(texture), mip_bytes=len(payload), palette_bytes=len(palette),
                                     palette_binding="adjacent-unverified" if palette else "none"))
        except ValueError as error:
            errors.append(dict(source=str(path), error=str(error)))
        if number % 100 == 0:
            print(f"Scanned {number + 1}/{len(files)} files; {len(seen)} unique textures", flush=True)
    (output / "manifest.json").write_text(json.dumps(dict(textures=manifest, errors=errors), indent=2), encoding="utf-8")
    return len(seen), len(manifest), len(errors)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("inputs", nargs="+", type=Path)
    parser.add_argument("--output", type=Path, default=ROOT / "private/rip/textures")
    parser.add_argument("--relaxed-xpr", action="store_true",
                        help="Accept modified outer magic/length; still validate every resource span")
    parser.add_argument("--raw", action="store_true", help="Also save original mips plus palette as .bin")
    args = parser.parse_args()
    try:
        unique, resources_count, errors = rip(args.inputs, args.output, args.relaxed_xpr, args.raw)
    except (OSError, ValueError) as error:
        print(f"Rip failed: {error}. Partial output is retained.", file=sys.stderr)
        return 1
    print(f"Ripped {unique} unique PNGs from {resources_count} resources; {errors} rejected (see private manifest).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
