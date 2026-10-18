"""Host test for the .pad swap (port/include/port_asset_swap.h) and the .bcr
swap in Koga::ToolData::Attach (src/MarioUtil/ToolData.cpp).

    python port/tools/test_assets.py [--target x86_64-pc-windows-msvc]

1. Extracts every .pad (map/map/pad*/ in all scene archives) and every .bcr
   (subtitle.szs rnbl/) from dolphin_test/files with `dtk vfs cp` (once).
2. Plays every replay big-endian in Python (TRecordValueManager::get semantics)
   and hashes each frame's outputs; decodes every BCSV entry.
3. Builds port/tools/test_assets.cpp with the real src/Player/MarioRecord.cpp
   and src/MarioUtil/ToolData.cpp, runs it on the raw files and compares.
"""
import argparse
import struct
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import build_port  # noqa: E402
import disc_inventory  # noqa: E402

ROOT = build_port.ROOT
DATA = disc_inventory.DISC / "data"
SRCS = ["port/tools/test_assets.cpp", "src/Player/MarioRecord.cpp",
        "src/MarioUtil/ToolData.cpp"]


def extract(dtk, out):
    out.mkdir(parents=True, exist_ok=True)
    if any(out.rglob("*.pad")):
        return
    for arc in sorted((DATA / "scene").glob("*.szs")):
        for name, _ in disc_inventory.ls(arc):
            if name.endswith((".pad", ".bin")) and "/pad" in name:
                dst = out / arc.stem / name.replace("/", "_")
                dst.parent.mkdir(parents=True, exist_ok=True)
                subprocess.run([dtk, "vfs", "cp", f"{arc}:{name}", str(dst)],
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    sub = DATA / "subtitle.szs"
    for name, _ in disc_inventory.ls(sub):
        if name.endswith(".bcr"):
            dst = out / "subtitle" / name.replace("/", "_")
            dst.parent.mkdir(parents=True, exist_ok=True)
            subprocess.run([dtk, "vfs", "cp", f"{sub}:{name}", str(dst)],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def fnv(h, data):
    for c in data:
        h = ((h ^ c) * 16777619) & 0xFFFFFFFF
    return h


def ref_pad(path, b):
    length = struct.unpack_from(">i", b, 0x10)[0]
    offs = struct.unpack_from(">10i", b, 0x14)
    fmts = [">f", ">h", ">H", ">B", ">B"]
    sizes = [4, 2, 2, 1, 1]
    chans = []
    for c in range(5):
        chans.append({"d": offs[2 * c], "v": offs[2 * c + 1], "i": 0, "e": 0})

    def get(c):
        ch = chans[c]
        val = struct.unpack_from(fmts[c], b, ch["v"] + ch["i"] * sizes[c])[0]
        ch["e"] += 1
        if ch["e"] >= struct.unpack_from(">I", b, ch["d"] + ch["i"] * 4)[0]:
            ch["i"] += 1
            ch["e"] = 0
        return val
    h, prev = 2166136261, 0
    for _ in range(length):
        mag, yaw, btn, p5, p6 = (get(c) for c in range(5))
        just = btn & (btn ^ prev)
        prev = btn
        h = fnv(h, struct.pack("<fhIIBB", mag, yaw, btn, just, p5, p6))
    return [f"pad {path} {max(length, 0)} {h:08x}"]


def ref_bcr(path, b):
    n, nf, do, es = struct.unpack_from(">iiiI", b, 0)
    items = {}
    for i in range(nf):
        hsh, _, off, _, typ = struct.unpack_from(">IIHBB", b, 16 + 12 * i)
        items[hsh] = (off, typ)

    def key(s):
        h = 0
        for ch in s.encode():
            h = ((ch + (h << 8)) & 0xFFFFFFFF) % 0x1FFFFD9  # u32 arithmetic
        return h
    out = [f"bcr {path} {n}"]
    for e in range(n):
        base = do + e * es
        st = struct.unpack_from(">i", b, base + items[key("start_frame")][0])[0]
        en = struct.unpack_from(">i", b, base + items[key("end_frame")][0])[0]
        to = base + items[key("type")][0]
        typ = b[to:b.index(0, to)].decode("ascii")
        out.append(f"  1 {st} {en} 1 {typ}")
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--target", default="x86_64-pc-windows-msvc")
    a = ap.parse_args()
    base = ROOT / "port" / "build" / "test_assets"
    out = base / a.target
    out.mkdir(parents=True, exist_ok=True)
    extract(str(ROOT / "build" / "tools" / "dtk.exe"), base / "data")
    files = sorted(p for p in (base / "data").rglob("*") if p.suffix in (".pad", ".bcr"))
    if not files:
        sys.exit("nothing extracted")
    exp = []
    for p in files:
        b = p.read_bytes()
        exp += ref_pad(str(p), b) if p.suffix == ".pad" else ref_bcr(str(p), b)
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
    exe = out / "test_assets.exe"
    r = subprocess.run([build_port.CLANG, f"--target={a.target}", "-fuse-ld=lld", "-g", *objs,
                        "-o", str(exe), "-Wl,/force:unresolved"], cwd=ROOT, capture_output=True,
                       text=True)
    if not exe.exists():
        print(r.stdout + r.stderr)
        sys.exit("link failed")
    r = subprocess.run([str(exe), str(out / "files.txt")], cwd=ROOT, capture_output=True,
                       text=True, errors="replace")
    act = r.stdout.splitlines()
    (out / "actual.txt").write_text(r.stdout, encoding="utf-8")
    (out / "expected.txt").write_text("\n".join(exp) + "\n", encoding="utf-8")
    bad = sum(1 for x, y in zip(exp, act) if x != y) + abs(len(exp) - len(act))
    for x, y in zip(exp, act):
        if x != y:
            print(f"  exp: {x}\n  got: {y}")
            break
    npad = sum(1 for p in files if p.suffix == ".pad")
    print(f"{npad} .pad files ({len({p.parent for p in files if p.suffix == '.pad'})} scenes), "
          f"{len(files) - npad} .bcr files, {bad} mismatching line(s), exit {r.returncode}")
    sys.exit(1 if bad or r.returncode else 0)


if __name__ == "__main__":
    main()
