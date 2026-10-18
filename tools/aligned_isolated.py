#!/usr/bin/env python3
"""Filter tools/aligned_scan.py output down to isolated (likely real) differences.

Usage:  python tools/aligned_isolated.py build/aligned.txt > build/isolated.txt

A pair is dropped when its target value appears among the "ours" values of the same function
and vice versa (two loads or immediates simply swapped by scheduling or register allocation).
Inserted/deleted lines are kept only when they are calls. What is left is a short list per
function: a single field offset, condition, immediate, literal or call that differs. Still
check each with `python tools/decomp-diff.py -u mario/<unit> -d <function>`: loop unrolling,
inlined helpers and different constant-pool labels for equal values remain false positives.
"""
import re
import sys


def values(line):
    parts = line.split("|")
    if len(parts) < 3:
        return None, None
    lb = [b for b in re.findall(r"\{([^}]*)\}", parts[1]) if not re.match(r"^[rf]\d+$", b)]
    rb = [b for b in re.findall(r"\{([^}]*)\}", parts[2]) if not re.match(r"^[rf]\d+$", b)]
    return tuple(lb), tuple(rb)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    blocks, unit, key = {}, None, None
    for l in open(sys.argv[1], encoding="utf-8"):
        l = l.rstrip("\n")
        if l.startswith("### "):
            unit = l.split()[1]
            continue
        m = re.match(r"\s+\[(\d+)\] (.*)  \(([\d.]+)%\)", l)
        if m:
            key = (unit, m.group(2), m.group(3))
            blocks[key] = []
        elif l.startswith("      ") and key:
            blocks[key].append(l.strip())
    for (u, f, mp), lines in blocks.items():
        pairs = [values(x) for x in lines]
        tv = [p[0] for p in pairs if p[0]]
        ov = [p[1] for p in pairs if p[1]]
        keep = []
        for x, (a, b) in zip(lines, pairs):
            if x.startswith(("<", ">")):
                if "bl " in x:
                    keep.append(x)
                continue
            if a is None or (a and b and a in ov and b in tv):
                continue
            keep.append(x)
        if keep:
            print(f"{u} :: {f[:90]} ({mp}%)")
            for k in keep[:8]:
                print("    " + k[:150])


if __name__ == "__main__":
    main()
