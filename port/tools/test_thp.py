"""Host test for the THP video decoder (port/src/port_thp_decode.cpp).

    python port/tools/test_thp.py [--movie openingA] [--frames 0,300,900] [--all]

Builds port/tools/test_thp.cpp with the decoder, decodes the listed frames of
dolphin_test/files/data/<movie>.thp into GX I8 tiled Y/U/V planes, converts
them to RGB with the THP TEV constants and writes
port/build/test_thp/<movie>_frame_<n>.png. --all also decodes every frame of
the movie and fails on any decode error (prints the average decode time).
"""
import argparse
import struct
import subprocess
import sys
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import build_port  # noqa: E402

ROOT = build_port.ROOT
MOVIES = Path("<project>/dolphin_test/files/data")
SRCS = ["port/tools/test_thp.cpp", "port/src/port_thp_decode.cpp"]


def ppm_to_png(src, dst):
    data = src.read_bytes()
    parts = data.split(b"\n", 3)
    w, h = map(int, parts[1].split())
    pix = parts[3]
    raw = b"".join(b"\0" + pix[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(tag, body):
        return (struct.pack(">I", len(body)) + tag + body
                + struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF))

    png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))
    dst.write_bytes(png)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--movie", default="openingA")
    ap.add_argument("--frames", default="0,150,600")
    ap.add_argument("--all", action="store_true")
    a = ap.parse_args()
    out = ROOT / "port" / "build" / "test_thp"
    out.mkdir(parents=True, exist_ok=True)

    objs = []
    for s in SRCS:
        obj = out / (Path(s).stem + ".obj")
        r = subprocess.run([build_port.CLANG, "--target=x86_64-pc-windows-msvc", "-c", "-O2",
                            "-std=c++17", "-fms-runtime-lib=dll", "-Wall",
                            "-D_CRT_SECURE_NO_WARNINGS", s, "-o", str(obj)],
                           cwd=ROOT)
        if r.returncode:
            sys.exit(f"compile failed: {s}")
        objs.append(str(obj))
    exe = out / "test_thp.exe"
    r = subprocess.run([build_port.CLANG, "--target=x86_64-pc-windows-msvc", "-fuse-ld=lld",
                        "-fms-runtime-lib=dll", *objs, "-o", str(exe)],
                       cwd=ROOT, capture_output=True, text=True)
    if r.returncode:
        print(r.stdout + r.stderr)
        sys.exit("link failed")

    movie = MOVIES / f"{a.movie}.thp"
    frames = [f for f in a.frames.split(",") if f]
    args = [str(exe), str(movie), str(out)] + (["all"] if a.all else []) + frames
    r = subprocess.run(args, cwd=ROOT, capture_output=True, text=True, errors="replace")
    print(r.stdout.strip())
    if r.stderr.strip():
        print(r.stderr.strip())
    for f in frames:
        ppm = out / f"frame_{f}.ppm"
        if ppm.exists():
            png = out / f"{a.movie}_frame_{f}.png"
            ppm_to_png(ppm, png)
            ppm.unlink()
            print(f"png: {png}")
    sys.exit(r.returncode)


if __name__ == "__main__":
    main()
