"""Inventory of every file format on the disc (port audit, worker w10).

    python port/tools/disc_inventory.py [-o out.txt] [--ext .bck] [--paths]

Walks dolphin_test/files, lists the contents of every RARC (.szs/.arc) with
`dtk vfs ls -r`, and prints per-extension counts (files, archives, bytes).
--ext limits the listing to one extension; --paths prints every archive:path.
The format -> loader -> swap-status map lives in port/ENDIAN.md ("Disc format
inventory"); this script is how that table was produced.
"""
import argparse
import collections
import concurrent.futures
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DISC = next((u / "dolphin_test" / "files" for u in (ROOT.parent, ROOT.parents[1])
             if (u / "dolphin_test" / "files").exists()), ROOT.parent / "dolphin_test" / "files")
DTK = ROOT / "build" / "tools" / "dtk.exe"
UNITS = {"bytes": 1, "KiB": 1024, "MiB": 1024 * 1024}


def ls(arc):
    p = subprocess.run([str(DTK), "vfs", "ls", "-r", f"{arc}:"], capture_output=True, text=True,
                       errors="replace")
    out = []
    for line in p.stdout.splitlines():
        parts = [x.strip() for x in line.split("|")]
        if len(parts) < 3 or parts[2] != "File":
            continue
        m = re.match(r"([\d.]+)\s*(\w+)", parts[0])
        size = int(float(m.group(1)) * UNITS.get(m.group(2), 1)) if m else 0
        out.append((parts[1], size))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-o")
    ap.add_argument("--ext")
    ap.add_argument("--paths", action="store_true")
    a = ap.parse_args()
    loose, arcs = [], []
    for f in sorted(DISC.rglob("*")):
        if not f.is_file():
            continue
        rel = f.relative_to(DISC).as_posix()
        if f.suffix.lower() in (".szs", ".arc") and not rel.startswith("AudioRes/Seqs"):
            arcs.append(f)
        loose.append((rel, f.stat().st_size))
    with concurrent.futures.ThreadPoolExecutor(8) as ex:
        listed = list(ex.map(ls, arcs))
    files = [("(disc)", p, s) for p, s in loose]
    for arc, entries in zip(arcs, listed):
        name = arc.relative_to(DISC).as_posix()
        files += [(name, p, s) for p, s in entries]
    by_ext = collections.defaultdict(list)
    for arc, p, s in files:
        ext = Path(p).suffix.lower() or "(none)"
        by_ext[ext].append((arc, p, s))
    lines = [f"{len(files)} files ({len(loose)} loose, {len(arcs)} archives)", "",
             f"{'ext':8} {'files':>6} {'archives':>8} {'bytes':>11}  example"]
    for ext, items in sorted(by_ext.items(), key=lambda kv: -len(kv[1])):
        if a.ext and ext != a.ext:
            continue
        narc = len({i[0] for i in items})
        lines.append(f"{ext:8} {len(items):6d} {narc:8d} {sum(i[2] for i in items):11d}  "
                     f"{items[0][0]}:{items[0][1]}")
        if a.paths:
            lines += [f"    {arc}:{p} ({s})" for arc, p, s in items]
    text = "\n".join(lines)
    if a.o:
        Path(a.o).write_text(text + "\n", encoding="utf-8")
    print(text)


if __name__ == "__main__":
    main()
