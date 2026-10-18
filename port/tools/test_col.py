"""Host test for the collision (.col) swap pass and the 64-bit group loader.

    python port/tools/test_col.py [--target x86_64-pc-windows-msvc]

1. Extracts the dolpic0 / airport0 / bianco0 scene archives (and common) from
   dolphin_test/files with `dtk vfs cp` (once) and collects every .col file.
2. Reads each file big-endian in Python (header, groups, vertices, triangles)
   and writes the expected triangle list.
3. Builds port/tools/test_col.cpp with the real MapCollisionEntry.cpp,
   MapMakeData.cpp, MapMakeList.cpp, MapCollisionData.cpp, MapArea.cpp,
   MapData.cpp and JSUMemoryStream/JSUInputStream (build_port.py flags), loads
   each file twice through TMapCollisionStatic::init, and compares what the
   real code built (TBGCheckData: bgType, data, sound material, vertices,
   min/max Y) with the Python values.
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
FILES = Path("<project>/dolphin_test/files/data")
ARCHIVES = ["common", "mario", "scene/dolpic0", "scene/bianco0", "scene/airport0",
            "scene/mamma0", "scene/ricco0"]
SRCS = ["port/tools/test_col.cpp",
        "src/Map/MapCollisionEntry.cpp", "src/Map/MapMakeData.cpp", "src/Map/MapMakeList.cpp",
        "src/Map/MapCollisionData.cpp", "src/Map/MapArea.cpp", "src/Map/MapData.cpp",
        "libs/JSystem/src/JSupport/JSUMemoryStream.cpp", "libs/JSystem/src/JSupport/JSUList.cpp",
        "libs/JSystem/src/JSupport/JSUInputStream.cpp"]


def extract(dtk, data):
    for a in ARCHIVES:
        dst = data / Path(a).name
        if dst.exists():
            continue
        r = subprocess.run([dtk, "vfs", "cp", f"{FILES / (a + '.szs')}:", str(dst)],
                           cwd=ROOT, stdout=subprocess.DEVNULL)
        if r.returncode:
            sys.exit(f"dtk vfs cp failed for {a}.szs")


def f32(x):
    return struct.unpack(">f", struct.pack(">f", x))[0]


def fmt(v):
    return f"{v:.9g}"


def col_ref(b):
    """Expected output lines for one .col file, read big-endian."""
    vn, vo, gn, go = struct.unpack_from(">4I", b, 0)
    verts = [struct.unpack_from(">3f", b, vo + i * 12) for i in range(vn)]
    L = [f"counts {vn} {gn}"]
    tris = []
    for g in range(gn):
        o = go + g * 0x18
        bg, tn, fl, pad = struct.unpack_from(">HhHH", b, o)
        io, so, uo, ao = struct.unpack_from(">4I", b, o + 8)
        assert pad == 0xFFFF and fl in (0, 1), (pad, fl)
        for j in range(tn):
            idx = struct.unpack_from(">3h", b, io + j * 6)
            data = struct.unpack_from(">h", b, ao + j * 2)[0] if fl & 1 else -9999
            tris.append((bg, data, b[so + j], b[uo + j], [verts[i] for i in idx]))
    L.append(f"pool {len(tris)}")
    for bg, data, snd, u7, p in tris:
        ys = [v[1] for v in p]
        # TBGCheckData::setVertex leaves minY/maxY at 0 for a degenerate triangle (zero normal)
        a, b2, c = p
        n = (
            (b2[1] - a[1]) * (c[2] - b2[2]) - (b2[2] - a[2]) * (c[1] - b2[1]),
            (b2[2] - a[2]) * (c[0] - b2[0]) - (b2[0] - a[0]) * (c[2] - b2[2]),
            (b2[0] - a[0]) * (c[1] - b2[1]) - (b2[1] - a[1]) * (c[0] - b2[0]))
        if n == (0.0, 0.0, 0.0):
            ys = [0.0]
        L.append("t %d %d %d %d %s %s %s %s %s" % (
            bg, data, snd, u7, " ".join(fmt(c) for c in p[0]), " ".join(fmt(c) for c in p[1]),
            " ".join(fmt(c) for c in p[2]), fmt(min(ys)), fmt(max(ys))))
    return L


def same(x, y):
    xs, ys = x.split(), y.split()
    if len(xs) != len(ys):
        return False
    for a, c in zip(xs, ys):
        if a == c:
            continue
        try:
            fa, fc = float(a), float(c)
        except ValueError:
            return False
        if not math.isclose(fa, fc, rel_tol=1e-6, abs_tol=1e-4):
            return False
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--target", default="x86_64-pc-windows-msvc")
    a = ap.parse_args()
    out = ROOT / "port" / "build" / "test_col" / a.target
    out.mkdir(parents=True, exist_ok=True)
    data = ROOT / "port" / "build" / "test_col" / "data"
    extract(str(ROOT / "build" / "tools" / "dtk.exe"), data)

    files = sorted(data.rglob("*.col"))
    exp = {}
    for p in files:
        exp[str(p)] = col_ref(p.read_bytes())
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
    exe = out / "test_col.exe"
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
    bad = 0
    ntri = 0
    for f, el in exp.items():
        al = act.get(f)
        ntri += len(el) - 2
        if al is None:
            print(f"MISSING output for {f}")
            bad += 1
        elif len(al) != len(el) or not all(same(x, y) for x, y in zip(el, al)):
            bad += 1
            if bad <= 5:
                print(f"MISMATCH {f} ({len(el)} vs {len(al)} lines)")
                for x, y in zip(el, al):
                    if not same(x, y):
                        print(f"  exp: {x}\n  got: {y}")
                        break
    print(f"{len(files)} .col files, {ntri} triangles, {bad} mismatching file(s), "
          f"exit {r.returncode}")
    sys.exit(1 if bad or r.returncode else 0)


if __name__ == "__main__":
    main()

