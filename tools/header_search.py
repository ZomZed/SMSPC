#!/usr/bin/env python3
"""Search shape variants of ONE inline function in a shared header; measure the whole-project effect.

  python tools/header_search.py --header libs/JSystem/include/JSystem/JGeometry/JGVec3.hpp \
      --anchor-file anchor.txt [--variants variants.json] [--gen order,swap] [--max 12] [--keep-best] [--out r.json]

anchor      exact text (whitespace-exact, as in the header) of the function to replace: --anchor "text" or --anchor-file.
            --occurrence N picks the Nth (0-based) occurrence.
variants    JSON list [{"name": "refret", "text": "<replacement for the whole anchor>"}, ...]
--gen       programmatic variants of the anchor body: order (permute simple `;` statements, 2..5 of them),
            swap (swap operands of simple `A + B` / `A * B`, one site at a time, plus all sites).
Baseline    run `python -m ninja baseline` on the unmodified tree first (this tool never touches it).
Per candidate: header written (encoded first, .tmp + os.replace), mario.dol/mario.elf deleted, full ninja
(FAILED/non-zero => build_failed), `dtk shasum -c config/GMSE01/build.sha1` (dol OK/DIFF/FAILED), then
`ninja changes_all`. Records total fuzzy (report.json), functions up/down, sum of deltas, worst single loss.
The original header is restored at the end, unless --keep-best and the best candidate has DOL OK,
sumDelta > 0 and ups > 3*downs. Ranked table is printed (and written to --out as JSON).
Headers are handled as raw bytes (latin-1 round trip, CRLF preserved), so Shift-JIS/BOM state is untouched.
Run from anywhere; it works on the tree that contains this script.
"""
import argparse, itertools, json, os, re, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build", "GMSE01")
DTK = os.path.join(ROOT, "build", "tools", "dtk.exe" if os.name == "nt" else "dtk")
ANSI = re.compile(r"\x1b\[[0-9;]*m")


def run(args, timeout=7200):
    r = subprocess.run(args, cwd=ROOT, capture_output=True, text=True, errors="replace", timeout=timeout)
    return r.returncode, ANSI.sub("", r.stdout + r.stderr)


def ninja(*t):
    return run([sys.executable, "-m", "ninja"] + list(t))


def write_atomic(path, data):
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(data)
    os.replace(tmp, path)


def total_fuzzy():
    try:
        with open(os.path.join(BUILD, "report.json"), encoding="utf-8") as f:
            return json.load(f)["measures"]["fuzzy_match_percent"]
    except Exception:
        return float("nan")


def measure(label):
    for n in ("mario.dol", "mario.elf"):
        try:
            os.remove(os.path.join(BUILD, n))
        except OSError:
            pass
    t0 = time.time()
    rc, out = ninja()
    cerr = [x for x in out.splitlines() if re.search(r"error", x, re.I) and "sjiswrap" not in x and "Shift JIS" not in x]
    have = os.path.exists(os.path.join(BUILD, "mario.dol")) and os.path.exists(os.path.join(BUILD, "mario.elf"))
    failed = bool(cerr) or not have  # a plain DOL mismatch also makes ninja's ok-step print FAILED; that is dol=DIFF, not a build failure
    dol = "FAILED"
    if not failed:
        rc2, so = run([DTK, "shasum", "-c", os.path.join("config", "GMSE01", "build.sha1")])
        ok = rc2 == 0 and "OK" in so and rc == 0
        dol = "OK" if ok else "DIFF"
    rc, ch = ninja("changes_all")
    ups = downs = 0
    delta = 0.0
    worst = (0.0, "")
    for l in ch.splitlines():
        m = re.match(r"\s*(\S.*?)\s*\|\s*fuzzy_match\s*\|\s*([\d.]+)%\s*->\s*([\d.]+)%", l)
        if not m or m.group(1).startswith("mario/"):
            continue
        d = float(m.group(3)) - float(m.group(2))
        if d > 0:
            ups += 1
        elif d < 0:
            downs += 1
            if d < worst[0]:
                worst = (d, m.group(1))
        delta += d
    errs = cerr[:3] if failed else []
    return dict(name=label, total=total_fuzzy(), ups=ups, downs=downs, delta=round(delta, 2),
                worst=round(worst[0], 2), worst_fn=worst[1][:60], dol=dol, build_failed=failed,
                secs=int(time.time() - t0), errors=errs)


def gen_variants(anchor, kinds):
    out = []
    m = re.search(r"\{(.*)\}\s*$", anchor, re.S)
    if not m:
        return out
    head, body, tail = anchor[:m.start(1)], m.group(1), anchor[m.end(1):]
    simple = [s.strip() + ";" for s in re.split(r";\s*\n", body.rstrip()) if s.strip()]
    ind = "\t\t"
    mi = re.match(r"\n?([ \t]*)", body)
    if mi and mi.group(1):
        ind = mi.group(1)

    def rebuild(ss):
        return head + "\n" + "".join(ind + s + "\n" for s in ss) + tail.lstrip("\n") if False else head + "\n" + "".join(ind + s + "\n" for s in ss) + tail

    if "order" in kinds and 2 <= len(simple) <= 5 and not any(re.search(r"\b(if|for|while|return)\b", s) for s in simple):
        n = len(simple)
        for p in itertools.permutations(range(n)):
            if list(p) != list(range(n)):
                out.append(("order" + "".join(map(str, p)), rebuild([simple[j] for j in p])))
    if "swap" in kinds:
        pat = re.compile(r"(?<![\w.>])([A-Za-z_][\w.\[\]]*)\s*([+*])\s*([A-Za-z_][\w.\[\]]*)(?![\w(])")
        sites = list(pat.finditer(anchor))
        for k, s in enumerate(sites[:8]):
            t = anchor[:s.start()] + "%s %s %s" % (s.group(3), s.group(2), s.group(1)) + anchor[s.end():]
            out.append(("swap%d(%s)" % (k, s.group(0).strip()), t))
        if len(sites) > 1:
            t = anchor
            for s in reversed(sites):
                t = t[:s.start()] + "%s %s %s" % (s.group(3), s.group(2), s.group(1)) + t[s.end():]
            out.append(("swapall", t))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--header", required=True)
    ap.add_argument("--anchor")
    ap.add_argument("--anchor-file")
    ap.add_argument("--occurrence", type=int, default=0)
    ap.add_argument("--variants")
    ap.add_argument("--gen", default="")
    ap.add_argument("--max", type=int, default=12)
    ap.add_argument("--keep-best", action="store_true")
    ap.add_argument("--out")
    a = ap.parse_args()
    hp = os.path.join(ROOT, a.header)
    orig = open(hp, "rb").read()
    text = orig.decode("latin-1")
    crlf = "\r\n" in text
    text = text.replace("\r\n", "\n")
    if a.anchor:
        anchor = a.anchor
    else:
        anchor = open(a.anchor_file, "rb").read().decode("latin-1").replace("\r\n", "\n").rstrip("\n")
    pos = -1
    for _ in range(a.occurrence + 1):
        pos = text.find(anchor, pos + 1)
        if pos < 0:
            sys.exit("anchor not found")
    cands = []
    if a.variants:
        for v in json.load(open(a.variants, encoding="utf-8")):
            cands.append((v["name"], v["text"]))
    cands += gen_variants(anchor, a.gen.split(","))
    cands = cands[:a.max]
    print("%d candidates" % len(cands), flush=True)

    def build_text(rep):
        new = text[:pos] + rep + text[pos + len(anchor):]
        if crlf:
            new = new.replace("\n", "\r\n")
        return new.encode("latin-1")

    results = []
    try:
        for name, rep in cands:
            write_atomic(hp, build_text(rep))
            r = measure(name)
            r["_text"] = rep
            results.append(r)
            print(json.dumps({k: v for k, v in r.items() if k != "_text"}), flush=True)
    finally:
        write_atomic(hp, orig)
    ranked = sorted(results, key=lambda r: (r["dol"] != "OK", -r["delta"]))
    print("\n%-30s %9s %4s %5s %8s %7s %-6s %s" % ("candidate", "total", "up", "down", "sumDelta", "worst", "dol", "worst fn"))
    for r in ranked:
        print("%-30s %9.4f %4d %5d %8.2f %7.2f %-6s %s" % (r["name"][:30], r["total"], r["ups"], r["downs"], r["delta"], r["worst"], r["dol"], r["worst_fn"]))
    best = ranked[0] if ranked else None
    kept = None
    if a.keep_best and best and best["dol"] == "OK" and best["delta"] > 0 and best["ups"] > 3 * best["downs"]:
        write_atomic(hp, build_text(best["_text"]))
        kept = best["name"]
        print("KEPT", kept)
    ninja()  # leave build/ consistent with the tree on disk
    for r in results:
        r.pop("_text", None)
    if a.out:
        json.dump(dict(header=a.header, kept=kept, results=results), open(a.out, "w"), indent=1)


if __name__ == "__main__":
    main()
