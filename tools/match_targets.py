#!/usr/bin/env python3
"""List the best function targets for matching work, from build/GMSE01/report.json.

Usage (repo root, after `python -m ninja`):
    python tools/match_targets.py [--min-size 160] [--max 98.5] [--min 40] [--top 60] [unit-prefix ...]

Prints functions of NonMatching game units sorted by unmatched bytes (size * (100 - fuzzy)), skipping
tiny functions, near-matches (usually frame-only or register-only, rarely worth an agent's time) and the
functions docs/AGENT_MATCHING_TIPS.md lists as blocked by an unsolved mechanism (SKIP below).
"""
import re
import argparse, json, os, sys

# Blocked by known unsolved mechanisms (see the "Not solved yet" sections of docs/AGENT_MATCHING_TIPS.md).
SKIP = (
    ("Player/Tongue", "movement"),                # nested vector operator depth (libs)
    ("Player/MarioSpecial", "wire"),
    ("Enemy/fireWanwan", "doAdjustTarget"),       # TQuat4::rotate depth (libs)
    ("Enemy/TabePuku", "Drag"),
)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--min-size", type=int, default=160)
    ap.add_argument("--max", type=float, default=98.5, help="skip functions at or above this fuzzy %%")
    ap.add_argument("--min", type=float, default=0.0)
    ap.add_argument("--top", type=int, default=60)
    ap.add_argument("--include-tried", action="store_true",
                    help="also list functions in match_tried.txt, with the earlier note (fresh eyes / new patterns)")
    ap.add_argument("prefix", nargs="*")
    a = ap.parse_args()
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    rep = json.load(open(os.path.join(root, "build", "GMSE01", "report.json")))
    tried = []  # tools/match_tried.txt: functions already worked on without a fix
    tp = os.path.join(root, "tools", "match_tried.txt")
    if os.path.exists(tp):
        for line in open(tp, encoding="utf-8"):
            parts = line.rstrip("\n").split("\t")
            if len(parts) >= 2 and not line.startswith("#"):
                tried.append((parts[0], parts[1], parts[2] if len(parts) > 2 else ""))
    rows = []
    for u in rep["units"]:
        name = u["name"]
        if not name.startswith("mario/") or name.startswith("mario/JSystem/") or u["measures"].get("complete_units", 0):
            continue
        unit = name[len("mario/"):]
        if a.prefix and not any(unit.startswith(p) for p in a.prefix):
            continue
        for f in u.get("functions") or []:
            size = int(f.get("size", 0))
            fz = f.get("fuzzy_match_percent", 100.0)
            if size < a.min_size or fz >= a.max or fz < a.min:
                continue
            # an entry matches by substring, or when all its word tokens occur in the mangled name
            # (agents often write slightly wrong mangled lengths like perform__14 / 7perform)
            hit = [t for t in tuple((s0, s1, "skip") for s0, s1 in SKIP) + tuple(tried)
                   if unit == t[0] and (t[1] in f["name"] or all(w in f["name"] for w in re.findall(r"[A-Za-z_]{3,}", t[1])))]
            if hit and not a.include_tried:
                continue
            dem = (f.get("metadata") or {}).get("demangled_name", f["name"])
            if hit:
                dem += "   [tried: %s]" % hit[-1][2][:80]
            rows.append((size * (100.0 - fz) / 100.0, fz, size, unit, dem))
    rows.sort(reverse=True)
    for r in rows[:a.top]:
        print("%6.0f  %6.2f%%  %5dB  %-28s %s" % r)
    print("(%d candidates; showing %d)" % (len(rows), min(len(rows), a.top)), file=sys.stderr)


if __name__ == "__main__":
    main()
