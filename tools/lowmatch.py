#!/usr/bin/env python3
"""List NonMatching game functions with a low match or a large size mismatch.

Usage (repo root, after `python -m ninja`):
    python tools/lowmatch.py [--below 60] [unit-prefix ...]

Prints `unit: match% target-size ours-size name` for every function under --below percent, or
whose size differs from the GameCube function by more than a quarter (at least 16 bytes).
These are where fabricated bodies hide (for example a callback that is only `return 0` in the
asm but plays a sound in our source). Constructors and the MarNameRefGen factories are skipped.
"""
import argparse
import contextlib
import importlib.util
import io
import os
import re
import sys
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)
_spec = importlib.util.spec_from_file_location("decomp_diff", os.path.join("tools", "decomp-diff.py"))
dd = importlib.util.module_from_spec(_spec)
_argv, sys.argv = sys.argv, [sys.argv[0]]
_spec.loader.exec_module(dd)
sys.argv = _argv


def scan(unit, below):
    try:
        with contextlib.redirect_stderr(io.StringIO()):
            data = dd.run_objdiff("mario/" + unit)
    except BaseException:
        return unit, []
    left = data.get("left", {}).get("symbols", [])
    right = data.get("right", {}).get("symbols", [])
    out = []
    for s in left:
        mp = s.get("match_percent")
        if mp is None or mp >= 100 or not s.get("instructions"):
            continue
        name = s.get("demangled_name", s.get("name", ""))
        if re.search(r"(\w+)::\1\(", name) or "__sinit" in name or "MarNameRefGen" in unit:
            continue
        ts = s.get("target_symbol")
        r = right[ts] if ts is not None and ts < len(right) else {}
        ls, rs = int(s.get("size", "0")), int(r.get("size", "0") or 0)
        if mp < below or (rs and abs(ls - rs) > max(16, ls // 4)):
            out.append(f"{mp:5.1f}% target {ls:5d}B ours {rs:5d}B  {name[:100]}")
    return unit, out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--below", type=float, default=60.0)
    ap.add_argument("prefix", nargs="*")
    a = ap.parse_args()
    units = re.findall(r'Object\(NonMatching, "([^"]+)"', open("configure.py", encoding="utf-8").read())
    units = [re.sub(r"\.(cpp|c)$", "", u) for u in units
             if not u.startswith(("JSystem/", "PowerPC", "TRK", "THP"))]
    if a.prefix:
        units = [u for u in units if any(u.startswith(p) for p in a.prefix)]
    with ThreadPoolExecutor(8) as ex:
        for u, out in ex.map(lambda x: scan(x, a.below), units):
            for o in out:
                print(f"{u}: {o}")


if __name__ == "__main__":
    main()
