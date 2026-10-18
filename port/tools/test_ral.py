"""Host test for the .ral rail graph swap pass (port_swap_ral).

    python port/tools/test_ral.py [--target x86_64-pc-windows-msvc]

1. Extracts map/scene.ral from every scene archive in dolphin_test/files/data/scene
   with `dtk vfs cp` (once).
2. Decodes each file big-endian in Python (graph descriptors, names, 0x44-byte
   rail nodes) and applies TGraphWeb::translateNodes' connection filtering.
3. Builds port/tools/test_ral.cpp with the real src/Enemy/graph.cpp and
   spline.cpp, builds a TGraphGroup from the raw bytes (twice) and compares
   every graph name, node count and rail node.
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
SRCS = ["port/tools/test_ral.cpp", "src/Enemy/graph.cpp", "src/Enemy/spline.cpp"]


def extract(dtk, data):
    n = 0
    for p in sorted(SCENES.glob("*.szs")):
        dst = data / (p.stem + ".ral")
        if dst.exists():
            n += 1
            continue
        r = subprocess.run([dtk, "vfs", "cp", f"{p}:map/scene.ral", str(dst)], cwd=ROOT,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if r.returncode == 0 and dst.exists():
            n += 1
    return n


def ref(b):
    L, descs, o = [], [], 0
    while True:
        n, no, ro = struct.unpack_from(">iII", b, o)
        if n == 0:
            break
        descs.append((n, no, ro))
        o += 12
    for n, no, ro in descs:
        name = b[no:b.index(0, no)].decode("ascii")
        nodes = []
        for i in range(n):
            f = struct.unpack_from(">3hhI4H8H8f", b, ro + i * 0x44)
            nodes.append({"pos": f[0:3], "cn": f[3], "flags": f[4], "pyr": f[5:9],
                          "con": list(f[9:17]), "per": list(f[17:25])})
        # TGraphWeb::translateNodes (edits the nodes in order, flags tested on the original array)
        for i in range(n):
            nd = nodes[i]
            if nd["flags"] & 0x8:
                keep = [j for j in range(nd["cn"]) if not (nodes[nd["con"][j]]["flags"] & 0x4)]
                nd["con"][:len(keep)] = [nd["con"][j] for j in keep]
                nd["per"][:len(keep)] = [nd["per"][j] for j in keep]
                nd["cn"] = len(keep)
            if nd["flags"] & 0x200:
                keep = [j for j in range(nd["cn"]) if nd["con"][j] >= i]
                nd["con"][:len(keep)] = [nd["con"][j] for j in keep]
                nd["per"][:len(keep)] = [nd["per"][j] for j in keep]
                nd["cn"] = len(keep)
        L.append(f"graph {name} {n}")
        for nd in nodes:
            L.append("node %d %d %d %d %d %d %d %d %d " % (*nd["pos"], nd["cn"], nd["flags"],
                                                           *nd["pyr"])
                     + " ".join(str(c) for c in nd["con"]) + " "
                     + " ".join("%.9g" % p for p in nd["per"]))
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
        if math.isnan(fa) and math.isnan(fc):
            continue
        if not math.isclose(fa, fc, rel_tol=1e-6, abs_tol=1e-6):
            return False
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--target", default="x86_64-pc-windows-msvc")
    a = ap.parse_args()
    out = ROOT / "port" / "build" / "test_ral" / a.target
    out.mkdir(parents=True, exist_ok=True)
    data = ROOT / "port" / "build" / "test_ral" / "data"
    data.mkdir(parents=True, exist_ok=True)
    got = extract(str(ROOT / "build" / "tools" / "dtk.exe"), data)
    files = sorted(data.glob("*.ral"))
    if not files:
        sys.exit("no .ral extracted")
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
    exe = out / "test_ral.exe"
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
    bad = nodes = 0
    for f, el in exp.items():
        al = act.get(f)
        nodes += sum(1 for x in el if x.startswith("node"))
        if al is None:
            print(f"MISSING output for {f}")
            bad += 1
        elif len(al) != len(el) or not all(same(x, y) for x, y in zip(el, al)):
            bad += 1
            if bad <= 3:
                print(f"MISMATCH {f} ({len(el)} vs {len(al)} lines)")
                for x, y in zip(el, al):
                    if not same(x, y):
                        print(f"  exp: {x}\n  got: {y}")
                        break
    print(f"{len(files)} .ral files, {nodes} rail nodes, {bad} mismatching file(s), "
          f"exit {r.returncode}")
    sys.exit(1 if bad or r.returncode else 0)


if __name__ == "__main__":
    main()
