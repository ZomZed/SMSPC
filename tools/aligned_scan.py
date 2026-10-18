#!/usr/bin/env python3
"""Aligned objdiff scan for logic differences in NonMatching functions.

Usage (from the repo root, after `python -m ninja`):
    python tools/aligned_scan.py OUT.txt [unit-prefix ...]
    e.g. python tools/aligned_scan.py build/aligned.txt Enemy/ MoveBG/MapObjBall

For every nonmatching function of the NonMatching game units in configure.py (or only those
whose path starts with one of the prefixes) it runs objdiff once per unit and keeps only the
aligned instruction pairs whose difference is unlikely to be register/scheduling noise:
  - same base register, different displacement (wrong field offset)   weight 5
  - different `cror` condition (<= vs >=)                               weight 5
  - different immediate in cmpwi/cmplwi/li/addi/rlwinm/ori/andi./lis   weight 5
  - different data value (float/double literal from .sdata2/.rodata)   weight 4
  - different call target, or a call present on one side only          weight 3
Constructors (A::A) and the MarNameRefGen factories are skipped (audited separately).
Output: one `### unit  score N` block per unit, functions sorted by score, aligned pairs as
`marker addr | target | ours` (marker as in tools/decomp-diff.py).
Swapped register/load order still shows up as pairs of lines with exchanged values; use
tools/aligned_isolated.py to drop those. Confirm every hit with tools/decomp-diff.py -d.
"""
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

OFFSET = re.compile(r"\{(-?0x[0-9a-f]+|\d+)\}\((r\d+)\)")
IMM_OPS = ("cmpwi", "cmplwi", "li", "addi", "rlwinm", "rlwinm.", "ori", "andi.", "lis")
INLINE_CALLS = re.compile(r"JGeometry|TVec|__ct|__dt|TUtil|operator")


def weight(marker, lt, rt):
    if marker == "~":
        op = lt.split()[0] if lt else ""
        ml, mr = OFFSET.search(lt), OFFSET.search(rt)
        if (ml and mr and ml.group(2) == mr.group(2) and ml.group(2) != "r1"
                and "{r" not in lt.split(",")[0] + rt.split(",")[0]):
            return 5
        if op == "cror":
            return 5
        if op in IMM_OPS and re.search(r"\{-?(0x[0-9a-f]+|\d+)\}", lt) and "r1" not in lt:
            return 5
        if "{@" in lt or "{@" in rt:
            return 4
        if op == "bl":
            return 3
        return 0
    if marker in "<>":
        t = lt or rt
        if t.startswith("bl ") and not INLINE_CALLS.search(t):
            return 3
    return 0


MARK = {"DIFF_ARG_MISMATCH": "~", "DIFF_OP_MISMATCH": "~", "DIFF_REPLACE": "|",
        "DIFF_INSERT": ">", "DIFF_DELETE": "<"}


def scan(unit):
    try:
        with contextlib.redirect_stderr(io.StringIO()):
            data = dd.run_objdiff("mario/" + unit)
    except BaseException:  # decomp-diff exits on errors
        return unit, []
    left = data.get("left", {}).get("symbols", [])
    right = data.get("right", {}).get("symbols", [])
    res = []
    for s in left:
        mp = s.get("match_percent")
        name = s.get("demangled_name", s.get("name", "")) or ""
        if mp is None or mp >= 100.0 or not s.get("instructions"):
            continue
        if re.search(r"(\w+)::\1\(", name) or "MarNameRefGen" in unit or "__sinit" in name:
            continue
        ts = s.get("target_symbol")
        r = right[ts] if ts is not None and ts < len(right) else {}
        li, ri = s.get("instructions", []), r.get("instructions", [])
        lines, score = [], 0
        for i in range(max(len(li), len(ri))):
            a = li[i] if i < len(li) else {}
            b = ri[i] if i < len(ri) else {}
            kind = a.get("diff_kind", "") or b.get("diff_kind", "")
            if not kind:
                continue
            m = MARK.get(kind, "?")
            lt = dd.render_instruction(a, left, is_diff=True) if a else ""
            rt = dd.render_instruction(b, right, is_diff=True) if b else ""
            w = weight(m, lt, rt)
            if w:
                score += w
                addr = a.get("instruction", {}).get("address") or b.get("instruction", {}).get("address") or 0
                lines.append(f"{m}{int(addr):6x} | {lt[:60]:<60} | {rt[:60]}")
        if score:
            res.append((score, name, mp, lines))
    res.sort(key=lambda x: -x[0])
    return unit, res


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    units = re.findall(r'Object\(NonMatching, "([^"]+)"', open("configure.py", encoding="utf-8").read())
    units = [re.sub(r"\.(cpp|c)$", "", u) for u in units
             if not u.startswith(("JSystem/", "PowerPC", "TRK", "THP"))]
    if sys.argv[2:]:
        units = [u for u in units if any(u.startswith(p) for p in sys.argv[2:])]
    with ThreadPoolExecutor(8) as ex:
        out = list(ex.map(scan, units))
    with open(sys.argv[1], "w", encoding="utf-8") as fo:
        for u, res in out:
            if not res:
                continue
            fo.write(f"### {u}  score {sum(r[0] for r in res)}\n")
            for sc, name, mp, lines in res:
                fo.write(f"  [{sc}] {name[:110]}  ({mp:.1f}%)\n")
                for l in lines[:25]:
                    fo.write("      " + l + "\n")
    print(f"{len(units)} units scanned -> {sys.argv[1]}")


if __name__ == "__main__":
    main()
