# SPDX-License-Identifier: GPL-3.0-or-later
"""Extract a user-owned Xbox ISO locally with XboxDev extract-xiso."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys


def package_root():
    return (Path(sys.executable).resolve().parent if getattr(sys, "frozen", False)
            else Path(__file__).resolve().parents[1])


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


class ToolError(subprocess.CalledProcessError):
    """Ends with the tool's last line, which says why it failed; users report only this message (#160)."""
    def __str__(self):
        return f"{super().__str__()} {self.output}"


def run_logged(args, log_path, cwd=None):
    print(f"{log_path.stem}... (log: {log_path})", flush=True)
    last = b""
    with log_path.open("xb") as log:
        with subprocess.Popen([str(arg) for arg in args], cwd=cwd,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT) as process:
            for line in process.stdout:
                log.write(line)
                log.flush()
                print(line.decode("utf-8", errors="replace"), end="", flush=True)
                last = line.strip() or last
            code = process.wait()
        if code:
            raise ToolError(code, args, f"{last.decode('utf-8', errors='replace')} (log: {log_path})")


def listing_files(text):
    files, seen = {}, set()
    count = None
    for line in text.splitlines():
        entry = re.fullmatch(r"([/\\].+) \((\d+) bytes\)", line)
        if entry:
            name, size = entry.groups()
            directory = name.endswith(("/", "\\"))
            name = name.replace("\\", "/")[1:]
            if directory:
                name = name[:-1]
            parts = name.split("/")
            for part in parts:
                reserved = part.split(".")[0].upper()
                if (not part or part in (".", "..") or part[-1:] in (".", " ")
                        or any(ord(c) < 32 or c in '<>:"|?*' for c in part)
                        or reserved in {"CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$"}
                        or re.fullmatch(r"(?:COM|LPT)[0-9Â¹Â²Â³]", reserved)):
                    raise ValueError("Image contains an unsafe Windows path")
            key = name.casefold()
            if key in seen:
                raise ValueError("Image contains duplicate or case-colliding paths")
            seen.add(key)
            if not directory:
                files[name] = int(size)
        elif re.fullmatch(r"\d+ files in .+ total \d+ bytes", line):
            count = int(line.split()[0])
        elif line and not line.startswith(("extract-xiso ", "listing ")):
            raise ValueError("Unrecognized extract-xiso listing; see listing.log")
    if not files or count != len(files):
        raise ValueError("Incomplete or empty image listing")
    file_keys = {name.casefold() for name in files}
    for name in seen:
        parts = name.split("/")
        if any("/".join(parts[:i]) in file_keys for i in range(1, len(parts))):
            raise ValueError("Image uses a file as a parent directory")
    return files


def extract(iso, output, tool):
    private = package_root() / "private"
    private.mkdir(exist_ok=True)
    output = output.absolute()
    for path in (private, output, *output.parents):
        if path.is_symlink() or getattr(path, "is_junction", lambda: False)():
            raise ValueError("Output must not traverse a symlink or junction")
    output = output.resolve()
    if output == private.resolve() or not output.is_relative_to(private.resolve()):
        raise ValueError("Choose a new output directory beneath this repository's private/")
    iso = iso.resolve(strict=True)
    if not iso.is_file():
        raise ValueError("Input must be an ISO file")
    executable = shutil.which(str(tool))
    if not executable:
        raise ValueError("Install XboxDev extract-xiso or pass --extractor <executable>")
    executable = str(Path(executable).resolve())
    output.mkdir(parents=True, exist_ok=False)
    print("Checking ISO hash...", flush=True)
    original_hash = sha256(iso)
    run_logged([executable, "-l", iso], output / "listing.log")
    files = listing_files((output / "listing.log").read_text(encoding="utf-8", errors="strict"))
    if shutil.disk_usage(output).free < sum(files.values()):
        raise ValueError("Not enough disk space for the listed files")
    disc = output / "disc"
    run_logged([executable, "-x", "-d", disc, iso], output / "extract.log")
    actual = {}
    for path in disc.rglob("*"):
        if path.is_symlink() or getattr(path, "is_junction", lambda: False)():
            raise ValueError("Extractor produced a link")
        if path.is_file():
            actual[path.relative_to(disc).as_posix()] = path.stat().st_size
    if actual != files:
        raise ValueError("Extracted files differ from the listing; see extract.log")
    print("Verifying extracted files and unchanged ISO...", flush=True)
    if sha256(iso) != original_hash:
        raise ValueError("Input image changed during extraction")
    with (output / "receipt.tmp").open("x", encoding="utf-8") as receipt:
        json.dump(dict(status="extracted", iso_sha256=original_hash,
                       extractor_sha256=sha256(Path(executable)),
                       files=files), receipt, indent=2)
    (output / "receipt.tmp").rename(output / "receipt.json")
    return disc


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("iso", type=Path)
    parser.add_argument("--output", type=Path,
                        help="New directory under this repository's private/")
    bundled = package_root() / "tools/artifacts/extract-xiso.exe"
    parser.add_argument("--extractor", default=str(bundled) if bundled.is_file() else "extract-xiso",
                        help="Path to XboxDev extract-xiso (default: PATH)")
    args = parser.parse_args()
    if args.output is None:
        args.output = package_root() / "private" / "imported-disc"
    print(f"Extracting to {args.output}. Large images can take several minutes.", flush=True)
    try:
        disc = extract(args.iso, args.output, args.extractor)
    except (OSError, ValueError, UnicodeError, subprocess.CalledProcessError) as error:
        print(f"Extraction failed: {error}. Any partial output is retained; use a new destination.", file=sys.stderr)
        return 1
    print(f"Extracted to {disc}. This does not generate or build the game runner.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
