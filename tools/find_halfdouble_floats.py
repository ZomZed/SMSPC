"""Find float literals that look like the HIGH word of a double read as a float.

Example bug: 120.0 (double 0x405E000000000000) mis-decoded as 3.46875f (0x405E0000).
For every float literal in src/ we take its 32-bit pattern b. If the low 16 bits are 0
and the double formed by (b << 32) is a "round" value (integer or .5, 1 <= |x| <= 1e6)
while the float itself is not an integer, it is reported as a candidate.
Usage: python tools/find_halfdouble_floats.py [src_dir]
"""
import os
import re
import struct
import sys

root = sys.argv[1] if len(sys.argv) > 1 else "src"
lit = re.compile(r"(?<![\w.])(-?\d+\.\d+)f\b")
hits = 0
for dp, _, files in os.walk(root):
    for fn in files:
        if not fn.endswith((".cpp", ".c", ".hpp", ".h")):
            continue
        p = os.path.join(dp, fn)
        for ln, line in enumerate(open(p, encoding="utf-8", errors="replace"), 1):
            for m in lit.finditer(line):
                f = float(m.group(1))
                if f == int(f):
                    continue
                b = struct.unpack(">I", struct.pack(">f", f))[0]
                if b & 0xFFFF:
                    continue
                d = struct.unpack(">d", struct.pack(">Q", b << 32))[0]
                if 1.0 <= abs(d) <= 1e6 and (d * 2) == int(d * 2):
                    hits += 1
                    print("%s:%d  %sf  -> as double high word = %g   | %s"
                          % (p, ln, m.group(1), d, line.strip()[:110]))
print("candidates:", hits)
