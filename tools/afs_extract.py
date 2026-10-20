# SPDX-License-Identifier: GPL-3.0-or-later
"""Extract CRI AFS archives to a new directory beneath this checkout's private/."""
import argparse
from dataclasses import dataclass
from pathlib import Path
import re
import struct
import sys


ROOT = Path(__file__).resolve().parents[1]


def private_output(path):
    path = Path(path).absolute()
    for parent in (path, *path.parents):
        if parent.is_symlink() or getattr(parent, "is_junction", lambda: False)():
            raise ValueError("Output must not traverse a symlink or junction")
    path = path.resolve()
    private = (ROOT / "private").resolve()
    if path == private or not path.is_relative_to(private):
        raise ValueError("Output must be beneath this checkout's private/")
    return path


@dataclass(frozen=True)
class Entry:
    offset: int
    size: int
    name: str = ""


def read_entries(source):
    source.seek(0, 2)
    length = source.tell()
    source.seek(0)
    header = source.read(8)
    if len(header) != 8 or header[:4] != b"AFS\0":
        raise ValueError("Not an AFS archive")
    count = struct.unpack_from("<I", header, 4)[0]
    table_end = 8 + count * 8
    if table_end > length or count > 1_000_000:
        raise ValueError("Invalid AFS entry count")
    entries = [Entry(*pair) for pair in struct.iter_unpack("<II", source.read(count * 8))]
    for entry in entries:
        if entry.offset + entry.size > length or (entry.size and entry.offset < table_end):
            raise ValueError("AFS entry lies outside payload area")
    first = min((entry.offset for entry in entries if entry.size), default=length)
    # Both public AFS variants occur: pointer after the index, or before data.
    for pointer in dict.fromkeys((table_end, first - 8)):
        if pointer < table_end or pointer + 8 > first:
            continue
        source.seek(pointer)
        offset, size = struct.unpack("<II", source.read(8))
        if not offset and not size:
            continue
        if size < count * 48 or offset < table_end or offset + size > length:
            continue
        if any(entry.size and offset < entry.offset + entry.size
               and entry.offset < offset + size for entry in entries):
            continue
        source.seek(offset)
        names = [source.read(48)[:32].split(b"\0", 1)[0].decode("ascii", errors="replace")
                 for _ in entries]
        return [Entry(entry.offset, entry.size, name) for entry, name in zip(entries, names)]
    return entries


def safe_name(name):
    # An index prefix prevents device names, duplicate names and case collisions.
    return re.sub(r"[^A-Za-z0-9_.-]", "_", name).strip(". ")[:80]


def extract(archive, output, names=False):
    output = private_output(output)
    with Path(archive).open("rb") as source:
        entries = read_entries(source)
        output.mkdir(parents=True, exist_ok=False)
        for index, entry in enumerate(entries):
            name = safe_name(entry.name) if names else ""
            target = output / (f"{index:06d}" + (f"_{name}" if name else ""))
            source.seek(entry.offset)
            with target.open("xb") as destination:
                remaining = entry.size
                while remaining:
                    block = source.read(min(remaining, 1024 * 1024))
                    if not block:
                        raise ValueError("Archive truncated during extraction")
                    destination.write(block)
                    remaining -= len(block)
    return len(entries)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive", type=Path)
    parser.add_argument("--output", type=Path, help="New directory under private/")
    parser.add_argument("--names", action="store_true", help="Append sanitized optional AFS names")
    args = parser.parse_args()
    output = args.output or ROOT / "private/rip/afs" / args.archive.stem
    try:
        count = extract(args.archive, output, args.names)
    except (OSError, ValueError) as error:
        print(f"Extraction failed: {error}. Partial output is retained.", file=sys.stderr)
        return 1
    print(f"Extracted {count} entries under private/.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
