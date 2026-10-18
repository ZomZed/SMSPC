#!/usr/bin/env python3
"""Evaluate member offsets, sizes or other constant expressions with the real MWCC.

Usage (repo root, after configure.py has been run):
    python tools/mwcc_probe.py "Player/Mario.hpp[,Other.hpp]" "EXPR" ["EXPR" ...]
Example:
    python tools/mwcc_probe.py Player/Mario.hpp "OFF(TMario, mJumpParams.mRotBroadEnableV)" \
        "sizeof(TParamRT<s16>)"
    -> OFF(TMario, mJumpParams.mRotBroadEnableV) = 3264 (0xCC0) ...

`OFF(T, member)` is offsetof (member paths with '.' work). Each expression must be an integer
constant. The headers are compiled with the GMSE01 game flags into a temporary object and the
values are read back from its .data, so the result is the GameCube layout (not the PC one).
Use it to name the field behind an offset in the target asm, e.g. a TParamRT value is at +0x10
inside its param (`lfs f0, 0xcd0(r31)` = the TParamRT member at 0xCC0).
"""
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

INCLUDES = ["include", "libs/dolphin/include", "libs/JSystem/include", "libs/THPPlayer/include",
            "libs/PowerPC_EABI_Support/include", "libs/TRK_MINNOW_DOLPHIN/include", "libs/OdemuExi2/include",
            "libs/PowerPC_EABI_Support/include/PowerPC_EABI_Support/Msl/MSL_C/MSL_Common",
            "libs/PowerPC_EABI_Support/include/PowerPC_EABI_Support/Msl/MSL_C++/MSL_Common",
            "build/GMSE01/include"]


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(2)
    headers = [h for h in sys.argv[1].split(",") if h]
    exprs = sys.argv[2:]
    tmp = tempfile.mkdtemp(prefix="mwcc_probe_", dir=os.path.join(ROOT, "build"))
    try:
        src = os.path.join(tmp, "probe.cpp")
        obj = os.path.join(tmp, "probe.o")
        with open(src, "w") as f:
            for h in headers:
                f.write(f"#include <{h}>\n")
            f.write("#define OFF(T, m) ((int)&((T*)0)->m)\n")
            f.write("extern const int sms_probe[] = {\n" + ",\n".join(exprs) + "\n};\n")
        exe = ".exe" if os.name == "nt" else ""
        cmd = [os.path.join("build", "tools", "sjiswrap" + exe), os.path.join("build", "compilers", "GC", "1.2.5", "mwcceppc.exe"),
               "-nodefaults", "-align", "powerpc", "-enum", "int", "-fp", "hardware", "-Cpp_exceptions", "off",
               "-maxerrors", "3", "-nosyspath", "-RTTI", "off", "-str", "reuse", "-multibyte", "-cwd", "source"]
        for i in INCLUDES:
            cmd += ["-i", i]
        cmd += ["-DBUILD_VERSION=2", "-Dnullptr=0", "-Dalignof=__builtin_align", "-DVERSION_GMSE01", "-DVERSION_GMSP01",
                "-proc", "gekko", "-DGEKKO", "-DNDEBUG=1", "-O4,p", "-lang=c++", "-c", src, "-o", obj]
        r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, errors="replace")
        if r.returncode != 0:
            print(r.stdout[-3000:], r.stderr[-2000:])
            sys.exit(1)
        dump = subprocess.run([os.path.join(ROOT, "build", "binutils", "powerpc-eabi-objdump" + exe), "-s", "-j", ".data", obj],
                              capture_output=True, text=True).stdout
        data = b""
        for line in dump.splitlines():
            m = re.match(r" [0-9a-f]+ ((?:[0-9a-f]+ ?){1,4})", line)
            if m:
                for w in m.group(1).split():
                    data += bytes.fromhex(w)
        vals = struct.unpack(">%di" % (len(data) // 4), data[:len(data) // 4 * 4])
        for e, v in zip(exprs, vals):
            print(f"{e} = {v} (0x{v & 0xFFFFFFFF:X})")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    main()
