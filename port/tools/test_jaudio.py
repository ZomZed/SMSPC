"""Build and run port/tools/test_jaudio.cpp (JAudio endianness host test).

    python port/tools/test_jaudio.py [--target x86_64-pc-windows-msvc]

Extracts mSound.aaf (nintendo.szs) and the .bas files (mario.szs) with dtk, then
runs the swap passes over the real data and the real AudioRes files.
"""
import argparse
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import build_port  # noqa: E402

ROOT = build_port.ROOT
DISC = Path("<project>/dolphin_test/files")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--target", default="x86_64-pc-windows-msvc")
    a = ap.parse_args()
    out = ROOT / "port" / "build" / "test_jaudio" / a.target
    out.mkdir(parents=True, exist_ok=True)
    fl = [f if not f.startswith("--target=") else f"--target={a.target}"
          for f in build_port.flags(False)]
    obj = out / "test_jaudio.obj"
    r = subprocess.run([build_port.CLANG, *fl, "port/tools/test_jaudio.cpp", "-o", str(obj)],
                       cwd=ROOT)
    if r.returncode:
        sys.exit("compile failed")
    exe = out / "test_jaudio.exe"
    r = subprocess.run([build_port.CLANG, f"--target={a.target}", "-fuse-ld=lld", "-g", str(obj),
                        "-o", str(exe)], cwd=ROOT)
    if r.returncode:
        sys.exit("link failed")
    dtk = str(ROOT / "build" / "tools" / "dtk.exe")
    data = ROOT / "port" / "build" / "test_archive" / "data"
    data.mkdir(parents=True, exist_ok=True)
    for a_ in ("nintendo", "mario"):
        if not (data / a_).exists():
            r = subprocess.run([dtk, "vfs", "cp", f"{DISC / 'data' / (a_ + '.szs')}:",
                                str(data / a_)], cwd=ROOT, stdout=subprocess.DEVNULL)
            if r.returncode:
                sys.exit(f"dtk vfs cp failed for {a_}.szs")
    sys.exit(subprocess.run([str(exe), str(data / "nintendo" / "audi" / "mSound.aaf"),
                             str(DISC / "AudioRes"), str(data / "mario" / "bas")],
                            cwd=ROOT).returncode)


if __name__ == "__main__":
    main()
