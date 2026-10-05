# SPDX-License-Identifier: GPL-3.0-or-later
"""Sort privately ripped textures using archive, model, runtime and pixel data."""
import argparse
import csv
import html
import json
import math
import os
from pathlib import Path, PureWindowsPath
import re
import shutil
import struct
import zlib


KEY_PATTERN = re.compile(r"^[0-9a-f]{2}-[0-9]+x[0-9]+-m[0-9]+-[0-9a-f]{64}$")
MATERIAL_SIZE = 0x90
MATERIAL_START = 0xA0
TEXTURE_ORDINAL_OFFSET = 0x70
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


def classify_record(record, character_names):
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
                        "model-texture-atlas")
            category = f"characters/{safe_component(character)}/{role}"
            if direct_characters:
                confidence = "medium" if role == "model-texture-atlas" else "high"
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
            category = f"outfits/{safe_component(source_name)}"
            basis.append("model/material references establish a model bundle; outfit role is inferred")
        elif visual.get("alpha_transparent", 0) > 0.15 and visual.get("palette_bins", 0) <= 24:
            category = "effects/alpha-sprites"
            basis.append("transparent, limited-palette image heuristic")
        elif visual.get("width", 0) <= 64 and visual.get("height", 0) <= 64:
            category = "ui-hud-fonts/small-textures"
            basis.append("small dimensions heuristic")
        elif visual.get("visual_hint") == "green-dominant":
            category = "foliage/green-dominant"
            basis.append("average-color heuristic")
        elif visual.get("visual_hint") == "blue-dominant":
            category = "water-sky/blue-dominant"
            basis.append("average-color heuristic")
        else:
            category = "other/unclassified"
            basis.append("no identifying container token or model reference")
        if layout_note:
            basis.append(layout_note)
    if record.get("layout", (None, 1))[1] > 1 and category.split("/", 1)[0] in {"characters", "outfits"}:
        top, subcategory = category.split("/", 1)
        category = f"{top}/{subcategory}-{layout}"
    if confidence == "low":
        basis.insert(0, "LOW-CONFIDENCE GUESS")
    record["category"] = category
    record["confidence"] = confidence
    record["notes"] = "; ".join(basis)


def link_texture(source, destination):
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.exists():
        if os.path.samefile(source, destination):
            return "hard link" if source.stat().st_ino == destination.stat().st_ino else "copy"
        raise FileExistsError(f"refusing to replace existing output: {destination}")
    try:
        os.link(source, destination)
        return "hard link"
    except OSError:
        shutil.copy2(source, destination)
        return "copy"


def remove_previous_links(output, texture_root):
    index = output / "index.csv"
    if not index.is_file():
        return
    with index.open(encoding="utf-8", newline="") as stream:
        for row in csv.DictReader(stream):
            key, category = row.get("key", ""), row.get("category", "")
            if not KEY_PATTERN.fullmatch(key) or "/" not in category:
                continue
            top, subcategory = category.split("/", 1)
            destination = output / safe_component(top) / safe_component(subcategory) / f"{key}.png"
            source = texture_root / f"{key}.png"
            if destination.is_file() and source.is_file() and os.path.samefile(source, destination):
                destination.unlink()


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
    project_root = rip_root.parent.parent
    private_root = project_root / "private"
    output = Path(output or rip_root / "sorted").resolve()
    try:
        output.relative_to(private_root.resolve())
    except ValueError as error:
        raise ValueError("sorted output must remain under the repository private directory") from error
    texture_root = rip_root / "textures"
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
    for record in records:
        record["character_aliases"] = character_aliases
        classify_record(record, classify_names)
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
        category, subcategory = record["category"].split("/", 1)
        destination = output / safe_component(category) / safe_component(subcategory) / f"{record['key']}.png"
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
    candidates = [project_root / "private" / "textures"]
    if len(project_root.parents) > 1:
        candidates.append(project_root.parents[1] / "texrep" / "doaxbv-re" / "private" / "textures")
    return [path for path in candidates if path.is_dir()]


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
