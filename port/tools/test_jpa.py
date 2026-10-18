"""Host test for the JPA (.jpa) big-endian handling on x86_64.

    python port/tools/test_jpa.py [--target x86_64-pc-windows-msvc]

1. Extracts particle.szs, bosshanachanJpa.szs and the dolpic0 / bianco0 / airport0
   scene archives from dolphin_test/files with `dtk vfs cp` (once).
2. Decodes every .jpa big-endian in Python (block walk, BSP1/ESP1/SSP1/ETX1 fields,
   BEM1 and FLD1 stream layouts, KFA1 key frames, TEX1 names) into the expected dump.
3. Builds port/tools/test_jpa.cpp with the real JPAEmitterLoader, JPABaseShape,
   JPAExtraShape, JPASweepShape, JPAExTexShape, JPADataBlock, JPATexture,
   JPAResourceManager, JPAEmitter (loadBaseEmitterBlock), JPAField
   (loadFieldBlock) and JSU stream sources, loads each file twice, and compares
   what the real objects hold. Also checks the loader never modifies the file.
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
ARCHIVES = ["particle", "bosshanachanJpa", "scene/dolpic0", "scene/bianco0", "scene/airport0"]
SRCS = ["port/tools/test_jpa.cpp"] + [
    f"libs/JSystem/src/JParticle/{n}.cpp" for n in
    ("JPAEmitterLoader", "JPABaseShape", "JPAExtraShape", "JPASweepShape", "JPAExTexShape",
     "JPADataBlock", "JPATexture", "JPAResourceManager", "JPAEmitter", "JPAField", "JPAMath")
] + ["libs/JSystem/src/JSupport/JSUMemoryStream.cpp", "libs/JSystem/src/JSupport/JSUInputStream.cpp",
     "libs/JSystem/src/JSupport/JSUList.cpp"]


def extract(dtk, data):
    for a in ARCHIVES:
        dst = data / Path(a).name
        if dst.exists():
            continue
        r = subprocess.run([dtk, "vfs", "cp", f"{FILES / (a + '.szs')}:", str(dst)],
                           cwd=ROOT, stdout=subprocess.DEVNULL)
        if r.returncode:
            sys.exit(f"dtk vfs cp failed for {a}.szs")


def u8(b, o): return b[o]
def u16(b, o): return struct.unpack_from(">H", b, o)[0]
def s16(b, o): return struct.unpack_from(">h", b, o)[0]
def u32(b, o): return struct.unpack_from(">I", b, o)[0]
def f32(b, o): return struct.unpack_from(">f", b, o)[0]


def fx(v):  # JPAConvertFixToFloat
    if v == 0x7fff:
        return 1.0
    f0 = struct.unpack("<f", struct.pack("<f", v * (1.0 / 32768.0) * 100000.0))[0]
    r5 = int(f0)
    r0 = int(math.fmod(f0, 10))
    r5 += (10 - r0) if r0 >= 5 else -r0
    return r5 * 1e-05


def g(v): return "%.9g" % v
def col(b, o): return "%d,%d,%d,%d" % tuple(b[o:o + 4])


def blocks(b):
    assert b[:8] == b"JEFFjpa1"
    o, out = 0x20, []
    for _ in range(u32(b, 0xC)):
        out.append((b[o:o + 4], o, u32(b, o + 4)))
        o += u32(b, o + 4)
    assert o == len(b)
    return out


def color_table(b, keyoff, nkeys, size):
    keys = [(s16(b, keyoff + k * 6), tuple(b[keyoff + k * 6 + 2: keyoff + k * 6 + 6]))
            for k in range(nkeys)]
    return keys


def ref(b):
    L = []
    bl = blocks(b)
    flds = [x for x in bl if x[0] == b"FLD1"]
    texs = [x for x in bl if x[0] == b"TEX1"]
    kfas = [x for x in bl if x[0] == b"KFA1"]
    kinds = {x[0] for x in bl}
    L.append("link %d %d %d %d %d %d %d" % (len(flds), len(texs), len(kfas), b"BSP1" in kinds,
                                          b"ESP1" in kinds, b"SSP1" in kinds, b"ETX1" in kinds))
    for t, o, sz in bl:
        d = b[o:o + sz]
        if t == b"BSP1":
            ty = d[0x24]
            L.append("bsp %s %s %d %d %d %d %d %s %s" % (
                g(f32(d, 0x1c)), g(f32(d, 0x18)), s16(d, 0x20), ty, d[0x25], d[0x26], d[0x44],
                col(d, 100), col(d, 0x68)))
            L.append("bsp2 " + " ".join(g(fx(s16(d, 0x80 + 2 * i)) * m) for i, m in
                                        enumerate([10, 10, 10, 10, 10, 10, 1, 1, 0.1, 0.1, 1])))
            r5, r6 = d[0x22], d[0x23]
            keynum = d[0x4e]
            unk80 = 1 if (d[0x4c] & 1) and keynum != 0 else 0
            unk81 = 0 if (d[0x4c] & 2) else 1
            r5_2 = (r5 & 2) or ty in (5, 6)
            r4 = (r6 & 2) or ty in (5, 6)
            unk68 = (1 if r5_2 else 0) << 1 | (1 if r4 else 0)
            L.append("bsp3 %d %d %d %d %d %d %d %d %d %d %d %d %d %d" % (
                -1 if (r5 & 1) else 0, -1 if (r6 & 1) else 0, s16(d, 0x5c), keynum, d[0x4f],
                unk80, unk81, d[0x5e], d[0x60], d[0x61], d[0x62], d[0x63], d[0x96], unk68))
            L.append("bsp4 " + " ".join(str(d[x]) for x in (0x35, 0x36, 0x37, 0x38, 0x39, 0x3a,
                                                             0x3b, 0x3c, 0x3d)) +
                     " %d %d %d %d %d %d" % (0 if (d[0x3e] >> 1) & 1 else 1, d[0x3f], d[0x40],
                                             d[0x41], d[0x42], d[0x43]))
            idx = s16(d, 0x12)
            L.append("bsp5" + "".join(" %d" % x for x in
                                      (d[idx:idx + keynum] if unk80 else [])))
            maxfrm = s16(d, 0x5c)
            for name, flag, cnt, offo in (("prmtbl", d[0x60] & 2, d[0x62], 0x14),
                                          ("envtbl", d[0x61] & 2, d[0x63], 0x16)):
                if flag:
                    ko = s16(d, offo)
                    keys = color_table(d, ko, cnt, maxfrm)
                    L.append("TABLE %s %d %s" % (name, maxfrm, repr(keys)))
                else:
                    L.append(name)
        elif t == b"ESP1":
            S = lambda o: fx(s16(d, o))
            ain, aout, aiv, abv, u18 = S(0x14), S(0x16), S(0x18), S(0x1a), S(0x1c)
            w1, w2, w3, wr = S(0x20), S(0x22), S(0x24), S(0x26)
            inc = (abv - aiv) / ain if ain != 0.0 else 1.0
            dec = (u18 - abv) / (1.0 - aout) if aout != 1.0 else 1.0
            L.append("esp " + " ".join(g(x) for x in (ain, aout, aiv, abv, u18, w1, w2, w3, wr,
                                                        inc, dec)))
            rs, sin_, sout = S(0x34), S(0x36), S(0x38)
            sy, u44, sx, u4c = S(0x3a) * 10, S(0x3e) * 10, S(0x44) * 10, S(0x48) * 10
            if sin_ != 0.0:
                irx, iry = (1.0 - sx) / sin_, (1.0 - sy) / sin_
            else:
                irx = iry = 1.0
            if sout != 1.0:
                drx, dry = (u4c - 1.0) / (1.0 - sout), (u44 - 1.0) / (1.0 - sout)
            else:
                drx = dry = 1.0
            L.append("esp2 " + " ".join(g(x) for x in (rs, sin_, sout, sy, u44, sx, u4c, irx, drx,
                                                         iry, dry, S(0x5a))))
            L.append("esp3 %s %s %s %s %d %d %d %d %d %d %d %d %d %d %d" % (
                g(S(0x5c)), g(S(0x5e)), g(S(0x60)), g(S(0x62)), s16(d, 0x42), s16(d, 0x4c), d[0x1e],
                d[0x1f], d[0x40], d[0x41], d[0x4a], d[0x4b], d[0x4e], d[0x64], 0))
        elif t == b"SSP1":
            S = lambda o: fx(s16(d, o))
            L.append("ssp %s %s %s %s %s %s %s %s %s %s %s %s %s" % (
                g(f32(d, 0x28)), g(f32(d, 0x2c)), g(f32(d, 0x4c)), g(f32(d, 0x50)), g(S(0x18)),
                g(S(0x30)), g(S(0x32)), g(S(0x34)), g(S(0x48)), g(S(0x4a)), g(S(0x54)),
                col(d, 0x58), col(d, 0x5c)))
            L.append("ssp2 %s %d %d %d %d %d %d %d %d %d %d %d %d" % (
                g(S(0x60)), s16(d, 0x14), s16(d, 0x16), d[0x10], d[0x11], d[0x12], d[0x1a], d[0x36],
                d[0x44], d[0x45], d[0x46], d[0x47], d[0x56]))
            L.append("ssp3 %d" % d[0x57])
        elif t == b"ETX1":
            mode, kind = d[0x10], d[0x11]
            vals = [0.0] * 6
            if kind in (0, 1):
                mid = 1  # GX_ITM_0
                vals = [fx(s16(d, 0x12 + 2 * i)) for i in range(6)]
            elif kind == 2:
                mid = 5  # GX_ITM_S0
            else:
                mid = 9  # GX_ITM_T0
            L.append("etx %d %d %d %d %d %d %d" % (mode, d[0x1e], d[0x1f], d[0x20], d[0x30],
                                                     d[0x33], mid) + " " + " ".join(g(v) for v in vals))
        elif t == b"BEM1":
            p = 0xC
            sc = struct.unpack_from(">3f", d, p); p += 12
            tr = struct.unpack_from(">3f", d, p); p += 12
            rot = struct.unpack_from(">3h", d, p); p += 6
            L.append("bem %s %s %s %s %s %s %d %d %d %d %d" % (
                *(g(x) for x in sc), *(g(x) for x in tr), *rot, d[p], d[p + 1]))
            p += 2 + 2  # type, interval, skip 2
            sub = u16(d, p); p += 2
            csr = f32(d, p); p += 4
            csv = fx(s16(d, p)); p += 2
            mf = s16(d, p); p += 2
            sf = s16(d, p); p += 2
            vs = u16(d, p); p += 2
            yaw = fx(s16(d, p)); p += 2
            mnr = fx(s16(d, p)); p += 2
            bl = s16(d, p); p += 2
            lrs = fx(s16(d, p)); p += 2
            bw = fx(s16(d, p)); p += 2
            wrs = fx(s16(d, p)); p += 2
            u1c4 = fx(s16(d, p)); p += 2
            u1cc = fx(s16(d, p)); p += 2
            bar = fx(s16(d, p)); p += 2
            arv = fx(s16(d, p)); p += 2
            f1fc, f200, f204, f208, f1c8 = struct.unpack_from(">5f", d, p); p += 20
            fv = [fx(v) for v in struct.unpack_from(">3h", d, p)]; p += 6
            u1e4 = fx(s16(d, p)); p += 2
            ef = u32(d, p); p += 4
            kam = u32(d, p)
            L.append("bem2 %d %s %s %d %d %d %s %s %d %s %s %s" % (
                sub, g(csr), g(csv), mf, sf, vs, g(yaw), g(mnr), bl, g(lrs), g(bw), g(wrs)))
            ln = math.sqrt(sum(float(x) * x for x in fv))
            dv = [x / ln for x in fv] if ln else [0, 0, 0]
            L.append("bem3 " + " ".join(g(x) for x in (u1c4, u1cc, bar, arv, f1fc, f200, f204,
                                                       f208, f1c8, *dv, u1e4)) + " %d %d" % (ef, kam))
        elif t == b"FLD1":
            p = 0xC
            u50, u51, u52, u53 = d[p:p + 4]; p += 4
            u54 = u16(d, p); p += 2 + 2
            fs = struct.unpack_from(">3f", d, p); p += 12  # u10 u14 maxdist
            v18 = struct.unpack_from(">3f", d, p); p += 12
            v24 = struct.unpack_from(">3f", d, p); p += 12
            u30, u34 = struct.unpack_from(">2f", d, p); p += 8
            u38 = u32(d, p); p += 4
            fi = [fx(s16(d, p + 2 * i)) for i in range(4)]
            L.append("fld %d %d %d %d %d %s %s %s %s %s %s %s %s %s %s %d" % (
                u50, u51, u52, u53, u54, g(fs[0]), g(fs[1]), g(fs[2]), *(g(x) for x in v18),
                *(g(x) for x in v24), g(u30), u38))
            L.append("fld2 %s %s %s %s %s" % (g(u34), g(fi[0]), g(fi[1]), g(fi[2]), g(fi[3])))
        elif t == b"KFA1":
            n = d[0x10]
            L.append("kfa %d %d" % (n, d[0x12]) + "".join(" " + g(f32(d, 0x20 + i * 4))
                                                          for i in range(n * 4)))
        elif t == b"TEX1":
            nm = d[0xC:0x20].split(b"\0")[0].decode("ascii")
            # ResTIMG at +0x20: format u8, width/height u16 at +2/+4, imageDataOffset u32 at +0x1C
            L.append("tex %s %d %d %d %d" % (nm, d[0x20], u16(d, 0x22), u16(d, 0x24), u32(d, 0x3c)))
    # the C++ side dumps in a fixed order (shapes, bem, fields, kfa, textures)
    order = {"link": 0, "bsp": 1, "bsp2": 1, "bsp3": 1, "bsp4": 1, "bsp5": 1, "TABLE": 1,
             "prmtbl": 1, "envtbl": 1, "esp": 2, "esp2": 2, "esp3": 2, "ssp": 3, "ssp2": 3,
             "ssp3": 3, "etx": 4, "bem": 5, "bem2": 5, "bem3": 5, "fld": 6, "fld2": 6, "kfa": 7,
             "tex": 8}
    return sorted(L, key=lambda s: order[s.split()[0]])  # stable: keeps per-kind block order


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
            continue  # the sign of a NaN (0/0 in the loader) is not meaningful
        if not math.isclose(fa, fc, rel_tol=2e-6, abs_tol=2e-5):
            return False
    return True


def check_tables(exp, act):
    """Colour tables: compare the key entries and the interpolation bounds."""
    bad = 0
    for e, a in zip(exp, act):
        if not e.startswith("TABLE"):
            continue
        _, name, maxfrm, rest = e.split(" ", 3)
        keys = eval(rest)
        tbl = [tuple(int(v) for v in c.split(",")) for c in a.split()[1:]]
        if len(tbl) != int(maxfrm) + 1:
            bad += 1
            continue
        for idx, c in keys:
            if idx < len(tbl) and tbl[idx] != c:
                bad += 1
        # entries between two keys must stay inside the per-channel range of those keys
        for (i0, c0), (i1, c1) in zip(keys, keys[1:]):
            for i in range(i0, min(i1, len(tbl) - 1) + 1):
                for ch in range(4):
                    lo, hi = sorted((c0[ch], c1[ch]))
                    if not lo - 1 <= tbl[i][ch] <= hi + 1:
                        bad += 1
    return bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--target", default="x86_64-pc-windows-msvc")
    a = ap.parse_args()
    out = ROOT / "port" / "build" / "test_jpa" / a.target
    out.mkdir(parents=True, exist_ok=True)
    data = ROOT / "port" / "build" / "test_jpa" / "data"
    extract(str(ROOT / "build" / "tools" / "dtk.exe"), data)

    files = sorted(data.rglob("*.jpa"))
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
    exe = out / "test_jpa.exe"
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
    nblocks = 0
    for f, el in exp.items():
        al = act.get(f)
        nblocks += len(el)
        if al is None:
            print(f"MISSING output for {f}")
            bad += 1
            continue
        half = len(al) // 2
        if len(al) != 2 * half or al[:half] != al[half:]:
            print(f"second load differs: {f}")
            bad += 1
            continue
        al = al[:half]
        exp_cmp = [("tbl" if e.startswith("TABLE") else e) for e in el]
        # normalise: a TABLE expectation matches the "prmtbl"/"envtbl" dump line
        ok = len(al) == len(el)
        if ok:
            tb = check_tables(el, al)
            for e, c in zip(el, al):
                if e.startswith("TABLE"):
                    continue
                if not same(e, c):
                    ok = False
                    if bad < 5:
                        print(f"MISMATCH {f}\n  exp: {e[:200]}\n  got: {c[:200]}")
                    break
            if tb:
                ok = False
                print(f"colour table mismatch in {f} ({tb})")
        else:
            print(f"line count {len(el)} vs {len(al)}: {f}")
            for e, c in zip(el, al):
                print("  ", e[:80], "|", c[:80])
                break
        if not ok:
            bad += 1
    print(f"{len(files)} .jpa files, {nblocks} dumped records, {bad} mismatching file(s), "
          f"exit {r.returncode}")
    sys.exit(1 if bad or r.returncode else 0)


if __name__ == "__main__":
    main()


