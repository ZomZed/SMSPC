"""Build/run device-independent host surround-output regression tests."""
import subprocess
from pathlib import Path
import build_port

root = build_port.ROOT
exe = root / "port/build/test_audio_out.exe"
exe.parent.mkdir(parents=True, exist_ok=True)
subprocess.run([build_port.CLANG, "--target=x86_64-pc-windows-msvc", "-std=c++17",
                "-D_CRT_SECURE_NO_WARNINGS", "-O1", "-fuse-ld=lld",
                "port/tools/test_audio_out.cpp", "-o", str(exe)], cwd=root, check=True)
subprocess.run([str(exe)], cwd=root, check=True)
