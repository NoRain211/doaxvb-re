# SPDX-License-Identifier: GPL-3.0-or-later
"""Sort privately ripped textures using archive, model, runtime and pixel data."""
import argparse
from collections import Counter
import csv
import filecmp
import html
import json
import math
import os
from pathlib import Path, PureWindowsPath
import re
import shutil
import struct
import zlib

from afs_extract import private_output
from model_format import companion_draws, read_mesh
from texture_rip import texture_key, texture_payloads


KEY_PATTERN = re.compile(r"^[0-9a-f]{2}-[0-9]+x[0-9]+-m[0-9]+-[0-9a-f]{64}$")
MATERIAL_SIZE = 0x90
MATERIAL_START = 0xA0
TEXTURE_ORDINAL_OFFSET = 0x70
# Rig slot numbers come from the recovered hierarchy; the region names are the
# anatomical reading of that hierarchy, not recovered strings.
BONE_REGIONS = {
    "head": (1, 17, 19, 21),
    "torso": (0, 2, 16, 20, 23),
    "arms": (4, 8, 10, 14, 24, 25, 26, 27, 30, 31),
    "hands": (5, 11, 28, 29),
    "legs": (3, 6, 7, 9, 12, 13),
    "feet": (18, 22),
}
REGION_OF_BONE = {bone: region for region, bones in BONE_REGIONS.items() for bone in bones}
DRAW_VARIANTS = range(4)
MAX_DESIGN_LINK = 4
ITEM_PART = {"head": "accessories", "torso": "swimsuit", "legs": "swimsuit",
             "arms": "accessories", "hands": "accessories", "feet": "accessories"}
ROLE_WORDS = {
    "skin-face-eyes-hair": {"skin", "face", "eye", "eyes", "hair"},
    "swimsuits": {"swim", "swimsuit", "costume", "outfit", "clothes"},
    "accessories": {"accessory", "accessories", "jewel", "jewelry"},
}
SEMANTIC_WORDS = {
    "foliage": {"foliage", "leaf", "leaves", "tree", "grass", "plant", "flower", "bush"},
    "water-sky": {"water", "sea", "ocean", "sky", "cloud", "wave"},
    "ui-hud-fonts": {"ui", "hud", "font", "menu", "cursor", "button", "gauge", "title", "logo"},
    "items-shop-icons": {"item", "shop", "icon", "catalog", "goods", "treasure"},
    "effects": {"effect", "particle", "spark", "flare", "smoke", "splash"},
    "environments": {"environment", "stage", "beach", "pool", "hotel", "island", "lobby", "room"},
    "outfits": {"swim", "swimsuit", "costume", "outfit", "clothes", "wear", "fashion"},
}
LOCATION_WORDS = {"beach", "pool", "hotel", "island", "lobby", "room", "stage"}


def load_json(path, default):
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        return default


def normalized_path(value):
    return "/".join(PureWindowsPath(str(value)).parts).lower()


def container_path(rip_root, value):
    parts = PureWindowsPath(str(value)).parts
    if len(parts) >= 3 and parts[0].lower() == "private" and parts[1].lower() == "rip":
        return rip_root.joinpath(*parts[2:])
    return None


def tokens(value):
    return set(re.findall(r"[a-z0-9]+", str(value).lower()))


def safe_component(value):
    value = re.sub(r"[^A-Za-z0-9._-]+", "_", str(value)).strip("._-")
    return (value or "unclassified")[:80]


def read_rgba_png(path):
    """Read the unfiltered RGBA PNGs emitted by texture_rip.py."""
    data = path.read_bytes()
    if not data.startswith(b"\x89PNG\r\n\x1a\n"):
        raise ValueError("not a PNG")
    width = height = None
    compressed = bytearray()
    offset = 8
    while offset + 12 <= len(data):
        size = struct.unpack_from(">I", data, offset)[0]
        kind = data[offset + 4:offset + 8]
        start, end = offset + 8, offset + 8 + size
        if end + 4 > len(data):
            raise ValueError("truncated PNG chunk")
        payload = data[start:end]
        expected = struct.unpack_from(">I", data, end)[0]
        if zlib.crc32(kind + payload) != expected:
            raise ValueError("PNG checksum mismatch")
        if kind == b"IHDR":
            if size != 13:
                raise ValueError("invalid PNG header")
            width, height, depth, color, compression, filtering, interlace = struct.unpack(
                ">IIBBBBB", payload
            )
            if (not width or not height or depth != 8 or color != 6
                    or compression or filtering or interlace):
                raise ValueError("expected non-interlaced 8-bit RGBA PNG")
        elif kind == b"IDAT":
            compressed.extend(payload)
        elif kind == b"IEND":
            break
        offset = end + 4
    if width is None or height is None:
        raise ValueError("PNG header missing")
    decoded = zlib.decompress(compressed)
    stride = width * 4
    if len(decoded) != (stride + 1) * height:
        raise ValueError("invalid PNG pixel span")
    pixels = bytearray(width * height * 4)
    for y in range(height):
        row = decoded[y * (stride + 1):(y + 1) * (stride + 1)]
        if row[0] != 0:
            raise ValueError("expected PNG filter type zero")
        pixels[y * stride:(y + 1) * stride] = row[1:]
    return width, height, bytes(pixels)


def sample_pixels(width, height, pixels, side):
    for iy in range(side):
        y = min(height - 1, ((2 * iy + 1) * height) // (2 * side))
        for ix in range(side):
            x = min(width - 1, ((2 * ix + 1) * width) // (2 * side))
            offset = (y * width + x) * 4
            yield pixels[offset:offset + 4]


def perceptual_hash(gray):
    """Return a compact 8x8 DCT hash for a square grayscale sample."""
    if max(gray) == min(gray):
        return 0
    size = 8
    cosine = [[math.cos((2 * x + 1) * u * math.pi / (2 * size))
               for x in range(size)] for u in range(size)]
    rows = [gray[y * size:(y + 1) * size] for y in range(size)]
    horizontal = [[sum(rows[y][x] * cosine[u][x] for x in range(size))
                   for u in range(size)] for y in range(size)]
    coeffs = [[sum(horizontal[y][u] * cosine[v][y] for y in range(size))
               for u in range(size)] for v in range(size)]
    values = [coeffs[y][x] for y in range(size) for x in range(size) if x or y]
    median = sorted(values)[len(values) // 2]
    result = 0
    bit = 0
    for y in range(size):
        for x in range(size):
            if x or y:
                if coeffs[y][x] > median:
                    result |= 1 << bit
                bit += 1
    return result


def image_features(path):
    width, height, pixels = read_rgba_png(path)
    sample = list(sample_pixels(width, height, pixels, 64))
    visible = [rgba for rgba in sample if rgba[3]]
    if not visible:
        visible = [(0, 0, 0, 255)]
    total = max(1, len(sample))
    transparent = sum(1 for rgba in sample if rgba[3] == 0)
    partial = sum(1 for rgba in sample if 0 < rgba[3] < 255)
    average = tuple(sum(pixel[channel] for pixel in visible) // len(visible)
                    for channel in range(3))
    palette = len({(r >> 4, g >> 4, b >> 4) for r, g, b, _ in visible})
    gray = []
    for rgba in sample_pixels(width, height, pixels, 8):
        r, g, b, alpha = rgba
        r = (r * alpha + 127 * (255 - alpha)) // 255
        g = (g * alpha + 127 * (255 - alpha)) // 255
        b = (b * alpha + 127 * (255 - alpha)) // 255
        gray.append((299 * r + 587 * g + 114 * b) / 1000)
    maximum, minimum = max(average), min(average)
    saturation = (maximum - minimum) / max(1, maximum)
    visual = ""
    if saturation >= 0.16 and average[1] > average[0] * 1.12 and average[1] > average[2] * 1.03:
        visual = "green-dominant"
    elif saturation >= 0.16 and average[2] > average[0] * 1.10 and average[2] > average[1] * 1.02:
        visual = "blue-dominant"
    return {
        "width": width,
        "height": height,
        "alpha_transparent": transparent / total,
        "alpha_partial": partial / total,
        "palette_bins": palette,
        "average_rgb": "#%02x%02x%02x" % average,
        "visual_hint": visual,
        "phash": perceptual_hash(gray),
    }


def layout_groups(records):
    """Cluster close hashes at matching dimensions; skip featureless flat fills."""
    result = {}
    by_shape = {}
    for record in records:
        visual = record["visual"]
        if visual.get("palette_bins", 0) <= 1:
            result[record["key"]] = ("layout-" + record["key"][-4:], 1)
        else:
            shape = (visual.get("width"), visual.get("height"))
            by_shape.setdefault(shape, []).append(record)
    for shape_records in by_shape.values():
        keys_by_hash = {}
        for record in shape_records:
            keys_by_hash.setdefault(record["visual"]["phash"], []).append(record["key"])
        hashes = list(keys_by_hash)
        parents = list(range(len(hashes)))

        def find(index):
            while parents[index] != index:
                parents[index] = parents[parents[index]]
                index = parents[index]
            return index

        def join(left, right):
            a, b = find(left), find(right)
            if a != b:
                parents[b] = a

        buckets = {}
        for index, value in enumerate(hashes):
            for band in range(4):
                buckets.setdefault((band, (value >> (band * 16)) & 0xffff), []).append(index)
        checked = set()
        for members in buckets.values():
            for pos, left in enumerate(members):
                for right in members[pos + 1:]:
                    pair = (left, right)
                    if pair in checked:
                        continue
                    checked.add(pair)
                    if (hashes[left] ^ hashes[right]).bit_count() <= 3:
                        join(left, right)
        component_hashes = {}
        for index, value in enumerate(hashes):
            component_hashes.setdefault(find(index), []).append(value)
        for cluster in component_hashes.values():
            members = [key for value in cluster for key in keys_by_hash[value]]
            label = "layout-%04x" % (min(cluster) & 0xffff)
            for key in members:
                result[key] = (label, len(members))
    return result


def source_label(path, metadata):
    item = metadata.get(normalized_path(path), {})
    if not item:
        parts = PureWindowsPath(str(path)).parts
        return "/".join(parts[-2:]) if parts else "unknown container"
    archive = item.get("archive", "archive")
    index = item.get("index")
    name = item.get("name")
    entry = "#%06d" % index if isinstance(index, int) else ""
    label = f"{archive}{entry}"
    return f"{label} ({name})" if name else label


def frame_membership(frame_roots):
    result = {}
    for root in frame_roots:
        root = Path(root)
        directories = sorted(root.glob("frame-*")) if root.is_dir() else []
        if root.is_dir() and root.name.startswith("frame-"):
            directories.append(root)
        for directory in directories:
            for image in directory.glob("*.png"):
                key = image.stem
                if KEY_PATTERN.fullmatch(key):
                    result.setdefault(key, set()).add(directory.name)
    return result


def discover_character_tokens(asset_metadata):
    extensions_by_stem = {}
    for item in asset_metadata.values():
        name = Path(item.get("name", "")).stem.lower()
        name = re.sub(r"^m_", "", name)
        ext = item.get("extension", "").lower()
        extensions_by_stem.setdefault(name, set()).add(ext)
    return {stem for stem, extensions in extensions_by_stem.items()
            if stem and ".bin" in extensions and ".mot" in extensions}


def discover_character_aliases(character_names):
    aliases = {}
    for name in character_names:
        consonants = [letter for letter in name if letter.isalpha() and letter not in "aeiou"]
        candidates = {name[:2]}
        if len(consonants) >= 2:
            candidates.add("".join(consonants[:2]))
        for candidate in candidates:
            aliases.setdefault(candidate, set()).add(name)
    return {alias: next(iter(names)) for alias, names in aliases.items() if len(names) == 1}


def model_references(rip_root, manifest_textures, model_objects, metadata):
    rows_by_path = {}
    for texture in manifest_textures:
        rows_by_path.setdefault(normalized_path(texture.get("source", "")), []).append(texture)
    for rows in rows_by_path.values():
        rows.sort(key=lambda row: row.get("record", 0))
    objects_by_path = {}
    for obj in model_objects:
        objects_by_path.setdefault(normalized_path(obj.get("path", "")), []).append(obj)
    references = {}
    missing = 0
    for path_key, objects in objects_by_path.items():
        texture_rows = rows_by_path.get(path_key, [])
        if not texture_rows:
            continue
        path = container_path(rip_root, objects[0].get("path", ""))
        if path is None or not path.is_file():
            missing += 1
            continue
        data = path.read_bytes()
        label = source_label(objects[0].get("path", ""), metadata)
        for obj in objects:
            position = int(obj.get("obj", -1)) + MATERIAL_START
            count = 0
            while position + MATERIAL_SIZE <= len(data) and count < 256:
                marker, size = struct.unpack_from("<II", data, position)
                if not marker & 0x80000000 or size != MATERIAL_SIZE:
                    break
                ordinal = struct.unpack_from("<I", data, position + TEXTURE_ORDINAL_OFFSET)[0]
                if ordinal < len(texture_rows):
                    key = texture_rows[ordinal].get("key")
                    references.setdefault(key, set()).add(f"{label} OBJ@0x{int(obj['obj']):x}")
                position += size
                count += 1
    return references, missing


def draw_list(companion):
    """Union the primary draw lists of the first few companion variants."""
    seen, draws = set(), []
    for variant in DRAW_VARIANTS:
        try:
            found, _ = companion_draws(companion, variant)
        except (ValueError, struct.error):
            break
        for draw in found:
            identity = (draw["object"], draw.get("palette"), draw.get("bone"))
            if identity not in seen:
                seen.add(identity)
                draws.append(draw)
    return draws


def object_textures(model, obj):
    """Texture ordinals and vertex count for objects the strict mesh reader refuses."""
    count, textures = struct.unpack_from("<II", model, 24)
    if not 0 <= obj < count:
        return [], 0
    offset = struct.unpack_from("<I", model, 40 + obj * 4)[0]
    vertices = struct.unpack_from("<I", model, offset + 32)[0]
    ordinals, position = [], offset + MATERIAL_START
    while position + MATERIAL_SIZE <= len(model) and len(ordinals) < 256:
        marker, size = struct.unpack_from("<II", model, position)
        if not marker & 0x80000000 or size != MATERIAL_SIZE:
            break
        ordinal = struct.unpack_from("<I", model, position + TEXTURE_ORDINAL_OFFSET)[0]
        if ordinal < textures:
            ordinals.append(ordinal)
        position += size
    return ordinals, vertices


def texture_bone_mass(model, companion, palettes, ordinal_keys):
    """Sum each texture's vertex weights per body region over the drawn objects.

    Returns ({key: Counter(region -> mass)}, keys that needed the even-weight fallback).
    """
    mass, approximate = {}, set()
    for draw in draw_list(companion):
        if "bone" in draw:
            bones = [draw["bone"]]
        elif draw["palette"] < len(palettes):
            bones = [entry[0] for entry in palettes[draw["palette"]]]
        else:
            continue
        try:
            mesh = read_mesh(model, draw["object"])
        except (ValueError, struct.error, IndexError):
            try:
                ordinals, vertices = object_textures(model, draw["object"])
            except struct.error:
                continue
            for ordinal in ordinals:
                key = ordinal_keys.get(ordinal)
                if key is None:
                    continue
                approximate.add(key)
                counter = mass.setdefault(key, Counter())
                for bone in bones:
                    counter[REGION_OF_BONE.get(bone, "other")] += vertices / len(bones)
            continue
        for ordinal, faces in mesh.subsets:
            key = ordinal_keys.get(ordinal)
            if key is None:
                continue
            counter = mass.setdefault(key, Counter())
            for vertex in faces:
                weights = mesh.weights[vertex]
                for slot, bone in enumerate(bones[:len(weights)]):
                    counter[REGION_OF_BONE.get(bone, "other")] += weights[slot]
    return mass, approximate


def bundle_code(stem):
    """Two-letter wearer/body code: suffix after an item number or separator, or before a body number."""
    match = (re.fullmatch(r"[a-z0-9_]*[0-9_]([a-z]{2})", stem)
             or re.fullmatch(r"[a-z]*([a-z]{2})[0-9]+", stem))
    return match.group(1) if match else ""


def bone_evidence(rip_root, asset_metadata):
    """Map texture keys to per-bundle body-region mass from companion draw palettes."""
    rig = load_json(rip_root / "model-rig.json", {})
    palettes = rig.get("palettes", [])
    bundles = {}
    for path, info in asset_metadata.items():
        name = Path(info.get("name", ""))
        suffix = name.suffix.lower()
        if suffix in (".xpr", ".cat"):
            location = container_path(rip_root, path)
            if location is not None:
                bundles.setdefault((info.get("archive", ""), name.stem.lower()), {})[suffix] = (location, path)
    evidence = {}
    for (archive, stem), files in sorted(bundles.items()):
        if ".xpr" not in files or ".cat" not in files:
            continue
        try:
            model = files[".xpr"][0].read_bytes()
            companion = files[".cat"][0].read_bytes()
        except OSError:
            continue
        ordinal_keys = {}
        try:
            for ordinal, (_, texture, payload, palette, error) in enumerate(texture_payloads(model, True)):
                if not error:
                    ordinal_keys[ordinal] = texture_key(texture, payload, palette)
        except ValueError:
            continue
        mass, approximate = texture_bone_mass(model, companion, palettes, ordinal_keys)
        regions = Counter()
        for counter in mass.values():
            regions.update(counter)
        body = regions["head"] > 0 and regions["feet"] > 0 and regions["torso"] > 0
        label = source_label(files[".xpr"][1], asset_metadata)
        for key, counter in mass.items():
            total = sum(counter.values())
            if total <= 0:
                continue
            evidence.setdefault(key, []).append({
                "label": label, "stem": stem, "code": bundle_code(stem), "body": body,
                "fractions": {region: value / total for region, value in counter.items()},
                "approximate": key in approximate,
            })
    return evidence


def skin_tone(rgb):
    r, g, b = rgb
    return r >= 150 and r > g > b and r - b >= 25


def average_rgb(visual):
    value = visual.get("average_rgb", "")
    if not re.fullmatch(r"#[0-9a-f]{6}", value):
        return (0, 0, 0)
    return tuple(int(value[i:i + 2], 16) for i in (1, 3, 5))


def head_part(visual):
    """Split head textures: alpha cards are hair, skin tones are face, the rest eyes or mouth."""
    if visual.get("alpha_transparent", 0) + visual.get("alpha_partial", 0) >= 0.05:
        return "hair", "high", "head texture with alpha coverage"
    rgb = average_rgb(visual)
    largest = max(visual.get("width", 0), visual.get("height", 0))
    if largest >= 256:
        return "face", "high", "large opaque head texture"
    if skin_tone(rgb) and rgb[0] >= 190:
        return "face", "high", "skin-toned head texture"
    r, g, b = rgb
    if r > 1.4 * b and r > g and r < 190 and largest <= 128:
        return "mouth", "medium", "dark red opaque head texture (mouth or teeth guess)"
    return "eyes", "medium", "opaque non-skin head texture (eye guess)"


def bone_summary(entries):
    """Average region fractions over bundles so repeated copies do not dominate."""
    totals = Counter()
    for entry in entries:
        totals.update(entry["fractions"])
    count = max(1, len(entries))
    fractions = {region: value / count for region, value in totals.items()}
    region, share = max(fractions.items(), key=lambda item: item[1])
    return region, share, fractions


def classify_by_bones(record, entries, girl_names, body_codes):
    """Classify a texture from its draw-palette bone evidence; return False if unusable."""
    region, share, fractions = bone_summary(entries)
    approximate = any(entry["approximate"] for entry in entries)
    bodies = [entry for entry in entries if entry["body"]]
    wearables = [entry for entry in entries if not entry["body"] and entry["code"] in body_codes]
    mix = ", ".join(f"{name} {value:.0%}" for name, value in sorted(
        fractions.items(), key=lambda item: -item[1])[:3])
    basis = [f"draw-palette bone mass: {mix}"]
    if approximate:
        basis.append("some objects use even palette weights (strict mesh reader refused them)")
    confidence = "high" if share >= 0.75 and not approximate else "medium"
    if bodies:
        codes = sorted({entry["code"] for entry in bodies})
        if region == "head":
            part, part_confidence, reason = head_part(record["visual"])
            basis.append(reason)
            if part_confidence == "medium":
                confidence = "medium"
        else:
            part = "skin"
            basis.append(f"primary body bundle, {region} region")
        girl = girl_names.get(codes[0], codes[0]) if len(codes) == 1 else "shared"
        basis.append("body bundle codes " + ", ".join(codes))
        record["category"] = f"characters/{safe_component(girl)}/{part}"
    elif wearables:
        part = ITEM_PART.get(region, "accessories")
        codes = sorted({entry["code"] for entry in wearables})
        series = sorted({entry["stem"][:-2] for entry in wearables})
        basis.append(f"wearable item for {', '.join(codes)}; {region} region")
        if len(codes) == 1:
            girl = girl_names.get(codes[0], codes[0])
            record["category"] = f"characters/{safe_component(girl)}/{part}"
        else:
            record["outfit_series"] = series
            record["outfit_part"] = part
            record["category"] = f"outfits/{safe_component(series[0])}/{part}"
    elif region != "other":
        stem = sorted(entry["stem"] for entry in entries)[0]
        record["category"] = f"items-shop-icons/held-{safe_component(stem)}"
        basis.append("drawn on the character rig without a wearer code")
        confidence = "medium"
    else:
        return False
    record["confidence"] = confidence
    record["notes"] = "; ".join(basis)
    return True


def classify_by_bundle(record, girl_names, body_codes, body_stems):
    """Place undrawn textures of body or wearable bundles by bundle identity alone."""
    stems = {Path(source["metadata"]["name"]).stem.lower()
             for source in record["sources"] if source["metadata"].get("name")}
    bodies = sorted({bundle_code(stem) for stem in stems & body_stems})
    worn = sorted(stem for stem in stems - body_stems if bundle_code(stem) in body_codes)
    reason = "no primary draw pass references this texture; part unknown"
    if bodies:
        girl = girl_names.get(bodies[0], bodies[0]) if len(bodies) == 1 else "shared"
        record["category"] = f"characters/{safe_component(girl)}/unsorted"
        record["notes"] = f"{reason}; body bundle codes {', '.join(bodies)}"
    elif worn:
        codes = sorted({bundle_code(stem) for stem in worn})
        if len(codes) == 1:
            girl = girl_names.get(codes[0], codes[0])
            record["category"] = f"characters/{safe_component(girl)}/unsorted"
        else:
            record["outfit_series"] = sorted({stem[:-2] for stem in worn})
            record["outfit_part"] = "unsorted"
            record["category"] = f"outfits/{safe_component(record['outfit_series'][0])}/unsorted"
        record["notes"] = f"{reason}; wearable item bundle for {', '.join(codes)}"
    else:
        return False
    record["confidence"] = "low"
    return True


def assign_outfit_designs(records):
    """Group outfit series into designs: colorways of one design share exact texture keys.

    Keys shared by many series are common trims or swatches and do not join designs.
    """
    outfits = [record for record in records if record.get("outfit_series")]
    parents = {}

    def find(name):
        parents.setdefault(name, name)
        while parents[name] != name:
            parents[name] = parents[parents[name]]
            name = parents[name]
        return name

    def join(left, right):
        a, b = sorted((find(left), find(right)))
        parents[b] = a

    for record in outfits:
        series = record["outfit_series"]
        find(series[0])
        if len(series) <= MAX_DESIGN_LINK:
            for other in series[1:]:
                join(series[0], other)
    for record in outfits:
        series = record["outfit_series"]
        design = find(series[0])
        colorway = series[0] if len(series) == 1 else "shared"
        record["category"] = (f"outfits/{safe_component(design)}/{safe_component(colorway)}/"
                              f"{record['outfit_part']}")
        if len(series) > 1:
            record["notes"] += "; shared by colorways " + ", ".join(series)


def classify_record(record, character_names):
    entries = record.get("bone_entries")
    if entries and classify_by_bones(record, entries, record["girl_names"], record["body_codes"]):
        layout, layout_size = record.get("layout", ("", 1))
        if layout_size > 1:
            record["notes"] += f"; layout group {layout} ({layout_size} textures)"
        return
    if record.get("body_codes") and classify_by_bundle(
            record, record["girl_names"], record["body_codes"], record["body_stems"]):
        return
    source_tokens = set()
    source_names = []
    model_labels = {reference.rsplit(" OBJ@", 1)[0] for reference in record["model_objects"]}
    model_sources = [source for source in record["sources"] if source["label"] in model_labels]
    evidence_sources = model_sources or record["sources"]
    for source in evidence_sources:
        info = source["metadata"]
        if info.get("name"):
            source_names.append(info["name"])
            source_tokens |= tokens(Path(info["name"]).stem)
    direct_characters = sorted(name for name in character_names if name in source_tokens)
    alias_map = record["character_aliases"]
    abbreviated_characters = []
    for name in source_names:
        stem = Path(name).stem.lower()
        suffix = re.search(r"(?:_|[0-9])([a-z]{2})$", stem)
        if suffix and suffix.group(1) in alias_map:
            abbreviated_characters.append((alias_map[suffix.group(1)], suffix.group(1)))
    characters = sorted(set(direct_characters) | {name for name, _ in abbreviated_characters})
    visual = record.get("visual", {})
    width, height = visual.get("width", 0), visual.get("height", 0)
    aspect = max(width, height) / max(1, min(width, height))
    natural = (visual.get("alpha_transparent", 0) < 0.05 and aspect <= 2
               and min(width, height) >= 64 and visual.get("palette_bins", 0) >= 24)
    layout, layout_size = record.get("layout", ("layout-unknown", 1))
    layout_note = f"layout group {layout} ({layout_size} textures)" if layout_size > 1 else ""
    basis = []
    confidence = "low"
    if characters:
        if len(characters) > 1:
            category = "characters/shared/model-texture-atlas"
            confidence = "medium" if len(direct_characters) > 1 else "low"
            basis.append("same texture key is referenced by multiple character-coded model containers")
            if not direct_characters:
                basis.append("character suffix mappings are inferred from paired archive stems")
        else:
            character = characters[0]
            role = next((name for name, words in ROLE_WORDS.items() if source_tokens & words),
                        "unsorted")
            category = f"characters/{safe_component(character)}/{role}"
            if direct_characters:
                confidence = "medium" if role == "unsorted" else "high"
                basis.append(f"paired character archive stem {character}")
            else:
                codes = sorted({code for name, code in abbreviated_characters if name == character})
                confidence = "low"
                basis.append(f"unverified character suffix {', '.join(codes)} matches paired archive stem {character}")
    else:
        semantic = next(((category, words) for category, words in SEMANTIC_WORDS.items()
                         if source_tokens & words), None)
        if semantic:
            category, words = semantic
            matched = sorted(source_tokens & words)
            if category == "environments":
                subcategory = next(iter(sorted(source_tokens & LOCATION_WORDS)), "location-unspecified")
            elif category == "outfits":
                subcategory = safe_component(Path(source_names[0]).stem) if source_names else "catalog"
            elif category == "ui-hud-fonts":
                subcategory = "fonts" if "font" in matched else "hud-and-ui"
            else:
                subcategory = "textures"
            category = f"{category}/{safe_component(subcategory)}"
            confidence = "medium"
            basis.append("container filename token " + ", ".join(matched))
        elif any(Path(name).stem.lower().startswith(("stg", "fld")) for name in source_names):
            source_name = next(Path(name).stem for name in source_names
                               if Path(name).stem.lower().startswith(("stg", "fld")))
            category = f"environments/{safe_component(source_name)}"
            confidence = "medium"
            prefix = "stage" if source_name.lower().startswith("stg") else "field"
            basis.append(f"{prefix} bundle filename prefix")
        elif record["model_objects"]:
            source_name = Path(source_names[0]).stem if source_names else "model-bundle"
            if record.get("bundle_objects", 0) <= 8:
                category = f"items-shop-icons/models-{safe_component(source_name)}"
                basis.append("small model bundle without character rig evidence; item role is inferred")
            else:
                category = f"environments/{safe_component(source_name)}"
                basis.append("large model bundle without character rig evidence; set or prop role is inferred")
        elif visual.get("alpha_transparent", 0) > 0.15 and visual.get("palette_bins", 0) <= 24:
            category = "effects/alpha-sprites"
            basis.append("transparent, limited-palette image heuristic")
        elif aspect >= 4 and min(width, height) <= 32:
            category = "ui-hud-fonts/strips-and-gradients"
            basis.append("strip aspect ratio heuristic")
        elif visual.get("width", 0) <= 64 and visual.get("height", 0) <= 64:
            category = "ui-hud-fonts/small-textures"
            basis.append("small dimensions heuristic")
        elif visual.get("visual_hint") == "green-dominant" and natural:
            category = "foliage/green-dominant"
            basis.append("average-color heuristic on an opaque, varied texture")
        elif visual.get("visual_hint") == "blue-dominant" and natural:
            category = "water-sky/blue-dominant"
            basis.append("average-color heuristic on an opaque, varied texture")
        elif visual.get("alpha_transparent", 0) > 0.15:
            category = "effects/alpha-decals"
            basis.append("transparent decal or sprite heuristic")
        else:
            category = "other/unclassified"
            basis.append("no identifying container token or model reference")
        if layout_note:
            basis.append(layout_note)
    if confidence == "low":
        basis.insert(0, "LOW-CONFIDENCE GUESS")
    record["category"] = category
    record["confidence"] = confidence
    record["notes"] = "; ".join(basis)


def link_texture(source, destination):
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.exists():
        if os.path.samefile(source, destination):
            return "hard link"
        raise FileExistsError(f"refusing to replace existing output: {destination}")
    try:
        os.link(source, destination)
        return "hard link"
    except OSError:
        shutil.copy2(source, destination)
        return "copy"


def remove_previous_links(output, texture_root):
    """Remove earlier sorted links or fallback copies of ripped textures, then prune emptied folders."""
    for destination in sorted(output.rglob("*.png")):
        source = texture_root / destination.name
        if (KEY_PATTERN.fullmatch(destination.stem) and source.is_file()
                and (os.path.samefile(source, destination)
                     or filecmp.cmp(source, destination, shallow=False))):
            destination.unlink()
    for folder in sorted((path for path in output.rglob("*") if path.is_dir()),
                         key=lambda path: len(path.parts), reverse=True):
        if not any(folder.iterdir()):
            folder.rmdir()


def build_gallery(records, output, texture_root):
    categories = sorted({record["category"] for record in records})
    options = "".join(f'<option value="{html.escape(category, quote=True)}">{html.escape(category)}</option>'
                      for category in categories)
    cards = []
    for record in records:
        key = record["key"]
        image = os.path.relpath(texture_root / f"{key}.png", output).replace(os.sep, "/")
        source = record["source_container"]
        refs = ", ".join(record["model_objects"][:8])
        if len(record["model_objects"]) > 8:
            refs += f" … +{len(record['model_objects']) - 8} more"
        frames = ", ".join(record["runtime_frames"])
        xpr_records = "; ".join(record["xpr_records"][:6])
        if len(record["xpr_records"]) > 6:
            xpr_records += f" … +{len(record['xpr_records']) - 6} more"
        search = " ".join((key, record["category"], source, xpr_records,
                            record["notes"], refs, frames))
        cards.append(
            f'<article class="card" data-category="{html.escape(record["category"], quote=True)}" '
            f'data-confidence="{record["confidence"]}" data-search="{html.escape(search.lower(), quote=True)}">'
            f'<img loading="lazy" src="{html.escape(image, quote=True)}" alt="Texture {key}">'
            f'<div class="body"><b>{html.escape(key)}</b>'
            f'<div>{html.escape(record["category"])} · {record["confidence"]} confidence</div>'
            f'<div>{html.escape(record["size"])}</div><small>{html.escape(source)}</small>'
            f'{"<small>XPR records: " + html.escape(xpr_records) + "</small>" if xpr_records else ""}'
            f'<small>{html.escape(record["notes"])}</small>'
            f'{"<small>Meshes: " + html.escape(refs) + "</small>" if refs else ""}'
            f'{"<small>Frames: " + html.escape(frames) + "</small>" if frames else ""}'
            f'</div></article>'
        )
    document = """<!doctype html>
<html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width">
<title>Texture sort index</title><style>
body{font:14px system-ui,sans-serif;margin:0;background:#111827;color:#e5e7eb}header{position:sticky;top:0;background:#111827;padding:14px;z-index:2;border-bottom:1px solid #374151}
input,select{font:inherit;padding:8px;margin:4px;background:#1f2937;color:inherit;border:1px solid #4b5563;border-radius:5px}.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(230px,1fr));gap:12px;padding:14px}.card{background:#1f2937;border:1px solid #374151;border-radius:7px;overflow:hidden;min-width:0}.card img{width:100%;height:190px;object-fit:contain;background:#0b1020}.body{padding:9px;overflow-wrap:anywhere}.body div,.body small{display:block;margin-top:5px;color:#cbd5e1}.body small{font-size:11px}.hidden{display:none}
</style><header><b>Texture index</b> <span id="count"></span><br>
<input id="search" type="search" placeholder="Search key, container, mesh, notes">
<select id="category"><option value="">All categories</option>""" + options + """</select>
<select id="confidence"><option value="">All confidence levels</option><option>high</option><option>medium</option><option>low</option></select>
</header><main class="grid">""" + "".join(cards) + """</main><script>
const cards=[...document.querySelectorAll('.card')],search=document.querySelector('#search'),category=document.querySelector('#category'),confidence=document.querySelector('#confidence'),count=document.querySelector('#count');
function filter(){let shown=0;for(const card of cards){const ok=(!category.value||card.dataset.category===category.value)&&(!confidence.value||card.dataset.confidence===confidence.value)&&card.dataset.search.includes(search.value.toLowerCase());card.classList.toggle('hidden',!ok);if(ok)shown++}count.textContent=`${shown} / ${cards.length} textures`}
search.addEventListener('input',filter);category.addEventListener('change',filter);confidence.addEventListener('change',filter);filter();
</script></html>
"""
    (output / "index.html").write_text(document, encoding="utf-8", newline="\n")


def sort_textures(rip_root, output=None, frame_roots=()):
    rip_root = Path(rip_root).resolve()
    output = private_output(output or rip_root / "sorted")
    texture_root = rip_root / "textures"
    # Re-runs unlink sorted PNGs that match a ripped source, so the two must never overlap.
    if texture_root.is_relative_to(output) or output.is_relative_to(texture_root):
        raise ValueError("sorted output must not contain or sit inside the ripped textures")
    manifest = load_json(texture_root / "manifest.json", {})
    manifest_textures = manifest.get("textures", [])
    asset_metadata = {
        normalized_path(path): item
        for path, item in load_json(rip_root / "asset-metadata.json", {}).items()
    }
    archive_keys = load_json(rip_root / "archive-keys.json", {})
    model_objects = load_json(rip_root / "model-objects.json", [])
    frames = frame_membership(frame_roots)
    references, missing_model_files = model_references(
        rip_root, manifest_textures, model_objects, asset_metadata
    )

    by_key = {}
    for texture in manifest_textures:
        key = texture.get("key", "")
        if not KEY_PATTERN.fullmatch(key):
            continue
        item = by_key.setdefault(key, {
            "key": key, "texture": texture.get("texture", {}), "sources": [],
        })
        path = texture.get("source", "")
        pair = (normalized_path(path), texture.get("record", 0))
        if not any((normalized_path(source["path"]), source["record"]) == pair
                   for source in item["sources"]):
            item["sources"].append({
                "path": path,
                "record": texture.get("record", 0),
                "label": source_label(path, asset_metadata),
                "metadata": asset_metadata.get(normalized_path(path), {}),
            })
    for key, sources in archive_keys.items():
        if key not in by_key or not KEY_PATTERN.fullmatch(key):
            continue
        known = {(normalized_path(source["path"]), source["record"])
                 for source in by_key[key]["sources"]}
        for source in sources:
            pair = (normalized_path(source.get("path", "")), source.get("record", 0))
            if pair not in known:
                by_key[key]["sources"].append({
                    "path": source.get("path", ""), "record": source.get("record", 0),
                    "label": source_label(source.get("path", ""), asset_metadata),
                    "metadata": asset_metadata.get(pair[0], {}),
                })
    records = []
    for key, item in sorted(by_key.items()):
        image_path = texture_root / f"{key}.png"
        if not image_path.is_file():
            continue
        try:
            item["visual"] = image_features(image_path)
        except (OSError, ValueError, zlib.error) as error:
            item["visual"] = {"width": 0, "height": 0, "phash": 0,
                               "visual_hint": "", "alpha_transparent": 0,
                               "alpha_partial": 0, "palette_bins": 0,
                               "average_rgb": "unknown"}
            item["visual_error"] = str(error)
        item["model_objects"] = sorted(references.get(key, ()))
        item["runtime_frames"] = sorted(frames.get(key, ()))
        item["source_container"] = "; ".join(sorted({
            source_label(source["path"], asset_metadata) for source in item["sources"]
        })) or "unknown"
        item["xpr_records"] = sorted({
            f"{source_label(source['path'], asset_metadata)} record@0x{int(source['record']):x}"
            for source in item["sources"]
        })
        dimensions = item["texture"]
        fmt = dimensions.get("format")
        item["size"] = (f"{dimensions.get('width', item['visual']['width'])}x"
                         f"{dimensions.get('height', item['visual']['height'])}; "
                         f"format 0x{fmt:02x}" if isinstance(fmt, int) else
                         f"{item['visual']['width']}x{item['visual']['height']}")
        records.append(item)
    groups = layout_groups(records)
    for record in records:
        record["layout"] = groups.get(record["key"], ("layout-unknown", 1))
    classify_names = discover_character_tokens(asset_metadata)
    character_aliases = discover_character_aliases(classify_names)
    evidence = bone_evidence(rip_root, asset_metadata)
    body_codes = {entry["code"] for entries in evidence.values()
                  for entry in entries if entry["body"] and entry["code"]}
    girl_names = {code: character_aliases[code] for code in body_codes if code in character_aliases}
    body_stems = {entry["stem"] for entries in evidence.values() for entry in entries if entry["body"]}
    object_counts = Counter(normalized_path(obj.get("path", "")) for obj in model_objects)
    for record in records:
        record["character_aliases"] = character_aliases
        record["bone_entries"] = evidence.get(record["key"], [])
        record["girl_names"] = girl_names
        record["body_codes"] = body_codes
        record["body_stems"] = body_stems
        record["bundle_objects"] = max((object_counts[normalized_path(source["path"])]
                                        for source in record["sources"]), default=0)
        classify_record(record, classify_names)
    assign_outfit_designs(records)
    for record in records:
        if record["confidence"] == "low" and not record["notes"].startswith("LOW-CONFIDENCE"):
            record["notes"] = "LOW-CONFIDENCE GUESS; " + record["notes"]
        visual = record["visual"]
        details = [
            f"alpha transparent {visual.get('alpha_transparent', 0):.0%}",
            f"partial {visual.get('alpha_partial', 0):.0%}",
            f"palette bins {visual.get('palette_bins', 0)}",
            f"average {visual.get('average_rgb', 'unknown')}",
            f"pHash {visual.get('phash', 0):016x}",
        ]
        if record.get("visual_error"):
            details.append("visual decode failed: " + record["visual_error"])
        if record["runtime_frames"]:
            details.append("runtime frames: " + ", ".join(record["runtime_frames"]))
        if record["model_objects"]:
            details.append(f"mesh references: {len(record['model_objects'])}")
        record["notes"] += "; " + "; ".join(details)

    output.mkdir(parents=True, exist_ok=True)
    remove_previous_links(output, texture_root)
    for record in records:
        destination = output.joinpath(*(safe_component(part) for part in record["category"].split("/")),
                                      f"{record['key']}.png")
        record["link_kind"] = link_texture(texture_root / f"{record['key']}.png", destination)
    fields = ["key", "category", "confidence", "source container", "size", "notes",
              "XPR records", "model objects", "runtime frames"]
    with (output / "index.csv").open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields, lineterminator="\n")
        writer.writeheader()
        for record in records:
            writer.writerow({
                "key": record["key"], "category": record["category"],
                "confidence": record["confidence"], "source container": record["source_container"],
                "size": record["size"], "notes": record["notes"],
                "XPR records": "; ".join(record["xpr_records"]),
                "model objects": "; ".join(record["model_objects"]),
                "runtime frames": "; ".join(record["runtime_frames"]),
            })
    build_gallery(records, output, texture_root)
    return records, missing_model_files


def default_frames(project_root):
    frames = project_root / "private" / "textures"
    return [frames] if frames.is_dir() else []


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rip-root", type=Path,
                        default=Path(__file__).resolve().parents[1] / "private" / "rip")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--frames", type=Path, action="append",
                        help="Texture frame-dump directory; repeat to include multiple roots")
    args = parser.parse_args(argv)
    rip_root = args.rip_root.resolve()
    frame_roots = args.frames if args.frames is not None else default_frames(rip_root.parent.parent)
    records, missing = sort_textures(rip_root, args.output, frame_roots)
    category_counts = {}
    confidence_counts = {}
    links = {}
    for record in records:
        category = record["category"].split("/", 1)[0]
        category_counts[category] = category_counts.get(category, 0) + 1
        confidence_counts[record["confidence"]] = confidence_counts.get(record["confidence"], 0) + 1
        links[record["link_kind"]] = links.get(record["link_kind"], 0) + 1
    print(f"Sorted {len(records)} unique textures into {args.output or rip_root / 'sorted'}")
    print("Categories: " + ", ".join(f"{name}={count}" for name, count in sorted(category_counts.items())))
    print("Confidence: " + ", ".join(f"{name}={count}" for name, count in sorted(confidence_counts.items())))
    print("File placement: " + ", ".join(f"{name}={count}" for name, count in sorted(links.items())))
    if missing:
        print(f"Model containers missing from private extraction: {missing}")
    print("Open index.html in a browser; filter by category/confidence or search notes and mesh references.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
