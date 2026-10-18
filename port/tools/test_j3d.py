"""Host test for the J3D model / animation swap passes (port/include/port_j3d_swap.h).

    python port/tools/test_j3d.py                 # x86_64 (default), build + run
    python port/tools/test_j3d.py --limit 50      # only the first 50 files of each kind

1. Extracts mario/common/yoshi/title and a few scene archives from
   dolphin_test/files with `dtk vfs cp` into port/build/test_j3d/data (once).
2. Writes the expected values of every .bmd/.bmt and .bck/.btk/.brk/.bpk/.btp
   file, read big-endian straight from the file (models through
   level_export/tools/bmd2obj.py, the reference BMD reader), to expected.txt.
3. Builds port/tools/test_j3d.cpp with the real J3DJointFactory.cpp,
   J3DShapeFactory.cpp, J3DMaterialFactory.cpp, J3DAnmLoader.cpp and
   JUTNameTab.cpp (build_port.py flags, x86_64), runs it, and compares its
   output (values read through the native structs after the swap pass, and
   through the real factories / anim loader) with expected.txt.
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
BMD2OBJ = Path("<project>/level_export/tools")
ARCHIVES = ["mario", "common", "yoshi", "title", "scene/dolpic0", "scene/bianco0",
            "scene/airport0", "scene/mamma0"]
SRCS = ["port/tools/test_j3d.cpp",
        "libs/JSystem/src/J3D/J3DGraphLoader/J3DJointFactory.cpp",
        "libs/JSystem/src/J3D/J3DGraphLoader/J3DShapeFactory.cpp",
        "libs/JSystem/src/J3D/J3DGraphLoader/J3DMaterialFactory.cpp",
        "libs/JSystem/src/J3D/J3DGraphLoader/J3DAnmLoader.cpp",
        "libs/JSystem/src/J3D/J3DGraphAnimator/J3DAnimation.cpp",
        "libs/JSystem/src/JUtility/JUTNameTab.cpp"]
MODEL_EXT = (".bmd", ".bdl", ".bmt")
ANM_EXT = (".bck", ".btk", ".brk", ".bpk", ".btp")

sys.path.insert(0, str(BMD2OBJ))
import bmd2obj  # noqa: E402


def u8(b, o): return b[o]
def s8(b, o): return struct.unpack_from(">b", b, o)[0]
def u16(b, o): return struct.unpack_from(">H", b, o)[0]
def s16(b, o): return struct.unpack_from(">h", b, o)[0]
def u32(b, o): return struct.unpack_from(">I", b, o)[0]
def f32(b, o): return struct.unpack_from(">f", b, o)[0]


def ntab(b, o):
    n = u16(b, o)
    out = []
    for i in range(n):
        off = u16(b, o + 4 + i * 4 + 2)
        e = b.index(0, o + off)
        out.append(esc(b[o + off:e]))
    return out


def esc(raw):  # same escaping as test_j3d.cpp's printName
    s = "".join(chr(c) if 0x21 <= c < 0x7F else "\\x%02x" % c for c in raw)
    return s or "_"


def blocks(b):
    sec, o = {}, 0x20
    for _ in range(u32(b, 0xC)):
        if o + 8 > len(b):
            break  # some .bmt files declare more blocks than they hold
        sec[b[o:o + 4].decode()] = o
        o += u32(b, o + 4)
    return sec


# ------------------------------------------------------------------ models
def model_ref(b):
    sec = blocks(b)
    m = bmd2obj.BMD(b) if b[4:8] != b"bmt3" else None
    L = []
    if "INF1" in sec:
        o = sec["INF1"]
        h = o + u32(b, o + 0x14)
        cnt = hs = 0
        while True:
            t, v = u16(b, h), u16(b, h + 2)
            h += 4
            cnt += 1
            hs += t + v
            if t == 0:
                break
        L.append(["inf1", u16(b, o + 8), u32(b, o + 0xC), u32(b, o + 0x10), cnt, hs])
    if "VTX1" in sec:
        o = sec["VTX1"]
        size = u32(b, o + 4)
        offs = [u32(b, o + 0xC + i * 4) for i in range(13)]
        nz = sorted(x for x in offs if x)
        fo = o + u32(b, o + 8)
        fmts = []
        while u32(b, fo) != 0xFF:
            fmts.append((u32(b, fo), u32(b, fo + 4), u32(b, fo + 8), b[fo + 12]))
            fo += 16
        L.append(["vtxfmt", len(fmts)] + [x for f in fmts for x in f])
        slot = {9: 0, 10: 1, 11: 3, 12: 4}
        for t in range(8):
            slot[13 + t] = 5 + t
        for (a, cnt, ty, frac) in fmts:
            so = offs[slot[a]]
            nxt = [x for x in nz if x > so]
            end = nxt[0] if nxt else size
            if a in (11, 12):
                esz = {0: 2, 1: 3, 2: 4, 3: 2, 4: 3, 5: 4}[ty]
                n = (end - so) // esz
                s = 0
                for k in range(n):
                    q = o + so + k * esz
                    if ty in (0, 3):
                        s += u16(b, q)
                    elif ty == 4:
                        s += (b[q] << 16) | (b[q + 1] << 8) | b[q + 2]
                    else:
                        s += sum(b[q:q + esz])
                L.append(["arr", a, n, s])
            else:
                arr = m.arrays.get(a, [])
                L.append(["arr", a, len(arr), sum(sum(t) for t in arr)])
    if "EVP1" in sec:
        o = sec["EVP1"]
        env = m.envelopes
        L.append(["evp1", u16(b, o + 8), sum(len(e) for e in env),
                  sum(j for e in env for j, _ in e), sum(w for e in env for _, w in e),
                  len(m.inv_bind), sum(x for mt in m.inv_bind for r in mt for x in r)])
    if "DRW1" in sec:
        drw = m.drw
        full = next((i for i, (f, _) in enumerate(drw) if f == 1), len(drw))
        L.append(["drw1", len(drw), sum(f for f, _ in drw), sum(i for _, i in drw), full])
    if "JNT1" in sec:
        o = sec["JNT1"]
        n = u16(b, o + 8)
        eo, ro = o + u32(b, o + 0xC), o + u32(b, o + 0x10)
        L.append(["jnt1", n])
        for i in range(n):
            q = eo + u16(b, ro + i * 2) * 0x40
            L.append(["jnt", i, u16(b, q), b[q + 2]] + [f32(b, q + k) for k in (4, 8, 12)]
                     + [s16(b, q + k) for k in (16, 18, 20)]
                     + [f32(b, q + k) for k in range(24, 64, 4)])
    if "MAT3" in sec:
        o = sec["MAT3"]
        n = u16(b, o + 8)
        offs = [u32(b, o + 0xC + i * 4) for i in range(30)]
        remap = [u16(b, o + offs[1] + i * 2) for i in range(n)]
        uniq = next((i for i in range(n) if remap[i] < i), n)
        names = ntab(b, o + offs[2]) if offs[2] else ["_"] * n
        L.append(["mat3", n, uniq])
        has_ind = offs[3] != 0 and ((offs[3] - offs[2]) & 0xFFFFFFFF) > 4  # u32 maths as in the factory
        for i in range(n):
            e = o + offs[0] + remap[i] * 0x14C
            ti = u16(b, e + 0x84)
            tex = 0xFFFF if ti == 0xFFFF else u16(b, o + offs[15] + ti * 2)
            if m is not None:
                assert (m.mat_tex[i] if m.mat_tex[i] is not None else 0xFFFF) == tex
            ci = b[e + 1]
            cull = 0xFF if ci == 0xFF else u32(b, o + offs[4] + ci * 4) & 0xFF
            si = b[e + 4]
            tsn = 0xFF if si == 0xFF else b[o + offs[19] + si]
            cc = b[e + 2]
            ccn = 0 if cc == 0xFF else b[o + offs[6] + cc]
            tci = u16(b, e + 0xDC)
            tevc = [0, 0, 0, 0] if tci == 0xFFFF else [s16(b, o + offs[17] + tci * 8 + k * 2) for k in range(4)]
            mci = u16(b, e + 8)
            matc = [255] * 4 if mci == 0xFFFF else list(b[o + offs[5] + mci * 4:o + offs[5] + mci * 4 + 4])
            tg = b[e + 3]
            tgn = 0 if tg == 0xFF else b[o + offs[10] + tg]
            t0 = u16(b, e + 0x28)
            tc0 = [-1] * 3 if t0 == 0xFFFF else list(b[o + offs[11] + t0 * 4:o + offs[11] + t0 * 4 + 3])
            r0 = u16(b, e + 0xBC)
            to0 = [-1] * 3 if r0 == 0xFFFF else list(b[o + offs[16] + r0 * 4:o + offs[16] + r0 * 4 + 3])
            L.append(["mat", i, names[i] if i < len(names) else "_", tex, cull, tsn, ccn, b[e + 0x9C]]
                     + tevc + matc + [tgn] + tc0 + to0)
            mi = u16(b, e + 0x48)
            if mi != 0xFFFF:
                q = o + offs[13] + mi * 0x64
                L.append(["texmtx", i, b[q], b[q + 1]] + [f32(b, q + k) for k in (4, 8, 12, 0x10, 0x14)]
                         + [s16(b, q + 0x18), f32(b, q + 0x1C), f32(b, q + 0x20), f32(b, q + 0x24),
                            f32(b, q + 0x50)])
            fi = u16(b, e + 0x144)
            if fi != 0xFFFF:
                q = o + offs[23] + fi * 0x2C
                L.append(["fog", i, b[q], b[q + 1], u16(b, q + 2)] + [f32(b, q + k) for k in (4, 8, 12, 16)]
                         + list(b[q + 0x14:q + 0x18]) + [u16(b, q + 0x18), u16(b, q + 0x2A)])
            ni = u16(b, e + 0x14A)
            if ni != 0xFFFF:
                q = o + offs[29] + ni * 0x10
                L.append(["nbt", i, b[q]] + [f32(b, q + k) for k in (4, 8, 12)])
            bi = u16(b, e + 0x148)
            if bi != 0xFFFF:
                q = o + offs[25] + bi * 4
                L.append(["blend", i] + list(b[q:q + 4]))
            li = u16(b, e + 0x18)
            if li != 0xFFFF and offs[9]:
                q = o + offs[9] + li * 0x34
                L.append(["light", i, f32(b, q), f32(b, q + 0x14)] + list(b[q + 0x18:q + 0x1C])
                         + [f32(b, q + 0x30)])
            if has_ind:
                q = o + offs[3] + i * 0x138
                L.append(["ind", i, b[q], b[q + 1], f32(b, q + 0x14), s8(b, q + 0x2C), b[q + 0x78]])
    if "SHP1" in sec:
        o = sec["SHP1"]
        n = u16(b, o + 8)
        so, ro = o + u32(b, o + 0xC), o + u32(b, o + 0x10)
        pdo, mdo, plo = u32(b, o + 0x20), o + u32(b, o + 0x24), o + u32(b, o + 0x28)
        L.append(["shp1", n])
        for i in range(n):
            q = so + u16(b, ro + i * 2) * 0x28
            groups = u16(b, q + 2)
            attrs = m.shapes[i][0]
            L.append(["shp", i, b[q], groups] + [f32(b, q + k) for k in range(0xC, 0x28, 4)]
                     + [len(attrs)] + [x for a in attrs for x in a])
            for k in range(groups):
                md = mdo + (u16(b, q + 6) + k) * 8
                pl = plo + (u16(b, q + 8) + k) * 8
                dl = pdo + u32(b, pl + 4)
                L.append(["pkt", i, k, u16(b, md), u16(b, md + 2), u32(b, md + 4), u32(b, pl), dl,
                          b[o + dl]])
    if "TEX1" in sec:
        o = sec["TEX1"]
        n = u16(b, o + 8)
        names = ntab(b, o + u32(b, o + 0x10)) if u32(b, o + 0x10) else ["_"] * n
        ho = o + u32(b, o + 0xC)
        L.append(["tex1", n])
        for i in range(n):
            h = ho + i * 0x20
            L.append(["tex", i, names[i], b[h], u16(b, h + 2), u16(b, h + 4), u16(b, h + 10),
                      b[h + 0x18], u32(b, h + 0x1C)])
    L.append(["conflicts", 0])
    return L


# -------------------------------------------------------------- animations
def keysum(b, o, n):
    return sum(u16(b, o + k * 2) for k in range(n))


def anm_ref(b):
    sec = blocks(b)
    L = []
    if "ANK1" in sec:
        o = sec["ANK1"]
        nj, cs, cr, ct = (u16(b, o + k) for k in (0xC, 0xE, 0x10, 0x12))
        L.append(["ank1", b[o + 8], b[o + 9], s16(b, o + 0xA), nj, cs, cr, ct,
                  keysum(b, o + u32(b, o + 0x14), nj * 27),
                  sum(f32(b, o + u32(b, o + 0x18) + k * 4) for k in range(cs)),
                  sum(s16(b, o + u32(b, o + 0x1C) + k * 2) for k in range(cr)),
                  sum(f32(b, o + u32(b, o + 0x20) + k * 4) for k in range(ct))])
    if "TTK1" in sec:
        o = sec["TTK1"]
        tr, cs, cr, ct = (u16(b, o + k) for k in (0xC, 0xE, 0x10, 0x12))
        nm = tr // 3
        names = ntab(b, o + u32(b, o + 0x1C))
        L.append(["ttk1", b[o + 8], b[o + 9], s16(b, o + 0xA), tr, cs, cr, ct,
                  keysum(b, o + u32(b, o + 0x14), tr * 9),
                  keysum(b, o + u32(b, o + 0x18), nm),
                  sum(b[o + u32(b, o + 0x20) + k] for k in range(nm)),
                  sum(f32(b, o + u32(b, o + 0x24) + k * 4) for k in range(nm * 3)),
                  sum(f32(b, o + u32(b, o + 0x28) + k * 4) for k in range(cs)),
                  sum(s16(b, o + u32(b, o + 0x2C) + k * 2) for k in range(cr)),
                  sum(f32(b, o + u32(b, o + 0x30) + k * 4) for k in range(ct)),
                  u16(b, o + 0x34), names[0] if names else "_"])
    if "TRK1" in sec:
        o = sec["TRK1"]
        cn, kn = u16(b, o + 0xC), u16(b, o + 0xE)
        cnt = [u16(b, o + 0x10 + k * 2) for k in range(8)]
        ct, kt = o + u32(b, o + 0x20), o + u32(b, o + 0x24)
        vals = [sum(s16(b, o + u32(b, o + 0x38 + j * 4) + k * 2) for k in range(cnt[j])) for j in range(8)]
        cnames = ntab(b, o + u32(b, o + 0x30))
        knames = ntab(b, o + u32(b, o + 0x34))
        L.append(["trk1", b[o + 8], s16(b, o + 0xA), cn, kn] + cnt
                 + [sum(keysum(b, ct + i * 0x1C, 12) + b[ct + i * 0x1C + 0x18] for i in range(cn)),
                    sum(keysum(b, kt + i * 0x1C, 12) + b[kt + i * 0x1C + 0x18] for i in range(kn)),
                    keysum(b, o + u32(b, o + 0x28), cn), keysum(b, o + u32(b, o + 0x2C), kn)]
                 + vals + [cnames[0] if cnames else "_", knames[0] if knames else "_"])
    if "PAK1" in sec:
        o = sec["PAK1"]
        mn = u16(b, o + 0xE)
        cnt = [u16(b, o + 0x10 + k * 2) for k in range(4)]
        names = ntab(b, o + u32(b, o + 0x20))
        L.append(["pak1", b[o + 8], s16(b, o + 0xC), mn] + cnt
                 + [keysum(b, o + u32(b, o + 0x18), mn * 12), keysum(b, o + u32(b, o + 0x1C), mn)]
                 + [sum(s16(b, o + u32(b, o + 0x24 + j * 4) + k * 2) for k in range(cnt[j])) for j in range(4)]
                 + [names[0] if names else "_"])
    if "TPT1" in sec:
        o = sec["TPT1"]
        mn = u16(b, o + 0xC)
        to = o + u32(b, o + 0x10)
        nvals = max([u16(b, to + i * 8) + u16(b, to + i * 8 + 2) for i in range(mn)] + [0])
        names = ntab(b, o + u32(b, o + 0x1C))
        L.append(["tpt1", b[o + 8], s16(b, o + 0xA), mn, u16(b, o + 0xE),
                  sum(u16(b, to + i * 8) + u16(b, to + i * 8 + 2) + u16(b, to + i * 8 + 6) for i in range(mn)),
                  sum(b[to + i * 8 + 4] for i in range(mn)), nvals,
                  keysum(b, o + u32(b, o + 0x14), nvals), keysum(b, o + u32(b, o + 0x18), mn),
                  names[0] if names else "_"])
    L.append(["badkeys", 0])
    return L


def fmt(v):
    if isinstance(v, float):
        return "%.9g" % v
    return str(v)


def extract(dtk, data):
    for a in ARCHIVES:
        dst = data / Path(a).name
        if dst.exists():
            continue
        r = subprocess.run([dtk, "vfs", "cp", f"{FILES / (a + '.szs')}:", str(dst)], cwd=ROOT,
                           stdout=subprocess.DEVNULL)
        if r.returncode:
            sys.exit(f"dtk vfs cp failed for {a}.szs")


def same(a, b):
    ta, tb = a.split(), b.split()
    if len(ta) != len(tb):
        return False
    for x, y in zip(ta, tb):
        if x == y:
            continue
        try:
            fx, fy = float(x), float(y)
        except ValueError:
            return False
        if not math.isclose(fx, fy, rel_tol=1e-5, abs_tol=1e-3):
            return False
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--target", default="x86_64-pc-windows-msvc")
    ap.add_argument("--limit", type=int, default=0)
    a = ap.parse_args()
    out = ROOT / "port" / "build" / "test_j3d" / a.target
    out.mkdir(parents=True, exist_ok=True)
    data = ROOT / "port" / "build" / "test_j3d" / "data"
    dtk = str(ROOT / "build" / "tools" / "dtk.exe")
    extract(dtk, data)

    files = sorted(p for p in data.rglob("*") if p.suffix.lower() in MODEL_EXT + ANM_EXT)
    models = [p for p in files if p.suffix.lower() in MODEL_EXT]
    anms = [p for p in files if p.suffix.lower() in ANM_EXT]
    if a.limit:
        models, anms = models[:a.limit], anms[:a.limit]
    exp, lst, skipped = [], [], []
    for p in models + anms:
        b = p.read_bytes()
        rel = p.relative_to(data).as_posix()
        try:
            lines = model_ref(b) if p.suffix.lower() in MODEL_EXT else anm_ref(b)
        except Exception as e:  # reference reader cannot parse it: test the swap only
            skipped.append(f"{rel}: {type(e).__name__} {e}")
            lines = None
        kind = "M" if p.suffix.lower() in MODEL_EXT else "A"
        lst.append(f"{kind} {p}")
        exp.append(f"== {rel}")
        if lines is None:
            exp.append("skip")
        else:
            exp.extend(" ".join(fmt(v) for v in l) for l in lines)
    (out / "files.txt").write_text("\n".join(lst) + "\n", encoding="utf-8")
    (out / "expected.txt").write_text("\n".join(exp) + "\n", encoding="utf-8")

    objs = []
    for s in SRCS:
        fl = [f if not f.startswith("--target=") else f"--target={a.target}"
              for f in build_port.flags(False)]
        obj = out / (Path(s).stem + ".obj")
        r = subprocess.run([build_port.CLANG, *fl, s, "-o", str(obj)], cwd=ROOT)
        if r.returncode:
            sys.exit(f"compile failed: {s}")
        objs.append(str(obj))
    exe = out / "test_j3d.exe"
    # The factories' create() paths reference J3DJoint/J3DShape/J3DMaterial code
    # that is not linked; the test never calls them.
    r = subprocess.run([build_port.CLANG, f"--target={a.target}", "-fuse-ld=lld", "-g", *objs,
                        "-o", str(exe), "-Wl,/force:unresolved"],
                       cwd=ROOT, capture_output=True, text=True)
    if not exe.exists():
        print(r.stdout + r.stderr)
        sys.exit("link failed")
    r = subprocess.run([str(exe), str(out / "files.txt"), str(data)], cwd=ROOT,
                       capture_output=True, text=True, errors="replace")
    (out / "actual.txt").write_text(r.stdout, encoding="utf-8")
    act = r.stdout.splitlines()
    expl = exp
    bad = 0
    cur = None
    ai = 0
    # compare file by file ("== path" sections); a skipped reference only
    # checks the C++ side's own invariants (it prints "skip" lines then).
    sect_e, sect_a = {}, {}
    for lines, d in ((expl, sect_e), (act, sect_a)):
        for ln in lines:
            if ln.startswith("== "):
                cur = ln[3:]
                d[cur] = []
            elif ln.startswith("SUMMARY"):
                cur = None
            elif cur is not None:
                d[cur].append(ln)
    for f, el in sect_e.items():
        al = sect_a.get(f)
        if al is None:
            print(f"MISSING output for {f}")
            bad += 1
            continue
        if el == ["skip"]:
            continue
        if len(el) != len(al) or not all(same(x, y) for x, y in zip(el, al)):
            bad += 1
            if bad <= 10:
                print(f"MISMATCH {f}")
                for x, y in zip(el, al):
                    if not same(x, y):
                        print(f"  exp: {x}\n  got: {y}")
                        break
                else:
                    print(f"  line count {len(el)} vs {len(al)}")
    tail = [ln for ln in act if ln.startswith(("FAIL", "SUMMARY"))]
    print("\n".join(tail[-20:]))
    print(f"reference skipped {len(skipped)} file(s)" + (": " + "; ".join(skipped[:5]) if skipped else ""))
    print(f"{len(models)} models, {len(anms)} animations, {bad} mismatching file(s), "
          f"exit {r.returncode}")
    sys.exit(1 if bad or r.returncode else 0)


if __name__ == "__main__":
    main()
