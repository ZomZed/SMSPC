"""Build and run port/tools/test_archive.cpp (endianness host test).

    python port/tools/test_archive.py                 # i686, same flags as build_port.py
    python port/tools/test_archive.py --target x86_64-pc-windows-msvc

Compiles the test plus the real JSUInputStream.cpp / JSUMemoryStream.cpp, the
real GC2D message readers / ParamInst.cpp and the JKernel heap + JKRMemArchive
units, links with clang + lld, and runs it from the project root.

The per-format tests need real files: this script pulls them out of the .szs
archives in dolphin_test/files/data with `dtk vfs cp` (option, common, params,
mario) into port/build/test_archive/data (once).
"""
import argparse
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import build_port  # noqa: E402

ROOT = build_port.ROOT
SRCS = ["port/tools/test_archive.cpp",
        "libs/JSystem/src/JSupport/JSUInputStream.cpp",
        "libs/JSystem/src/JSupport/JSUMemoryStream.cpp",
        "libs/JSystem/src/JSupport/JSUList.cpp",
        "libs/JSystem/src/JKernel/JKRDisposer.cpp",
        "libs/JSystem/src/JKernel/JKRHeap.cpp",
        "libs/JSystem/src/JKernel/JKRExpHeap.cpp",
        "libs/JSystem/src/JKernel/JKRSolidHeap.cpp",
        "libs/JSystem/src/JKernel/JKRFileLoader.cpp",
        "libs/JSystem/src/JKernel/JKRArchivePri.cpp",
        "libs/JSystem/src/JKernel/JKRArchivePub.cpp",
        "libs/JSystem/src/JKernel/JKRMemArchive.cpp",
        "src/GC2D/MessageLoader.cpp",
        "src/GC2D/MessageUtil.cpp",
        "src/System/ParamInst.cpp"]
DISC = Path("<project>/dolphin_test/files/data")
ARCHIVES = ["option", "common", "params", "mario", "title", "game_6", "guide", "select"]


def extract(dtk, data):
    for a in ARCHIVES:
        dst = data / a
        if dst.exists():
            continue
        r = subprocess.run([dtk, "vfs", "cp", f"{DISC / (a + '.szs')}:", str(dst)], cwd=ROOT,
                           stdout=subprocess.DEVNULL)
        if r.returncode:
            sys.exit(f"dtk vfs cp failed for {a}.szs")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--target", default="i686-pc-windows-msvc")
    ap.add_argument("szs", nargs="?")
    a = ap.parse_args()
    out = ROOT / "port" / "build" / "test_archive" / a.target
    out.mkdir(parents=True, exist_ok=True)
    objs = []
    for s in SRCS:
        fl = [f if not f.startswith("--target=") else f"--target={a.target}"
              for f in build_port.flags(False)]
        obj = out / (Path(s).stem + ".obj")
        r = subprocess.run([build_port.CLANG, *fl, s, "-o", str(obj)], cwd=ROOT)
        if r.returncode:
            sys.exit(f"compile failed: {s}")
        objs.append(str(obj))
    exe = out / "test_archive.exe"
    # The JKernel units reference DVD/ARAM/decompression code the test never
    # reaches; leave those unresolved instead of linking the whole kernel.
    # /largeaddressaware lets i686 map the arena at 0x80000000 too.
    r = subprocess.run([build_port.CLANG, f"--target={a.target}", "-fuse-ld=lld", "-g",
                        "-Wl,/force:unresolved", "-Wl,/largeaddressaware",
                        *objs, "-o", str(exe)], cwd=ROOT, capture_output=True, text=True)
    if r.returncode:
        sys.stderr.write(r.stdout + r.stderr)
        sys.exit("link failed")
    dtk = str(ROOT / "build" / "tools" / "dtk.exe")
    data = ROOT / "port" / "build" / "test_archive" / "data"
    extract(dtk, data)
    szs = a.szs or str(DISC / "common.szs")
    sys.exit(subprocess.run([str(exe), szs, dtk, str(data)], cwd=ROOT).returncode)


if __name__ == "__main__":
    main()
