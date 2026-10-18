"""Store/call diff between the target and our objects (w11 constructor and store/call audit).

    python tools/ctordiff.py <unit, e.g. Enemy/bosstelesa> [function substring] [-a] [-c]
    python tools/ctordiff.py --rank [dir-prefix ...] [-c] [-o out.md]

Compares the target object (build/GMSE01/obj/<unit>.o) with ours (build/GMSE01/src/<unit>.o)
function by function, as a multiset of events, ignoring registers and scheduling:
  call <symbol>              every bl with a relocation
  st<w|h|b|fs|fd> +<off> = K  store through a non-stack base at <off>; K is the stored
                             constant (li / lis+addi in the same block), '?' if unknown,
                             '@sym' for a relocated address (vtables etc.), 'f' for FP stores
  fconst <value>             an lfs/lfd of a literal from .sdata2/.rodata (value decoded)
  imm <value>                an li/lis immediate (only with -c: noisier)
  l<wz|hz|ha|bz|fs|fd> +<off> a field load through a non-stack base (only with -c)
  mask <value>               an rlwinm/andi. bit mask (only with -c): wrong flag/input bits
With -c, a field read at the wrong offset or a test of the wrong bit shows up too; the usual
noise there is the same field reached through another base register and reloads/hoisting.
Stack traffic (r1) and indexed stores (stwx...) are ignored. A store with an unknown value on
one side matches the same store with any value on the other. Local symbol numbering
(@instance$123, @L) is normalised.

--rank runs over every unit (optionally only those under the given src/ directory prefixes)
and every function that is NonMatching in build/GMSE01/report.json, and prints the functions
ranked by the number of differing events."""
import collections, json, re, struct, subprocess, sys
from pathlib import Path

OD = r"build\binutils\powerpc-eabi-objdump.exe"


def run(*a):
    return subprocess.run([OD, *a], capture_output=True, text=True, errors="replace").stdout


def literal_values(path):
    secs, cur = {}, None
    for l in run("-s", path).splitlines():
        m = re.match(r"Contents of section (\S+):", l)
        if m:
            cur = m.group(1); secs[cur] = bytearray(); continue
        m = re.match(r" ([0-9a-f]+) ((?:[0-9a-f]+ ?){1,4})", l)
        if m and cur:
            off = int(m.group(1), 16); b = bytearray()
            for w in m.group(2).split():
                b += bytes.fromhex(w)
            if len(secs[cur]) < off:
                secs[cur] += bytes(off - len(secs[cur]))
            secs[cur][off:off + len(b)] = b
    vals = {}
    for l in run("-t", path).splitlines():
        m = re.match(r"([0-9a-f]+) (\w)\s+O (\.\w+)\s+([0-9a-f]+) (\S+)", l)
        if m and m.group(3) in secs and m.group(3) in (".sdata2", ".rodata", ".sdata", ".data"):
            off = int(m.group(1), 16); sz = int(m.group(4), 16)
            b = bytes(secs[m.group(3)][off:off + sz])
            if sz == 4 and len(b) == 4:
                vals[m.group(5)] = "%g" % struct.unpack(">f", b)[0]
            elif sz == 8 and len(b) == 8:
                vals[m.group(5)] = "d%g" % struct.unpack(">d", b)[0]
            # dtk sometimes merges neighbouring literals into one odd-sized symbol
            # (e.g. size 6: a float and half of the next); decode each word as sym+off
            sec = secs[m.group(3)]
            for k in range(0, max(sz, 4), 4):
                w = bytes(sec[off + k:off + k + 4])
                if len(w) == 4:
                    vals[f"{m.group(5)}+{k:#x}"] = "%g" % struct.unpack(">f", w)[0]
    return vals


def norm_sym(s):
    s = re.sub(r"[+-]0x[0-9a-f]+$", "", s)
    if re.match(r"^(@\d+|lbl_|\.\.\.)", s):
        return "L"
    return re.sub(r"\$\d+$", "$", s)


def disasm(path):
    out = run("-dr", path)
    funcs, cur = {}, None
    for line in out.splitlines():
        m = re.match(r"[0-9a-f]+ <(.+)>:", line)
        if m:
            cur = m.group(1); funcs[cur] = []; continue
        if cur is None:
            continue
        m = re.match(r"\s+[0-9a-f]+:\s+(?:[0-9a-f]{2} ){4}\s*(\S+)\s*(.*)", line)
        if m:
            funcs[cur].append([m.group(1), m.group(2).strip(), None, None]); continue
        m = re.match(r"\s+[0-9a-f]+: (R_PPC_\S+)\s+(\S+)", line)
        if m and funcs[cur]:
            mo = re.search(r"([+-]0x[0-9a-f]+)$", m.group(2))
            raw = re.sub(r"[+-]0x[0-9a-f]+$", "", m.group(2))
            if mo and int(mo.group(1), 16) > 0:
                raw += f"+{int(mo.group(1), 16):#x}"
            funcs[cur][-1][2] = norm_sym(m.group(2))
            funcs[cur][-1][3] = raw
    return funcs


def events(ins, lits=None, imms=False):
    ev = []
    const = {}
    for op, args, rel, raw in ins:
        a = [x.strip() for x in args.split(",")] if args else []
        if op == "bl":
            ev.append("call " + (rel or args)); const.clear(); continue
        if op.startswith("b"):
            const.clear(); continue
        if op in ("lfs", "lfd") and raw and lits is not None:
            # raw keeps a non-zero offset as "sym+0x4" (see disasm)
            v = lits.get(raw)
            if v is None and op == "lfs":
                v = lits.get(raw + "+0x0")
            if v is not None:
                ev.append("fconst " + v); continue
        if op == "li" and len(a) == 2:
            const[a[0]] = "@" + rel if rel else a[1]
            if imms and not rel:
                ev.append("imm " + a[1])
            continue
        if op == "lis" and len(a) == 2:
            const[a[0]] = "@" + rel if rel else "hi:" + a[1]
            if imms and not rel:
                ev.append("imm hi:" + a[1])
            continue
        if op in ("addi", "ori") and len(a) == 3 and a[1] in const and str(const[a[1]]).startswith("@"):
            const[a[0]] = const[a[1]] + ("" if a[2] in ("0",) or rel else "+" + a[2]); continue
        if op == "addi" and len(a) == 3 and rel:
            const[a[0]] = "@" + rel; continue
        if imms and re.match(r"(lwz|lhz|lha|lbz|lfs|lfd)$", op) and len(a) == 2 and not rel:
            mm = re.match(r"(-?(?:0x)?[0-9a-f]+)\((r\d+)\)", a[1])
            if mm and mm.group(2) != "r1":
                ev.append(f"{op} +{int(mm.group(1), 0):#x}")
        if imms and op in ("rlwinm.", "rlwinm") and len(a) == 5 and a[2] == "0":
            mb, me = int(a[3]), int(a[4])
            if mb <= me:
                mask = sum(1 << (31 - b) for b in range(mb, me + 1))
                ev.append(f"mask {mask:#x}")
        if imms and op == "andi." and len(a) == 3:
            ev.append(f"mask {int(a[2], 0):#x}")
        if re.match(r"(stw|sth|stb|stfs|stfd)$", op) and len(a) == 2:
            mm = re.match(r"(-?(?:0x)?[0-9a-f]+)\((r\d+)\)", a[1])
            if mm and mm.group(2) != "r1":
                off = int(mm.group(1), 0)
                val = const.get(a[0], "?") if not op.startswith("stf") else "f"
                if rel:
                    val = "@" + rel
                ev.append(f"{op} +{off:#x} = {val}")
            continue
        if a:
            const.pop(a[0], None)
    return ev


def compare(ea, eb):
    a, b = collections.Counter(ea), collections.Counter(eb)
    ta, tb = a - b, b - a
    for x, y in ((ta, tb), (tb, ta)):
        for k in list(x):
            if k.endswith("= ?"):
                pre = k[:-1]
                for k2 in list(y):
                    if k2.startswith(pre) and x[k] and y[k2]:
                        n = min(x[k], y[k2]); x[k] -= n; y[k2] -= n
    return +ta, +tb


def unit_diff(unit, filt=None, imms=False):
    A = disasm(f"build/GMSE01/obj/{unit}.o"); B = disasm(f"build/GMSE01/src/{unit}.o")
    la = literal_values(f"build/GMSE01/obj/{unit}.o"); lb = literal_values(f"build/GMSE01/src/{unit}.o")
    res = {}
    for f in A:
        if f not in B or (filt and filt not in f):
            continue
        res[f] = compare(events(A[f], la, imms), events(B[f], lb, imms))
    return res


def single(argv):
    show_all = "-a" in argv; imms = "-c" in argv
    args = [x for x in argv if x not in ("-a", "-c")]
    res = unit_diff(args[0], args[1] if len(args) > 1 else None, imms)
    nd = 0
    for f, (ta, tb) in res.items():
        if ta or tb or show_all:
            nd += bool(ta or tb)
            print("##", f)
            for k, v in sorted(ta.items()):
                print("   target-only", v, k)
            for k, v in sorted(tb.items()):
                print("   ours-only  ", v, k)
    print(f"-- {nd} functions differ")


def rank(argv):
    out = None
    if "-o" in argv:
        i = argv.index("-o"); out = argv[i + 1]; argv = argv[:i] + argv[i + 2:]
    imms = "-c" in argv
    prefixes = [p for p in argv if p != "-c"]
    rep = json.loads(Path("build/GMSE01/report.json").read_text())
    nonmatch = collections.defaultdict(dict)
    for u in rep["units"]:
        for f in u.get("functions", []):
            p = f.get("fuzzy_match_percent", 0.0)
            if p < 100.0:
                nonmatch[u["name"]][f["name"]] = p
    rows = []
    for o in sorted(Path("build/GMSE01/obj").rglob("*.o")):
        unit = o.relative_to("build/GMSE01/obj").with_suffix("").as_posix()
        if prefixes and not any(unit.startswith(p) for p in prefixes):
            continue
        uname = "mario/" + unit
        if uname not in nonmatch or not Path(f"build/GMSE01/src/{unit}.o").exists():
            continue
        try:
            res = unit_diff(unit, None, imms)
        except Exception as e:  # noqa
            continue
        for f, (ta, tb) in res.items():
            if f not in nonmatch[uname] or not (ta or tb):
                continue
            score = sum(ta.values()) + sum(tb.values())
            rows.append((score, unit, f, nonmatch[uname][f], ta, tb))
    rows.sort(key=lambda r: -r[0])
    lines = []
    for score, unit, f, pct, ta, tb in rows:
        lines.append(f"### {score:3d}  {unit} :: {f}  ({pct:.1f}%)")
        for k, v in sorted(ta.items()):
            lines.append(f"    target-only {v} {k}")
        for k, v in sorted(tb.items()):
            lines.append(f"    ours-only   {v} {k}")
    text = "\n".join(lines) + "\n"
    if out:
        Path(out).write_text(text, encoding="utf-8")
    else:
        sys.stdout.write(text)
    print(f"-- {len(rows)} NonMatching functions with store/call differences", file=sys.stderr)


if __name__ == "__main__":
    if sys.argv[1:2] == ["--rank"]:
        rank(sys.argv[2:])
    else:
        single(sys.argv[1:])
