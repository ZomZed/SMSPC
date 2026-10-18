"""List non-void functions that can fall off their end (GameCube returns stale r3, clang = UB).

    python port/tools/find_noreturn.py [filter]

Reuses census.py's units and flags, enabling only -Wreturn-type.
"""
import concurrent.futures
import os
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import census  # noqa: E402

PAT = re.compile(r"^(.*?):(\d+):\d+: warning: (non-void function '?(.*?)'? does not return a value.*|control reaches end of non-void function.*)$")


def run(u):
    src = census.ROOT / u["src"]
    if not src.exists():
        return []
    lang = ["-x", "c", "-std=gnu99"] if src.suffix == ".c" and "libs/JSystem/" not in src.as_posix() else ["-x", "c++", "-std=gnu++17"]
    common = [c for c in census.COMMON if c not in ("-w", "-Wno-everything", "-ferror-limit=3")]
    cmd = [census.CLANG, *common, "-ferror-limit=0", "-Wno-everything", "-Wreturn-type", *lang]
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
        m = re.match(r"^(.*?):(\d+):\d+: warning: (.*)\[-Wreturn-type\]", l)
        if m and ("non-void" in m.group(3)):
            out.append((m.group(1).replace("\\", "/"), int(m.group(2)), m.group(3)))
    return out


def main():
    flt = sys.argv[1] if len(sys.argv) > 1 else ""
    us = [u for u in census.units() if u["src"].startswith(flt) or u["name"].startswith(flt)]
    with concurrent.futures.ThreadPoolExecutor(max(1, (os.cpu_count() or 4) - 2)) as ex:
        res = list(ex.map(run, us))
    seen = set()
    for r in res:
        for f, ln, msg in r:
            if "port/" in f and "/src/" not in f and not f.startswith(("src", "libs")):
                pass
            k = (f, ln, msg)
            if k in seen:
                continue
            seen.add(k)
    for f, ln, msg in sorted(seen):
        print(f"{f}:{ln}: {msg}")
    print(f"{len(seen)} hits")


if __name__ == "__main__":
    main()
