"""Report how much of the program runs hand-written code in place of generated code.

Progress is measured the way decompilation projects measure it: code units
split the program into game code and libraries, and a function's bytes count
as covered once tools/decomp/functions.csv records hand-written code that runs
in its place. Run with --check to validate the ledger without the report.
"""
import argparse
import csv
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LEDGER = Path("tools/decomp/functions.csv")
UNITS = Path("tools/decomp/units.json")
STATUSES = ("replaced", "wrapped", "optional")


def recipe_functions(root=ROOT):
    """Function start -> size in bytes, from the generation recipe."""
    recipe = json.loads((root / "tools/game-recipe/recipe.json").read_text(encoding="utf-8"))
    sizes = {}
    for shard in recipe["functions"]:
        for entry in json.loads((root / shard).read_text(encoding="utf-8")):
            sizes[int(entry["start"], 16)] = entry["size"]
    for recovery in recipe["recoveries"]:
        for entry in json.loads((root / recovery).read_text(encoding="utf-8")):
            start = int(entry["start"], 16)
            sizes.setdefault(start, int(entry["end"], 16) - start)
    return sizes


def load_units(root=ROOT):
    return [dict(unit, start=int(unit["start"], 16), end=int(unit["end"], 16))
            for unit in json.loads((root / UNITS).read_text(encoding="utf-8"))]


def load_ledger(root=ROOT):
    with open(root / LEDGER, newline="", encoding="utf-8") as ledger:
        return list(csv.DictReader(ledger))


def runtime_sources(root=ROOT):
    return {path.relative_to(root).as_posix(): path.read_text(encoding="utf-8", errors="replace")
            for path in sorted((root / "recomp-runtime").iterdir())
            if path.suffix in (".c", ".cpp", ".h") and "_test." not in path.name}


def check(rows, sources, units):
    """Problems that make the ledger disagree with the runtime source."""
    errors, seen = [], set()
    generated_calls = {match.upper() for text in sources.values()
                       for match in re.findall(r"sub_[0-9A-Fa-f]{8}", text)}
    for line, row in enumerate(rows, start=2):
        where = f"{LEDGER}:{line}"
        try:
            address = int(row["address"], 16)
        except ValueError:
            errors.append(f"{where}: bad address {row['address']!r}")
            continue
        if address in seen:
            errors.append(f"{where}: duplicate address {row['address']}")
        seen.add(address)
        if row["status"] not in STATUSES:
            errors.append(f"{where}: status must be one of {', '.join(STATUSES)}")
        if row["source"] not in sources:
            errors.append(f"{where}: source {row['source']} is not a runtime source file")
        literal = re.compile(rf"0x0*{address:x}u?\b", re.IGNORECASE)
        if not any(literal.search(text) for text in sources.values()):
            errors.append(f"{where}: no runtime source mentions {row['address']}")
        if row["status"] == "replaced" and f"SUB_{address:08X}" in generated_calls:
            errors.append(f"{where}: replaced, but the runtime still calls sub_{address:08X}")
        if not any(unit["start"] <= address < unit["end"] for unit in units):
            errors.append(f"{where}: {row['address']} is outside every unit in {UNITS}")
    return errors


def report(rows, sizes, units):
    covered = {int(row["address"], 16): row["status"] for row in rows}
    # Entries the recipe never bounded still count, with no bytes.
    sizes = {**{address: 0 for address in covered}, **sizes}
    totals = {unit["name"]: {"functions": 0, "bytes": 0, **{s: [0, 0] for s in STATUSES}}
              for unit in units}
    for address, size in sizes.items():
        unit = next((u for u in units if u["start"] <= address < u["end"]), None)
        if unit is None:
            continue
        total = totals[unit["name"]]
        total["functions"] += 1
        total["bytes"] += size
        if address in covered:
            total[covered[address]][0] += 1
            total[covered[address]][1] += size
    lines = ["| Unit | Kind | Functions | Bytes | Replaced | Wrapped | Optional |",
             "|---|---|---:|---:|---:|---:|---:|"]

    def cells(total):
        def share(status):
            count, size = total[status]
            percent = 100.0 * size / total["bytes"] if total["bytes"] else 0.0
            return f"{count} ({percent:.2f}%)"
        return f"{total['functions']} | {total['bytes']} | " + " | ".join(share(s) for s in STATUSES)

    for kind in ("game", "library"):
        kind_total = {"functions": 0, "bytes": 0, **{s: [0, 0] for s in STATUSES}}
        for unit in (u for u in units if u["kind"] == kind):
            total = totals[unit["name"]]
            lines.append(f"| {unit['name']} | {kind} | {cells(total)} |")
            kind_total["functions"] += total["functions"]
            kind_total["bytes"] += total["bytes"]
            for status in STATUSES:
                kind_total[status] = [a + b for a, b in zip(kind_total[status], total[status])]
        lines.append(f"| **all {kind}** | {kind} | {cells(kind_total)} |")
    unlisted = sorted(a for a in covered if sizes[a] == 0)
    if unlisted:
        lines.append("")
        lines.append("Ledger entries without recipe bounds (counted with no bytes): "
                     + ", ".join(f"0x{a:08X}" for a in unlisted))
    return "\n".join(lines)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--check", action="store_true", help="validate the ledger only")
    args = parser.parse_args(argv)
    rows = load_ledger()
    units = load_units()
    errors = check(rows, runtime_sources(), units)
    for error in errors:
        print(error, file=sys.stderr)
    if not args.check:
        print(report(rows, recipe_functions(), units))
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())

