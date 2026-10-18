"""PC-port compile census.

Compiles every game + JSystem translation unit listed in configure.py with the
Visual Studio-bundled clang (syntax check only, no codegen) and records which
ones compile and the first error of each failure.

    python port/tools/census.py            # all units
    python port/tools/census.py Enemy/     # only units whose path starts with this
    python port/tools/census.py -j 8 --show src/Enemy/bosseel.cpp

Writes port/CENSUS.md (summary + failures grouped by first error) and
port/census.json. Run from the project root. Does not touch the GameCube build.
"""
import argparse
import concurrent.futures
import json
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
VS_LLVM = Path(r"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\Llvm\x64\bin")
CLANG = os.environ.get("SMS_PORT_CLANG", str(VS_LLVM / "clang.exe"))

# Libraries that the PC build replaces with host implementations (port/src/...).
SKIP_PREFIXES = ("dolphin/", "PowerPC_EABI_Support/", "TRK_MINNOW_DOLPHIN/", "OdemuExi2/",
                 "THPPlayer/")
MIDDLEWARE = ("dolphin", "JSystem", "THPPlayer", "PowerPC_EABI_Support", "TRK_MINNOW_DOLPHIN",
              "OdemuExi2")
MSL = "libs/PowerPC_EABI_Support/include/PowerPC_EABI_Support/Msl"

INCLUDES = [
    "port/include",
    "include",
    *[f"libs/{lib}/include" for lib in MIDDLEWARE],
]  # MSL (CodeWarrior's libc) is replaced by the host C/C++ runtime headers
DEFINES = ["BUILD_VERSION=2", "VERSION_GMSE01", "VERSION_GMSP01",
           "LINK_ALL_SOURCE=1", "PORT_SKIP_CUTSCENES=1", "NDEBUG=1"]
COMMON = [
    "--target=x86_64-pc-windows-msvc",  # 64-bit: Aurora/Dawn ship for windows-amd64 only (see POINTERS.md)
    "-fsyntax-only", "-fno-exceptions", "-fno-rtti", "-fsigned-char",
    "-ferror-limit=3", "-w", "-Wno-everything",
    "-include", "port/include/port_compat.h",
]


def units():
    text = (ROOT / "configure.py").read_text(encoding="utf-8")
    out = []
    for m in re.finditer(r'(PCHObject|Object)\([^\n]*?"([^"]+\.(?:cpp|c))"', text):  # [^\n] keeps MatchingFor(...) units
        kind, name = m.group(1), m.group(2)
        if name.startswith(SKIP_PREFIXES):
            continue
        top, _, rest = name.partition("/")
        src = f"libs/{top}/src/{rest}" if top in MIDDLEWARE else f"src/{name}"
        out.append({"name": name, "src": src, "pch": kind == "PCHObject"})
    seen, uniq = set(), []
    for u in out:
        if u["name"] not in seen:
            seen.add(u["name"])
            uniq.append(u)
    return uniq


def compile_one(u):
    src = ROOT / u["src"]
    if not src.exists():
        return dict(u, status="missing", error="source file missing")
    lang = ["-x", "c", "-std=gnu99"] if src.suffix == ".c" and "libs/JSystem/" not in src.as_posix() else ["-x", "c++", "-std=gnu++17"]
    cmd = [CLANG, *COMMON, *lang]
    for i in INCLUDES:
        cmd += ["-I", i]
    for d in DEFINES:
        cmd += ["-D", d]
    if u["pch"]:
        cmd += ["-include", "include/SMS.pch"]
    cmd.append(u["src"])
    p = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, errors="replace")
    if p.returncode == 0:
        return dict(u, status="ok", error="")
    errs = [l for l in p.stderr.splitlines() if ": error:" in l or "fatal error:" in l]
    return dict(u, status="fail", error=errs[0] if errs else p.stderr.strip()[:300],
                errors=errs[:3], cmd=" ".join(cmd))


def bucket(err):
    """Group errors by message text with file/line/identifiers stripped."""
    m = re.search(r"error: (.*)", err)
    msg = m.group(1) if m else err
    msg = re.sub(r"'[^']*'", "'X'", msg)
    return msg[:120]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("filter", nargs="?", default="")
    ap.add_argument("-j", type=int, default=max(1, (os.cpu_count() or 4) - 2))
    ap.add_argument("--show", help="print full clang output for one source path")
    a = ap.parse_args()

    if a.show:
        u = next((x for x in units() if x["src"] == a.show.replace("\\", "/")), None)
        if not u:
            sys.exit("unit not found: " + a.show)
        r = compile_one(u)
        print(r.get("cmd", ""))
        p = subprocess.run(r.get("cmd", "echo ok").split(), cwd=ROOT)
        return

    us = [u for u in units() if u["name"].startswith(a.filter) or u["src"].startswith(a.filter)]
    with concurrent.futures.ThreadPoolExecutor(a.j) as ex:
        res = list(ex.map(compile_one, us))
    ok = [r for r in res if r["status"] == "ok"]
    bad = [r for r in res if r["status"] != "ok"]
    groups = {}
    for r in bad:
        groups.setdefault(bucket(r["error"]), []).append(r)

    lines = ["# PC port compile census", "",
             f"{len(ok)} / {len(res)} units compile (clang, syntax only, x86_64).", "",
             "## Failures grouped by first error", ""]
    for msg, rs in sorted(groups.items(), key=lambda kv: -len(kv[1])):
        lines.append(f"### {len(rs)} x `{msg}`")
        for r in rs[:12]:
            lines.append(f"- `{r['src']}`: {r['error'][:200]}")
        if len(rs) > 12:
            lines.append(f"- ... and {len(rs) - 12} more")
        lines.append("")
    if not a.filter:
        (ROOT / "port" / "CENSUS.md").write_text("\n".join(lines), encoding="utf-8")
        (ROOT / "port" / "census.json").write_text(json.dumps(res, indent=1), encoding="utf-8")
    print(f"{len(ok)} / {len(res)} units compile")
    for msg, rs in sorted(groups.items(), key=lambda kv: -len(kv[1]))[:15]:
        print(f"{len(rs):4d}  {msg}")


if __name__ == "__main__":
    main()
