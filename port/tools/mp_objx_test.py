"""Shared-objectives follow-ups (P1b, w8): automated multiplayer scenarios.

  python port/tools/mp_objx_test.py [--only NAME[,NAME]] [--port 7813] [--exe PATH]

Each scenario runs 2 instances on this machine (instance 0 hosts with shared objectives on,
instance 1 joins), silent and off-screen (SMS_PORT_AUDIO=0, SMS_PORT_OFFSCREEN=1), with isolated
user dirs under port/build_aurora/mp_objx/<scenario>/<i>/, drives them with the debug knobs of
port_mp_objectives_extra.cpp (SMS_PORT_MP_OBJX_TEST), port_mp_presence.cpp
(SMS_PORT_MP_PRESENCE_TEST: goto a stage) and port_mp_game.cpp (SMS_PORT_MP_DEBUG_COLLECT), and
checks the logs. Scenarios (see MULTIPLAYER.md "Shared objectives follow-ups (P1b, w8)"):
  redcoin   both in Ricco Harbor episode 6 (3,5, its red-coin Shine forgotten first: free mode owns
            every Shine, and the script starts no challenge for a Shine already got); the host
            presses the red-coin switch and answers its instruction talk:
            the guest's switch is pressed too, its talk is closed when the host's challenge starts,
            both get the 8 red coins and the timer, the host collects red coins, the guest is credited
  latejoin  the host starts the challenge; the guest enters the stage later and gets the
            press with the elapsed time (its red coins and timer are moved on)
  coins100  plaza: both at 99 coins; the host takes a run-time (pool) coin: the guest is credited
            (shared total 100) and its 100-coin Shine appears; the guest takes the Shine; with the
            banner option the host shows "Shine Get"
  blue      the host takes a plaza blue coin in scenario 2; the guest enters plaza scenario 8:
            that blue coin is flagged there and not out
  loadtime  a load-time coin claim goes through the host's arbitration (first claim wins)
Exit code 0 = every selected scenario passed.
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
EXE = ROOT / "port" / "build_aurora" / "sms_port.exe"
OUT = ROOT / "port" / "build_aurora" / "mp_objx"


def find_disc():
    for up in (ROOT.parent, ROOT.parents[1]):
        p = up / "dolphin_test" / "files"
        if p.exists():
            return p
    return ROOT.parent / "dolphin_test" / "files"


DISC = Path(os.environ.get("SMS_PORT_DISC", str(find_disc())))

# Frames are presented frames of each instance; the host starts ~6 s before the guest, so its
# counter runs ~360 frames ahead. Free mode reaches the plaza around frame 1000-1300.
SCENARIOS = {
    "redcoin": dict(seconds=120, env={
        "all": {"SMS_PORT_MP_PRESENCE_TEST": "Host:goto@1700=3,5;Guest1:goto@1400=3,5"},
        0: {"SMS_PORT_MP_OBJX_TEST": "Host:unshine@1500=15;Host:press@3200",
            "SMS_PORT_PADSCRIPT": "3500-4300/40:A",
            "SMS_PORT_MP_DEBUG_COLLECT": "4700,4800,4900:red"},
        1: {"SMS_PORT_MP_OBJX_TEST": "Guest1:unshine@1200=15;Guest1:toswitch@2600"}},
        checks=[(0, r"red-coin switch pressed here: sent"),
                (1, r"red-coin switch pressed here \(remote press\)"),
                (0, r"red-coin challenge started here: sent"),
                (1, r"Host's red-coin challenge started"),
                (1, r"red-coin challenge already running: instruction talk closed"),
                (0, r"red-coin challenge: 8 red coins out"),
                (1, r"red-coin challenge: 8 red coins out"),
                (1, r"\[mp\] objective [0-9a-f]+ from slot 0: applying"),
                (1, r"red-coin challenge: [0-7] red coins out, [1-8] collected")]),
    "latejoin": dict(seconds=120, env={
        "all": {"SMS_PORT_MP_PRESENCE_TEST": "Host:goto@1700=3,5;Guest1:goto@3600=3,5"},
        0: {"SMS_PORT_MP_OBJX_TEST": "Host:unshine@1500=15;Host:press@2700",
            "SMS_PORT_PADSCRIPT": "3000-3800/40:A"},
        1: {"SMS_PORT_MP_OBJX_TEST": "Guest1:unshine@1200=15;Guest1:toswitch@4300"}},
        checks=[(0, r"red-coin challenge started here: sent"),
                (0, r"entered stage 3/5 during a red-coin challenge \([1-9]\d* ms in\)"),
                (1, r"red-coin switch pressed here \(late join\)"),
                (1, r"red-coin challenge already running: instruction talk closed"),
                (1, r"joined late: \d+ red coins and the timer moved on by \d+ ms")]),
    "coins100": dict(seconds=120, env={
        "all": {"SMS_PORT_MP_OBJX_TEST": "Host:coins@2300=99;Guest1:coins@2000=99;Host:spawncoin@2700;Guest1:shine100@3000"},
        0: {"SMS_PORT_MP_OBJX_BANNER": "1"}},
        checks=[(0, r"run-time coin \d+ collected here: credit sent"),
                (1, r"run-time coin collected by Host: \+1 \(total 100\)"),
                (1, r"coin total reached 100: the 100-coin Shine appears"),
                (1, r"Mario to the 100-coin Shine"),
                (0, r"Guest1 got Shine \d+ in this stage: banner")]),
    "blue": dict(seconds=120, env={
        "all": {"SMS_PORT_MP_PRESENCE_TEST": "Guest1:goto@2600=1,8"},
        0: {"SMS_PORT_MP_DEBUG_COLLECT": "2200:blue"}},
        checks=[(0, r"\[mp\] objective [0-9a-f]+ collected here"),
                (1, r"stage 1/8: .*"),
                (1, r"blue coins here: \d+ out, [1-9]\d* already collected"),
                (-1, r"blue coin \d+ is out although its flag is set")]),
    "loadtime": dict(seconds=100, env={
        "all": {"SMS_PORT_MP_PRESENCE_TEST": "Host:goto@1700=3,5;Guest1:goto@1400=3,5",
                "SMS_PORT_MP_OBJX_TEST": "Guest1:loadcoin@2600;Host:loadcoin@3000"}},
        checks=[]),  # filled from the logs: see run()
}


def launch(scen, i, port, exe, env_extra):
    d = OUT / scen / str(i)
    d.mkdir(parents=True)
    env = {k: v for k, v in os.environ.items() if not k.startswith("SMS_PORT_")}
    env.update({
        "SMS_PORT_DISC": str(DISC), "SMS_PORT_OFFSCREEN": "1", "SMS_PORT_AUDIO": "0", "SMS_PORT_NO_PADS": "1",
        "SMS_PORT_USER_DIR": str(d / "user"), "SMS_PORT_CACHE_DIR": str(OUT / "cache" / str(i)),
        "SMS_PORT_SETTINGS": str(d / "port_settings.ini"), "SMS_PORT_MP_INI": str(d / "port_multiplayer.ini"),
        "SMS_PORT_THP_SPEED": "8", "SMS_PORT_MP_LOG": "1",
        "SMS_PORT_MP": f"host:{port}" if i == 0 else f"join:127.0.0.1:{port}",
        "SMS_PORT_MP_NAME": "Host" if i == 0 else f"Guest{i}",
    })
    if i == 0:
        env["SMS_PORT_MP_SHARED"] = "1"
    env.update(env_extra)
    # MP_OBJX_EXTRA="KEY=VALUE;..." adds env vars to every instance (debugging)
    for kv in os.environ.get("MP_OBJX_EXTRA", "").split(";"):
        if "=" in kv:
            k, v = kv.split("=", 1)
            env[k] = v
    log = open(d / "log.txt", "wb")
    return subprocess.Popen([str(exe)], cwd=d, env=env, stdout=log, stderr=subprocess.STDOUT,
                            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0)), d / "log.txt"


def run(name, sc, port, exe):
    if (OUT / name).exists():
        shutil.rmtree(OUT / name, ignore_errors=True)
    procs, logs = [], []
    for i in range(2):
        env = dict(sc["env"].get("all", {}))
        env.update(sc["env"].get(i, {}))
        p, log = launch(name, i, port, exe, env)
        procs.append(p)
        logs.append(log)
        time.sleep(6 if i == 0 else 1)
    t0 = time.time()
    while time.time() - t0 < sc["seconds"] and all(p.poll() is None for p in procs):
        time.sleep(1)
    crashed = [i for i, p in enumerate(procs) if p.poll() is not None]
    for p in procs:
        if p.poll() is None:
            p.kill()
            p.wait()
    texts = [l.read_text(encoding="utf-8", errors="replace") for l in logs]
    for i in range(2):
        shutil.rmtree(OUT / name / str(i) / "user", ignore_errors=True)
    checks = list(sc["checks"])
    if name == "loadtime":
        if "no load-time coin in this stage" in texts[0] + texts[1]:
            checks = [(-2, "the plaza has no load-time coin: arbitration not exercised")]
        else:
            checks = [(1, r"load-time \w+ coin \d+ collected here"),
                      (0, r"load-time \w+ coin \d+ collected by Guest1: credited")]
    ok = not crashed
    print(f"== {name}" + (f"  (instance(s) {crashed} exited early)" if crashed else ""))
    for who, pat in checks:
        if who == -2:
            print(f"   SKIP  {pat}")
            continue
        if who == -1:  # must not appear anywhere
            hit = any(re.search(pat, t) for t in texts)
            print(f"   {'FAIL' if hit else 'ok  '}  never: {pat}")
            ok &= not hit
            continue
        hit = re.search(pat, texts[who]) is not None
        print(f"   {'ok  ' if hit else 'FAIL'}  [{'host' if who == 0 else 'guest'}] {pat}")
        ok &= hit
    return ok


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--only", default="")
    ap.add_argument("--port", type=int, default=7813)
    ap.add_argument("--exe", default=str(EXE))
    ap.add_argument("--seconds", type=int, default=0, help="override every scenario's run time")
    ap.add_argument("--redstage", default="", help="stage,scenario with a red-coin switch for redcoin/latejoin")
    args = ap.parse_args()
    if args.redstage:
        for n in ("redcoin", "latejoin"):
            e = SCENARIOS[n]["env"]["all"]
            e["SMS_PORT_MP_PRESENCE_TEST"] = e["SMS_PORT_MP_PRESENCE_TEST"].replace("=20,0", "=" + args.redstage)
            SCENARIOS[n]["checks"] = [(w, p.replace("20/0", args.redstage.replace(",", "/"))) for w, p in SCENARIOS[n]["checks"]]
    if args.seconds:
        for sc in SCENARIOS.values():
            sc["seconds"] = args.seconds
    names = [n for n in SCENARIOS if not args.only or n in args.only.split(",")]
    results = {n: run(n, SCENARIOS[n], args.port, args.exe) for n in names}
    shutil.rmtree(OUT / "cache", ignore_errors=True)
    print("\n" + "  ".join(f"{n}: {'PASS' if r else 'FAIL'}" for n, r in results.items()))
    sys.exit(0 if all(results.values()) else 1)


if __name__ == "__main__":
    main()
