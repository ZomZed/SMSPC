"""Writes port/patches/aurora_<name>.patch (unified diffs against pristine Aurora files).

    python port/tools/aurora_patch_diff.py <name> lib/gx/shader.cpp [more files...]

The pristine copies live in port/patches/orig/<same relative path> (saved before the
first edit of each file); the live files are in port_deps/aurora. Apply a patch to a
clean Aurora checkout with `git apply` (paths are a/<rel>, b/<rel>) or `patch -p1`.
"""
import difflib
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ORIG = ROOT / "port" / "patches" / "orig"


def find_aurora():
    for up in (ROOT.parent, ROOT.parents[1]):
        p = up / "port_deps" / "aurora"
        if p.exists():
            return p
    sys.exit("port_deps/aurora not found")


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    name, files = sys.argv[1], sys.argv[2:]
    aurora = find_aurora()
    out = []
    for rel in files:
        a = (ORIG / rel).read_text(encoding="utf-8").splitlines(keepends=True)
        b = (aurora / rel).read_text(encoding="utf-8").splitlines(keepends=True)
        out += difflib.unified_diff(a, b, f"a/{rel}", f"b/{rel}")
    dst = ROOT / "port" / "patches" / f"aurora_{name}.patch"
    dst.write_text("".join(out), encoding="utf-8", newline="\n")
    print(f"wrote {dst} ({len(out)} lines)")


if __name__ == "__main__":
    main()
