"""List pointer <-> 32-bit hazards that clang flags (x64 LLP64).

    python port/tools/find_ptr32.py [filter] [-o out.txt] [--all]

Reuses census.py's units and flags, enabling only the pointer-truncation
warnings. Each hit is de-duplicated by (file, line, kind). By default hits in
port/ host code and the Dolphin SDK (replaced on PC) are dropped; --all keeps
them. Output lines: `file:line: [kind] message`, then per-kind and per-directory
counts. See port/POINTERS.md for how the hits were triaged.
"""
import argparse
import collections
import concurrent.futures
import os
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import census  # noqa: E402

FLAGS = ["-Wshorten-64-to-32", "-Wpointer-to-int-cast", "-Wint-to-pointer-cast",
         "-Wvoid-pointer-to-int-cast", "-Wint-to-void-pointer-cast"]
PAT = re.compile(r"^(.*?):(\d+):\d+: warning: (.*)\[(-W[\w-]+)\]\s*$")


def run(u):
    src = census.ROOT / u["src"]
    if not src.exists():
        return []
    lang = ["-x", "c", "-std=gnu99"] if src.suffix == ".c" and "libs/JSystem/" not in src.as_posix() else ["-x", "c++", "-std=gnu++17"]
    common = [c for c in census.COMMON if c not in ("-w", "-Wno-everything", "-ferror-limit=3")]
    cmd = [census.CLANG, *common, "-ferror-limit=0", "-Wno-everything", *FLAGS, *lang]
    for i in census.INCLUDES:
        cmd += ["-I", i]
    for d in census.DEFINES:
        cmd += ["-D", d]
    if u["pch"]:
        cmd += ["-include", "include/SMS.pch"]
    cmd.append(u["src"])
    p = subprocess.run(cmd, cwd=census.ROOT, capture_output=True, text=True, errors="replace")
    out = []
    for l in p.stderr.splitlines():
        m = PAT.match(l)
        if m:
            out.append((m.group(1).replace("\\", "/"), int(m.group(2)), m.group(4), m.group(3).strip()))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("filter", nargs="?", default="")
    ap.add_argument("-o", help="write the hit list to this file")
    ap.add_argument("--all", action="store_true", help="keep port/ and dolphin hits")
    a = ap.parse_args()
    us = [u for u in census.units() if u["src"].startswith(a.filter) or u["name"].startswith(a.filter)]
    with concurrent.futures.ThreadPoolExecutor(max(1, (os.cpu_count() or 4) - 2)) as ex:
        res = list(ex.map(run, us))
    seen = {}
    for r in res:
        for f, ln, kind, msg in r:
            if not a.all and (f.startswith("port/") or "libs/dolphin/" in f):
                continue
            seen.setdefault((f, ln, kind), msg)
    lines = [f"{f}:{ln}: [{k}] {m}" for (f, ln, k), m in sorted(seen.items())]
    kinds = collections.Counter(k for (_, _, k) in seen)
    dirs = collections.Counter("/".join(f.split("/")[:3] if f.startswith("libs") else f.split("/")[:2]) for (f, _, _) in seen)
    summary = [f"{len(seen)} hits"] + [f"  {n:5d} {k}" for k, n in kinds.most_common()]
    summary += [f"  {n:5d} {d}" for d, n in dirs.most_common()]
    text = "\n".join(lines + summary)
    if a.o:
        Path(a.o).write_text(text, encoding="utf-8")
        print("\n".join(summary))
    else:
        print(text)


if __name__ == "__main__":
    main()
