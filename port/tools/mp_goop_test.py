"""Goop cleaning sync test (w6, MULTIPLAYER.md P8 goop). Runs 2-3 sms_port.exe instances
(silent, hidden: SMS_PORT_AUDIO=0, SMS_PORT_OFFSCREEN=1): instance 0 hosts with
SMS_PORT_MP_GOOP=1, everyone goes to the same stage with w11's presence knob, the host sprays
FLUDD in a slow circle, Guest1 stands still, Guest2 (with -n 3) arrives late (snapshot path).
Then compares the per-layer pollution degrees (the game's pixel counters) the instances log.

  python port/tools/mp_goop_test.py [-n 3] [--stage 0,1|2,0] [--seconds 110] [--port 7799] [--shot 3000,3600]

Shots and logs in port/build_aurora/mp_goop_test/<i>/.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).parent))
from smoke_all import DISC, bmp_to_png  # noqa: E402

EXE = ROOT / "port" / "build_aurora" / "sms_port.exe"
OUT = ROOT / "port" / "build_aurora" / "mp_goop_test"
# host: FLUDD (R) while walking slowly in a square, from frame 2100 (the stage is loaded by then)
SPRAY = ",".join(f"{2100 + i * 200}-{2300 + i * 200}:S{(i * 90) % 360}+R" for i in range(8))
# other stages: Mario is put next to the goop at 2400 (SMS_PORT_MP_GOOP_TEST), then sprays around
SPRAY_NEAR = ",".join(f"{2500 + i * 150}-{2650 + i * 150}:S{(i * 60) % 360}+RA200" for i in range(10))
# airstrip: walk to the goop first (w8's BOOTLOG E6: goop around (-1882, 1136)), then spray around
SPRAY_AIRSTRIP = "2100-2700:G-1500;900," + ",".join(
    f"{2700 + i * 150}-{2850 + i * 150}:S{(i * 60) % 360}+RA200" for i in range(10))
LOG_RE = re.compile(r"\[mp\] goop: stage (\d+)/(\d+) frame (\d+), sync (\d), sent (\d+) recv (\d+) injected (\d+) "
                    r"dropped (\d+), snapshot cells (\d+), degrees([ \d]*), total (\d+)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-n", type=int, default=3)
    ap.add_argument("--stage", default="0,1")
    ap.add_argument("--seconds", type=float, default=110)
    ap.add_argument("--port", type=int, default=7799)
    ap.add_argument("--shot", default="")
    ap.add_argument("--size", default="960x720")
    ap.add_argument("--exe", default=str(EXE))
    ap.add_argument("--dump", default="4700", help="SMS_PORT_MP_GOOP_DUMP frame (maps compared texel by texel)")
    ap.add_argument("--backend", default="", help="SMS_PORT_BACKEND for every instance (e.g. vulkan)")
    a = ap.parse_args()
    w, h = a.size.split("x")
    names = ["Host"] + [f"Guest{i}" for i in range(1, a.n)]
    goto = ";".join(f"{nm}:goto@{1500 if i < 2 else 3900}={a.stage}" for i, nm in enumerate(names))
    if OUT.exists():
        shutil.rmtree(OUT, ignore_errors=True)
    procs, logs = [], []
    t0 = time.time()
    for i in range(a.n):
        d = OUT / str(i)
        d.mkdir(parents=True)
        (d / "port_settings.ini").write_text(f"[window]\nwidth = {w}\nheight = {h}\n")
        env = dict(os.environ)
        for k in ("SMS_PORT_WARP", "SMS_PORT_PADSCRIPT", "SMS_PORT_KEYSCRIPT", "SMS_PORT_SHOT", "SMS_PORT_MP"):
            env.pop(k, None)
        env.update({
            "SMS_PORT_DISC": str(DISC), "SMS_PORT_OFFSCREEN": "1", "SMS_PORT_AUDIO": "0", "SMS_PORT_NO_PADS": "1",
            "SMS_PORT_USER_DIR": str(d / "user"), "SMS_PORT_CACHE_DIR": str(d / "cache"),
            "SMS_PORT_SETTINGS": str(d / "port_settings.ini"), "SMS_PORT_MP_INI": str(d / "port_multiplayer.ini"),
            "SMS_PORT_THP_SPEED": "8",
            "SMS_PORT_MP": f"host:{a.port}" if i == 0 else f"join:127.0.0.1:{a.port}",
            "SMS_PORT_MP_NAME": names[i],
            "SMS_PORT_MP_PRESENCE_TEST": goto, "SMS_PORT_MP_GOOP_LOG": "1", "SMS_PORT_MP_GOOP_DUMP": a.dump,
        })
        if a.backend:
            env["SMS_PORT_BACKEND"] = a.backend
        if i == 0:
            env["SMS_PORT_MP_GOOP"] = "1"
            env["SMS_PORT_PADSCRIPT"] = SPRAY_AIRSTRIP if a.stage.startswith("0,") else SPRAY_NEAR
            if not a.stage.startswith("0,"):
                env["SMS_PORT_MP_GOOP_TEST"] = "0@2400"  # other stages: put Mario next to the goop
        elif i == 1 and a.stage.startswith("0,"):
            env["SMS_PORT_PADSCRIPT"] = "4000-4500:G-1500;900"  # Guest1 walks over to look at the cleaned patch
        if a.shot:
            env["SMS_PORT_SHOT"] = a.shot
            env["SMS_PORT_SHOT_PRESENT"] = "1"
        log = open(d / "log.txt", "wb")
        logs.append(d / "log.txt")
        procs.append(subprocess.Popen([a.exe], cwd=d, env=env, stdout=log, stderr=subprocess.STDOUT,
                                      creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0)))
        time.sleep(6 if i == 0 else 3)
    while time.time() - t0 < a.seconds:
        time.sleep(1)
    alive = [p.poll() is None for p in procs]
    for p in procs:
        if p.poll() is None:
            p.kill()
            p.wait()
    ok = True

    def check(c, msg):
        nonlocal ok
        print(("PASS " if c else "FAIL ") + msg)
        ok &= bool(c)

    # strict map/degree checks on the airstrip; elsewhere each client's own enemies add goop
    # (not synced unless enemy sync owns them), so those numbers are informational
    strict = a.stage.startswith("0,")
    last = {}
    maps = {}
    for i, lp in enumerate(logs):
        t = lp.read_text(encoding="utf-8", errors="replace")
        check(alive[i], f"{names[i]}: still running at the end")
        check("[port] CRASH" not in t and "[aurora FATAL" not in t, f"{names[i]}: no crash/fatal")
        rows = [m for m in LOG_RE.finditer(t) if m.group(1) + "," + m.group(2) == a.stage]
        if rows:
            m = rows[-1]
            degrees = [int(x) for x in m.group(10).split()]
            last[i] = dict(sent=int(m.group(5)), recv=int(m.group(6)), inj=int(m.group(7)), snap=int(m.group(9)),
                           degrees=degrees, total=int(m.group(11)), first=[int(x) for x in rows[0].group(10).split()])
            print(f"  {names[i]}: first degrees {last[i]['first']}, last {degrees} (total {last[i]['total']}), "
                  f"sent {last[i]['sent']} recv {last[i]['recv']} injected {last[i]['inj']} snapshot cells {last[i]['snap']}")
        check(i in last, f"{names[i]}: logged goop state in stage {a.stage}")
        maps[i] = {p.name: p.read_bytes()[54:] for p in lp.parent.glob("goop_*.bmp")}
        for png in sorted(lp.parent.glob("*.bmp")):
            bmp_to_png(png, png.with_suffix(".png"), 1)
            png.unlink()
        shutil.rmtree(lp.parent / "cache", ignore_errors=True)
        shutil.rmtree(lp.parent / "user", ignore_errors=True)
    if 0 in last:
        check(last[0]["sent"] > 0, "Host: sent cleaning stamps")
        if strict:
            check(last[0]["total"] < sum(x for x in last[0]["first"] if x >= 0), "Host: goop was cleaned")
        for i in range(1, a.n):
            if i not in last:
                continue
            check(last[i]["inj"] > 0 or last[i]["snap"] > 0, f"{names[i]}: applied the host's cleaning")
            d0, di = last[0]["total"], last[i]["total"]
            diff = abs(d0 - di) / max(1, d0)
            msg = f"{names[i]}: total degree {di} vs host {d0} ({100 * diff:.1f} % apart)"
            check(diff < 0.03, msg) if strict else print("INFO " + msg)
    for i in range(1, a.n):
        for name, data in maps.get(0, {}).items():
            other = maps.get(i, {}).get(name)
            if other is None or len(other) != len(data):
                continue
            px = range(0, len(data), 3)
            diff = sum(1 for k in px if data[k] != other[k])
            pol0 = sum(1 for k in px if data[k] > 30)
            mism = sum(1 for k in px if (data[k] > 30) != (other[k] > 30))
            print(f"  {names[i]} vs Host {name}: {diff} texels differ, polluted {pol0} on the host, "
                  f"{mism} texels polluted on one side only")
            msg = f"{names[i]}: map {name} matches the host's (polluted texels)"
            check(mism <= max(20, pol0 // 100), msg) if strict else print("INFO " + msg)
    print("ALL PASSED" if ok else "SOME CHECKS FAILED", f"({time.time() - t0:.0f} s, logs in {OUT})")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
