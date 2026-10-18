"""Host test for the RVZ/WIA/GCZ disc-image reader (port/src/port_disc_wia.cpp). (w9)

    python port/tools/test_disc.py <image> [<image> ...] [--iso game.iso] [--full]

Builds port/tools/test_disc.cpp with the reader, zstd and zlib from Aurora's build and bzip2
from port_deps, then compares every file of the ISO's FST (with --full: the whole disc and
random ranges) read through each image against the plain ISO. Test images can be made with
Dolphin's DolphinTool, e.g.
    DolphinTool convert -i game.iso -o sms.rvz -f rvz -c zstd -l 5 -b 131072
"""
import argparse
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import build_aurora  # noqa: E402

ROOT = build_aurora.ROOT
DEPS = build_aurora.AURORA_BUILD / "_deps"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("images", nargs="+")
    ap.add_argument("--iso", default=str(ROOT / "orig" / "GMSE01" / "game.iso"))
    ap.add_argument("--full", action="store_true")
    a = ap.parse_args()
    out = ROOT / "port" / "build" / "test_disc"
    out.mkdir(parents=True, exist_ok=True)
    clang = build_aurora.CLANG
    inc = ["-I", str(DEPS / "zstd-src" / "lib"), "-I", str(DEPS / "zlib-build"), "-I", str(build_aurora.BZIP2_DIR)]
    xz = build_aurora.XZ_DIR
    lzma = ["-DPORT_DISC_HAVE_LZMA", "-DLZMA_API_STATIC", "-I", str(xz / "src" / "liblzma" / "api")] if xz.exists() else []
    objs = []
    for s, lang in (("port/tools/test_disc.cpp", "c++"), ("port/src/port_disc_wia.cpp", "c++")):
        obj = out / (Path(s).stem + ".obj")
        r = subprocess.run([clang, "--target=x86_64-pc-windows-msvc", "-c", "-O2", "-x", lang, "-std=c++17",
                            "-fms-runtime-lib=dll", "-D_CRT_SECURE_NO_WARNINGS", "-DPORT_DISC_STANDALONE",
                            *inc, *lzma, s, "-o", str(obj)], cwd=ROOT)
        if r.returncode:
            sys.exit(f"compile failed: {s}")
        objs.append(str(obj))
    for s in build_aurora.BZIP2_SOURCES:
        obj = out / ("bz_" + Path(s).stem + ".obj")
        r = subprocess.run([clang, "--target=x86_64-pc-windows-msvc", "-c", "-O2", "-x", "c", "-w",
                            "-fms-runtime-lib=dll", "-D_CRT_SECURE_NO_WARNINGS", "-DBZ_EXPORT",
                            str(build_aurora.BZIP2_DIR / s), "-o", str(obj)], cwd=ROOT)
        if r.returncode:
            sys.exit(f"compile failed: {s}")
        objs.append(str(obj))
    if xz.exists():
        for s in build_aurora.XZ_SOURCES:
            obj = out / ("xz_" + s.replace("/", "_") + ".obj")
            r = subprocess.run([clang, "--target=x86_64-pc-windows-msvc", "-c", "-O2", "-x", "c", "-w",
                                "-std=gnu11", "-fms-runtime-lib=dll", *[f"-D{d}" for d in build_aurora.XZ_DEFINES],
                                *[f"-I{xz / i}" for i in build_aurora.XZ_INCLUDES],
                                str(xz / "src" / "liblzma" / s), "-o", str(obj)], cwd=ROOT)
            if r.returncode:
                sys.exit(f"compile failed: {s}")
            objs.append(str(obj))
    exe = out / "test_disc.exe"
    libs = [str(DEPS / "zstd-build" / "lib" / "zstd_static.lib"), str(DEPS / "zlib-build" / "zlib.lib")]
    r = subprocess.run([clang, "--target=x86_64-pc-windows-msvc", "-fuse-ld=lld", "-fms-runtime-lib=dll",
                        *objs, *libs, "-o", str(exe)], cwd=ROOT, capture_output=True, text=True)
    if r.returncode:
        print(r.stdout + r.stderr)
        sys.exit("link failed")
    for dll in (DEPS / "zlib-build").glob("zlib1.dll"):
        shutil.copy2(dll, out / dll.name)
    rc = 0
    for img in a.images:
        print(f"== {img}")
        r = subprocess.run([str(exe), img, a.iso] + (["full"] if a.full else []), cwd=out)
        rc |= r.returncode
    sys.exit(rc)


if __name__ == "__main__":
    main()
