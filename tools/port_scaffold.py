"""Generate a starting point for decompiling a unit.

Usage: python tools/port_scaffold.py mario/Enemy/fruitsboat [--no-m2c]

Writes build/drafts/<unit>.txt containing:
  1. every symbol the JP linker map lists for the unit, per section, in map order
     (UNUSED = fully inlined/dead; give it a body only if a caller shows it inlined)
  2. the .text list in SOURCE order (reversed, for -inline deferred units)
  3. an m2c draft of every function present in build/GMSE01/asm/<path>.s
"""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MAP = os.path.join(ROOT, "orig", "GMSJ01", "files", "mario.MAP")

def _find_m2c():
    # works from the main tree and from workers\wN (m2c lives next to the main tree)
    d = ROOT
    for _ in range(4):
        cand = os.path.join(d, "m2c", "m2c-master", "m2c.py")
        if os.path.exists(cand):
            return cand
        d = os.path.dirname(d)
    return os.path.join(os.path.dirname(ROOT), "m2c", "m2c-master", "m2c.py")


M2C = _find_m2c()
SECTIONS = [".text", ".ctors", ".rodata", ".data", ".bss", ".sdata", ".sbss", ".sdata2"]


def map_symbols(cpp):
    out = {s: [] for s in SECTIONS}
    cur = None
    pat = re.compile(r"^\s+(UNUSED|[0-9a-f]{8})\s+([0-9a-f]{6})\s+(\S+)\s+(?:\d+\s+)?(\S+)\s+.*\s" + re.escape(cpp) + r"\s*$")
    with open(MAP, encoding="latin-1") as f:
        for line in f:
            m = re.match(r"^(\.\w+|extab|extabindex) section layout", line)
            if m:
                cur = m.group(1) if m.group(1) in out else None
                continue
            if cur is None or cpp not in line:
                continue
            m = pat.match(line)
            if not m or m.group(4) == cur:
                continue
            out[cur].append((m.group(1) == "UNUSED", int(m.group(2), 16), m.group(4)))
    return out


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    unit = args[0]
    path = unit.split("/", 1)[1] if unit.startswith("mario/") else unit
    cpp = os.path.basename(path) + ".cpp"
    asm = os.path.join(ROOT, "build", "GMSE01", "asm", path + ".s")
    syms = map_symbols(cpp)
    os.makedirs(os.path.join(ROOT, "build", "drafts"), exist_ok=True)
    out_path = os.path.join(ROOT, "build", "drafts", path.replace("/", "_") + ".txt")
    asm_fns = []
    if os.path.exists(asm):
        asm_fns = re.findall(r'^\.fn "?([^",\s]+)"?', open(asm, encoding="utf-8", errors="replace").read(), re.M)

    with open(out_path, "w", encoding="utf-8") as o:
        o.write("# Scaffold for %s (JP map, %s)\n\n" % (unit, cpp))
        for s in SECTIONS:
            if not syms[s]:
                continue
            o.write("## %s (%d symbols, map order)\n" % (s, len(syms[s])))
            for unused, size, name in syms[s]:
                o.write("  %-7s 0x%-6x %s\n" % ("UNUSED" if unused else "", size, name))
            o.write("\n")
        o.write("## .text in SOURCE order (reverse of map; for -inline deferred)\n")
        for unused, size, name in reversed(syms[".text"]):
            o.write("  %-7s 0x%-6x %s\n" % ("UNUSED" if unused else "", size, name))
        o.write("\n## Functions in US asm not named in JP map .text for this TU\n")
        jp = {n for _, _, n in syms[".text"]}
        for n in asm_fns:
            if n not in jp:
                o.write("  %s\n" % n)
        if "--no-m2c" in sys.argv or not os.path.exists(asm):
            print("wrote", out_path); return
        o.write("\n# ---------------- m2c drafts (rough; never trust over asm) ----------------\n")
        for n in asm_fns:
            try:
                r = subprocess.run([sys.executable, M2C, "-t", "ppc", "-f", n, "--globals=none", asm],
                                   capture_output=True, text=True, timeout=120)
                body = r.stdout.strip() or r.stderr.strip()
            except Exception as ex:
                body = "/* m2c failed: %s */" % ex
            o.write("\n// ===== %s =====\n%s\n" % (n, body))
    print("wrote", out_path, "(%d functions drafted)" % len(asm_fns))


if __name__ == "__main__":
    main()
