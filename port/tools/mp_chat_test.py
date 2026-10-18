"""Chat + emote test for multiplayer (w6, task board P3). Runs 2-3 sms_port.exe instances like
mp_test.py (instance 0 hosts on 127.0.0.1, the others join), but with a per-instance stick script,
a larger window and the chat/emote debug knobs, then checks the logs.

  python port/tools/mp_chat_test.py [-n 3] [--seconds 90] [--port 7796] [--shot 2000,2300] [--size 1280x960]

Script (absolute presented frames; each instance acts on its own local slot):
  - guests walk away from the spawn point (Guest1 right, Guest2 left) so everyone is in view;
  - chat: host and guests talk, one /me action, one emote command, a flood (rate limit),
    the input line and the on-screen keyboard open for screenshots;
  - emotes: host Shine pose (D-pad up through the keyboard pad: SMS_PORT_KEYSCRIPT "I"),
    Guest1 "Phew" via /phew, Guest2 aww via the emote knob;
  - host turns chat off at the end (lobby setting): later messages are refused.
Checks: every instance logged every other instance's chat lines, the emote lines, the
rate-limit refusal, and no crash. Shots/logs in port/build_aurora/mp_chat_test/<i>/.
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
OUT = ROOT / "port" / "build_aurora" / "mp_chat_test"

CHAT = "|".join([
    "0@1500:Hello from the host!",
    "1@1560:hi host, nice plaza",
    "2@1620:/me waves at everyone",
    "0@1700:!open:typing a message...",
    "0@1860:!close",
    "1@1700:!osk:hello from a controller",
    "1@1860:!close",
    "2@1700:!emotes",
    "2@1860:!close",
    "1@2000:/phew",
    "0@2400:spam 1", "0@2401:spam 2", "0@2440:spam 3", "0@2480:spam 4", "0@2520:spam 5", "0@2560:spam 6",
    "1@2600:/emotes",
    "0@3000:!chatoff",
    "1@3400:after chat off",
])
EMOTES = "2@2000:2|0@2700:4|1@2700:6"
KEYS = "2000:I,3100:F2"  # host only: D-pad up on the keyboard pad (quick emote), then F2 (lobby panel)
WALK = ["", "+60-85:S90", "+60-85:S270"]  # a few steps only (the fountain is near)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-n", type=int, default=3)
    ap.add_argument("--seconds", type=float, default=90)
    ap.add_argument("--port", type=int, default=7796)
    ap.add_argument("--shot", default="1500,1800,2100,2300,2800")
    ap.add_argument("--size", default="1280x960")
    ap.add_argument("--exe", default=str(EXE))
    a = ap.parse_args()
    w, h = a.size.split("x")
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
            # silent and hidden (user rule for every test instance)
            "SMS_PORT_DISC": str(DISC), "SMS_PORT_OFFSCREEN": "1", "SMS_PORT_AUDIO": "0", "SMS_PORT_NO_PADS": "1",
            "SMS_PORT_USER_DIR": str(d / "user"), "SMS_PORT_CACHE_DIR": str(d / "cache"),
            "SMS_PORT_SETTINGS": str(d / "port_settings.ini"), "SMS_PORT_MP_INI": str(d / "port_multiplayer.ini"),
            "SMS_PORT_THP_SPEED": "8",
            "SMS_PORT_MP": f"host:{a.port}" if i == 0 else f"join:127.0.0.1:{a.port}",
            "SMS_PORT_MP_NAME": "Host" if i == 0 else f"Guest{i}",
            "SMS_PORT_MP_CHAT_AUTO": CHAT, "SMS_PORT_MP_EMOTE_AUTO": EMOTES,
            "SMS_PORT_SHOT": a.shot, "SMS_PORT_SHOT_PRESENT": "1",
        })
        if WALK[i % len(WALK)]:
            env["SMS_PORT_PADSCRIPT"] = WALK[i % len(WALK)]
        if i == 0:
            env["SMS_PORT_KEYSCRIPT"] = KEYS
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

    names = ["Host"] + [f"Guest{i}" for i in range(1, a.n)]
    want_chat = {"Host": "Hello from the host!", "Guest1": "hi host, nice plaza", "Guest2": "waves at everyone"}
    want_emote = {"Host": "Shine pose", "Guest1": "Phew", "Guest2": "Aww"}
    for i, lp in enumerate(logs):
        t = lp.read_text(encoding="utf-8", errors="replace")
        me = names[i]
        check(alive[i], f"{me}: still running at the end")
        check("[port] CRASH" not in t and "[aurora FATAL" not in t, f"{me}: no crash/fatal")
        for other in names:
            if other == me:
                continue
            check(f"[mp] chat: {other}: {want_chat[other]}" in t, f"{me}: got {other}'s chat line")
            check(f"[mp] chat (emote): {other}: {want_emote[other]}" in t, f"{me}: saw {other}'s emote")
        if i == 0:
            check("Slow down" in t, "Host: rate limit refused the flood")
            spam = len(re.findall(r"\[mp\] chat: Host: spam \d", t))
            check(spam <= 5, f"Host: {spam} of 6 spam lines went out")
        if i == 1:
            check("The host turned chat off." in t, "Guest1: refused to send after the host turned chat off")
        check("after chat off" not in t, f"{me}: no chat line after the host turned chat off")
        mine = re.findall(r"\[mp\] emote (\d+) \(", t)
        check(len(mine) >= 1, f"{me}: played emotes {mine}")
        for png in sorted(lp.parent.glob("*.bmp")):
            bmp_to_png(png, png.with_suffix(".png"), 1)
            png.unlink()
        shutil.rmtree(lp.parent / "cache", ignore_errors=True)
    print("ALL PASSED" if ok else "SOME CHECKS FAILED", f"({time.time() - t0:.0f} s, logs in {OUT})")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
