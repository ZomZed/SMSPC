#!/usr/bin/env python3
"""Tool-agnostic matching loop for AI agents and humans (stdlib only, run from anywhere).

  python tools/ai_task.py next [--n 10] [--min-size 160] [prefix ...]   untried targets (skips match_tried.txt)
  python tools/ai_task.py show <unit> <function> [--full]               diff, signature resolved, noise filtered
  python tools/ai_task.py search <unit> <function>                      automated regalloc search (decl moves, swaps): run before hand-tuning
  python tools/ai_task.py build <unit>                                  rebuild that unit's object only
  python tools/ai_task.py check                                         full ninja + DOL sha + changes_all losses
  python tools/ai_task.py tried <unit> <name> <reason...>               record a dead end in tools/match_tried.txt

<unit> is the path under src/ without extension, as printed by `next` (e.g. Enemy/Kukku).
<function> is any substring of the (de)mangled name; if several functions match you get a list.
`check` exits 1 if the DOL is not byte-identical or any function lost more than 0.5 points
against build/GMSE01/baseline.json (create it with `python -m ninja baseline` BEFORE you edit).
See docs/AI_CONTRIBUTING.md.
"""
import argparse
import json
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PY = sys.executable
BUILD = os.path.join(ROOT, "build", "GMSE01")
LOSS_LIMIT = 0.5


def run(args, capture=False):
    return subprocess.run(args, cwd=ROOT, capture_output=capture, text=True, errors="replace")


def ninja(*targets):
    return run([PY, "-m", "ninja", *targets], capture=True)


def load_report():
    p = os.path.join(BUILD, "report.json")
    if not os.path.exists(p):
        sys.exit("build/GMSE01/report.json missing: run `python -m ninja` first (see docs/AI_CONTRIBUTING.md)")
    with open(p, encoding="utf-8") as f:
        return json.load(f)


def cmd_next(a):
    r = run([PY, os.path.join("tools", "match_targets.py"), "--top", str(a.n), "--min-size", str(a.min_size)] + (["--include-tried"] if a.include_tried else []) + a.prefix, capture=True)
    sys.stdout.write("  unmatched  fuzzy    size  unit                         function\n" + r.stdout)
    sys.stderr.write(r.stderr)
    print("Next: python tools/ai_task.py show <unit> <function>")
    return 0


def resolve(unit, query):
    """Return (demangled name with args, mangled name, fuzzy) candidates in the unit."""
    rep = load_report()
    uname = "mario/" + unit
    for u in rep["units"]:
        if u["name"] != uname:
            continue
        out = []
        q = query.lower()
        for f in u.get("functions") or []:
            dem = (f.get("metadata") or {}).get("demangled_name", f["name"])
            if q in dem.lower() or q in f["name"].lower():
                out.append((dem, f["name"], f.get("fuzzy_match_percent", 100.0)))
        return out
    sys.exit("unknown unit %r (use the path under src/ without extension, e.g. Enemy/Kukku)" % unit)


def cmd_show(a):
    cands = resolve(a.unit, a.function)
    if not cands:
        sys.exit("no function matching %r in %s" % (a.function, a.unit))
    exact = [c for c in cands if a.function in (c[0], c[1])]
    if len(cands) > 1 and len(exact) != 1:
        print("%d candidates, pass a longer substring or the full signature:" % len(cands))
        for dem, mang, fz in cands:
            print("  %6.2f%%  %s" % (fz, dem))
        return 1
    dem, mang, fz = (exact or cands)[0]
    sys.path.insert(0, os.path.join(ROOT, "tools"))
    from near_triage import real_lines
    cmd = [PY, os.path.join("tools", "decomp-diff.py"), "-u", "mario/" + a.unit, "-d", dem]
    full = run(cmd + ["--no-collapse"], capture=True).stdout
    real = real_lines(full)
    print("%s  (%.2f%%)" % (dem, fz))
    if not real:
        print("No real instruction differences: only register names, stack offsets (frame size) or "
              "literal-pool labels differ. Usually a poor target; see docs/MATCHING_CHEATSHEET.md "
              "'Frame-only functions' and tools/match_tried.txt.")
    else:
        print("%d real differing lines (register/offset/pool-only lines filtered out):" % len(real))
        print("\n".join(real[:200]))
        if len(real) > 200:
            print("... %d more (use `tools/decomp-diff.py -d --range`)" % (len(real) - 200))
    if a.full:
        print("\n--- full diff ---")
        print(run(cmd, capture=True).stdout)
    dp = os.path.join(ROOT, "tools", "regalloc_done.txt")
    if os.path.exists(dp):
        for l in open(dp, encoding="utf-8"):
            p = l.rstrip("\n").split("\t")
            if len(p) >= 5 and p[0] == a.unit and p[1] == dem:
                print("regalloc_search already ran: %s (%s, %s): decl moves / split-join / operand swaps "
                      "will not help; look for a structural change" % (p[2], p[3], p[4]))
    print("\nMangled: %s\nKnown dead ends: grep -n %s tools/match_tried.txt" % (mang, a.unit.split("/")[-1]))
    return 0


def cmd_search(a):
    cands = resolve(a.unit, a.function)
    exact = [c for c in cands if a.function in (c[0], c[1])]
    if len(cands) != 1 and len(exact) != 1:
        sys.exit("need exactly one function, got %d (pass the full signature)" % len(cands))
    dem = (exact or cands)[0][0]
    r = run([PY, os.path.join("tools", "regalloc_search.py"), "--unit", a.unit, "--func", dem,
             "--max-compiles", str(a.max_compiles)] + (["--redo"] if a.redo else []), capture=True)
    out = [l for l in r.stdout.splitlines() if "<- best" in l or "result:" in l or ":" in l[:80] and "#" not in l[:4]]
    print("\n".join(out[-12:]))
    return r.returncode


def cmd_build(a):
    src = None
    for ext in (".cpp", ".c"):
        for base in ("src",):
            p = os.path.join(ROOT, base, a.unit + ext)
            if os.path.exists(p):
                src = p
    if not src:
        sys.exit("src/%s.cpp|.c not found (libs units: use `python -m ninja build/GMSE01/src/<path>.o`)" % a.unit)
    obj = os.path.join("build", "GMSE01", "src", a.unit.replace("/", os.sep) + ".o")
    r = ninja(obj)
    out = (r.stdout + r.stderr).strip().splitlines()
    print("\n".join(out[-40:]))
    print("build %s: %s" % (obj, "OK" if r.returncode == 0 else "FAILED"))
    return r.returncode


def cmd_check(a):
    bad = 0
    r = ninja()
    text = r.stdout + r.stderr
    errs = [l for l in text.splitlines() if "FAILED" in l or "error:" in l]
    if r.returncode != 0:
        print("BUILD FAILED"); print("\n".join(errs[:20])); return 1
    dol = [l for l in text.splitlines() if "mario.dol" in l]
    dtk = os.path.join(ROOT, "build", "tools", "dtk" + (".exe" if os.name == "nt" else ""))
    sha = run([dtk, "shasum", "-c", os.path.join("config", "GMSE01", "build.sha1")], capture=True)
    shaout = re.sub(r"\x1b\[[0-9;]*m", "", (sha.stdout + sha.stderr)).strip()
    print("DOL:", shaout or (dol[-1] if dol else "?"))
    if sha.returncode != 0 or "OK" not in shaout:
        print("!! DOL is NOT byte-identical: some edit changed linked code/data. Revert or fix."); bad = 1
    if not os.path.exists(os.path.join(BUILD, "baseline.json")):
        print("no baseline.json: run `python -m ninja baseline` on an unedited tree first; loss check skipped")
        return bad
    ch = ninja("changes_all")
    cur_unit, units, funcs, losses = None, [], [], []
    for l in (ch.stdout + ch.stderr).splitlines():
        m = re.match(r"\s*(\S.*?)\s*\|\s*fuzzy_match\s*\|\s*([\d.]+)%\s*->\s*([\d.]+)%", l)
        if not m:
            continue
        name, old, new = m.group(1), float(m.group(2)), float(m.group(3))
        (units if name.startswith("mario/") else funcs).append((name, old, new))
        if new < old - LOSS_LIMIT and not name.startswith("mario/"):
            losses.append((name, old, new))
    for n, o, w in units:
        print("unit  %-50s %.2f -> %.2f" % (n, o, w))
    print("functions changed: %d  improved: %d  worse: %d" % (len(funcs), sum(1 for f in funcs if f[2] > f[1]), sum(1 for f in funcs if f[2] < f[1])))
    for n, o, w in sorted(funcs, key=lambda x: x[2] - x[1]):
        if w < o:
            print("  %s %s  %.2f -> %.2f" % ("LOSS>0.5" if (n, o, w) in losses else "loss    ", n, o, w))
    if losses:
        print("!! %d function(s) lost more than %.1f points: undo or fix them." % (len(losses), LOSS_LIMIT)); bad = 1
    print("CHECK", "FAILED" if bad else "PASSED")
    return bad


def cmd_tried(a):
    p = os.path.join(ROOT, "tools", "match_tried.txt")
    reason = " ".join(a.reason).replace("\t", " ")
    with open(p, "a", encoding="utf-8", newline="\n") as f:
        f.write("%s\t%s\t%s\n" % (a.unit, a.name, reason))
    print("recorded: %s\t%s\t%s" % (a.unit, a.name, reason))
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("next"); p.add_argument("--n", type=int, default=10); p.add_argument("--min-size", type=int, default=160); p.add_argument("--include-tried", action="store_true"); p.add_argument("prefix", nargs="*"); p.set_defaults(f=cmd_next)
    p = sub.add_parser("show"); p.add_argument("unit"); p.add_argument("function"); p.add_argument("--full", action="store_true"); p.set_defaults(f=cmd_show)
    p = sub.add_parser("search"); p.add_argument("unit"); p.add_argument("function"); p.add_argument("--max-compiles", type=int, default=60); p.add_argument("--redo", action="store_true"); p.set_defaults(f=cmd_search)
    p = sub.add_parser("build"); p.add_argument("unit"); p.set_defaults(f=cmd_build)
    p = sub.add_parser("check"); p.set_defaults(f=cmd_check)
    p = sub.add_parser("tried"); p.add_argument("unit"); p.add_argument("name", help="substring of the mangled name"); p.add_argument("reason", nargs="+"); p.set_defaults(f=cmd_tried)
    a = ap.parse_args()
    sys.exit(a.f(a))


if __name__ == "__main__":
    main()
