"""Rank near-matching functions by REAL instruction differences.

Runs decomp-diff --no-collapse for every function that match_targets.py lists in a % band and
drops diff lines that vanish once register names ({..} markers), stack offsets (N(r1)) and
literal-pool labels (@N@) are ignored. Functions with 0 remaining lines differ only in register
allocation / frame size / pool layout; the rest are worth a look.
Usage: python tools/near_triage.py [--min 98.5] [--max 99.95] [--out file]
"""
import argparse, re, subprocess, sys, os
from concurrent.futures import ThreadPoolExecutor

NOISE = re.compile(r"\{[^}]*\}|@\d+@?|-?0x[0-9a-f]+\(r1\)|\d+\(r1\)")

def real_lines(text):
    out = []
    for l in text.splitlines():
        if not l or l[0] not in "~<>|":
            continue
        p = l.split("|")
        if len(p) < 3:
            continue
        a, b = p[1].strip(), p[2].strip()
        if a and b and NOISE.sub("", a) == NOISE.sub("", b):
            continue
        if a and b and a.split()[0] == b.split()[0] and a.split()[0] in (
                "stw", "lwz", "stfs", "lfs", "stfd", "lfd", "addi", "bl", "stwu", "lis") and ("r1" in a + b or "@" in a + b):
            continue
        out.append(l.rstrip())
    return out

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--min", type=float, default=98.5)
    ap.add_argument("--max", type=float, default=99.95)
    ap.add_argument("--out", default=None)
    a = ap.parse_args()
    py = sys.executable
    t = subprocess.run([py, "tools/match_targets.py", "--top", "5000", "--min-size", "0",
                        "--min", str(a.min), "--max", str(a.max)], capture_output=True, text=True).stdout
    rows = []
    for l in t.splitlines():
        m = re.match(r"\s*(\d+)\s+([\d.]+)%\s+(\d+)B\s+(\S+)\s+(.*)$", l)
        if m:
            rows.append((m.group(4), m.group(5).strip(), m.group(2)))
    def one(r):
        unit, fn, pct = r
        name = fn.split("(")[0]
        d = subprocess.run([py, "tools/decomp-diff.py", "-u", "mario/" + unit, "-d", name, "--no-collapse"],
                           capture_output=True, text=True, errors="replace").stdout
        return (len(real_lines(d)), unit, fn, pct)
    with ThreadPoolExecutor(8) as ex:
        res = list(ex.map(one, rows))
    res.sort(key=lambda x: -x[0])
    lines = ["%3d real  %s%%  %s  %s" % (n, p, u, f) for n, u, f, p in res]
    txt = "\n".join(lines) + "\n%d functions, %d with real differences\n" % (len(res), sum(1 for x in res if x[0]))
    if a.out:
        open(a.out, "w", encoding="utf-8").write(txt)
    print(txt[-2000:])

if __name__ == "__main__":
    main()
