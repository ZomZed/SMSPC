"""Host test for the .ymp pollution map swap pass (port_swap_ymp) and the native
TPollutionLayerInfo array built by TPollutionManager::setDataAddress.

    python port/tools/test_ymp.py [--target x86_64-pc-windows-msvc]

Extracts map/ymap.ymp from every scene archive (dtk vfs cp), decodes it
big-endian in Python, and compares with what the real setDataAddress builds
(layer fields, height map offset and size, first/last height map byte).
"""
import argparse
import math
import struct
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import build_port  # noqa: E402

ROOT = build_port.ROOT
SCENES = Path("<project>/dolphin_test/files/data/scene")
SRCS = ["port/tools/test_ymp.cpp", "src/Map/PollutionManager.cpp",
        "libs/JSystem/src/JSupport/JSUList.cpp"]


def extract(dtk, data):
    for p in sorted(SCENES.glob("*.szs")):
        dst = data / (p.stem + ".ymp")
        if not dst.exists():
            subprocess.run([dtk, "vfs", "cp", f"{p}:map/ymap.ymp", str(dst)], cwd=ROOT,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def g(v): return "%.9g" % v


def ref(b):
    n, _pad, off = struct.unpack_from(">HHI", b, 0)
    L = [f"layers {n}"]
    for i in range(n):
        o = off + i * 0x2C
        t, fl, pl, _ = struct.unpack_from(">4H", b, o)
        f = struct.unpack_from(">6f", b, o + 8)
        w, h, u24, hm = struct.unpack_from(">HHII", b, o + 0x20)
        size = 1 << (w + h)
        L.append("layer %d %d %d %s %d %d %d %d %d %d %d" % (
            t, fl, pl, " ".join(g(x) for x in f), w, h, u24, hm, size, b[hm], b[hm + size - 1]))
    return L


def same(x, y):
    xs, ys = x.split(), y.split()
    if len(xs) != len(ys):
        return False
    for a, c in zip(xs, ys):
        if a != c:
            try:
                if not math.isclose(float(a), float(c), rel_tol=1e-7, abs_tol=1e-7):
                    return False
            except ValueError:
                return False
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--target", default="x86_64-pc-windows-msvc")
    a = ap.parse_args()
    out = ROOT / "port" / "build" / "test_ymp" / a.target
    out.mkdir(parents=True, exist_ok=True)
    data = ROOT / "port" / "build" / "test_ymp" / "data"
    data.mkdir(parents=True, exist_ok=True)
    extract(str(ROOT / "build" / "tools" / "dtk.exe"), data)
    files = sorted(data.glob("*.ymp"))
    if not files:
        sys.exit("no .ymp extracted")
    exp = {str(p): ref(p.read_bytes()) for p in files}
    (out / "files.txt").write_text("\n".join(str(p) for p in files) + "\n", encoding="utf-8")

    objs = []
    for s in SRCS:
        fl = [f if not f.startswith("--target=") else f"--target={a.target}"
              for f in build_port.flags(False)]
        obj = out / (Path(s).stem + ".obj")
        r = subprocess.run([build_port.CLANG, *fl, s, "-o", str(obj)], cwd=ROOT)
        if r.returncode:
            sys.exit(f"compile failed: {s}")
        objs.append(str(obj))
    exe = out / "test_ymp.exe"
    r = subprocess.run([build_port.CLANG, f"--target={a.target}", "-fuse-ld=lld", "-g", *objs,
                        "-o", str(exe), "-Wl,/force:unresolved"],
                       cwd=ROOT, capture_output=True, text=True)
    if not exe.exists():
        print(r.stdout + r.stderr)
        sys.exit("link failed")
    r = subprocess.run([str(exe), str(out / "files.txt")], cwd=ROOT, capture_output=True,
                       text=True, errors="replace")
    (out / "actual.txt").write_text(r.stdout, encoding="utf-8")
    act, cur = {}, None
    for ln in r.stdout.splitlines():
        if ln.startswith("== "):
            cur = ln[3:]
            act[cur] = []
        elif ln.startswith(("FAIL", "SUMMARY")):
            print(ln)
            cur = None
        elif cur is not None:
            act[cur].append(ln)
    bad = layers = 0
    for f, el in exp.items():
        al = act.get(f)
        layers += len(el) - 1
        if al is None:
            print(f"MISSING output for {f}")
            bad += 1
        elif len(al) != len(el) or not all(same(x, y) for x, y in zip(el, al)):
            bad += 1
            if bad <= 3:
                print(f"MISMATCH {f}")
                for x, y in zip(el, al):
                    if not same(x, y):
                        print(f"  exp: {x}\n  got: {y}")
                        break
    print(f"{len(files)} .ymp files, {layers} layers, {bad} mismatching file(s), exit {r.returncode}")
    sys.exit(1 if bad or r.returncode else 0)


if __name__ == "__main__":
    main()

