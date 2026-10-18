"""Multiplayer smoke test (w10): runs 2-4 sms_port.exe instances on this machine and checks
that they find each other. See port/MULTIPLAYER.md.

  python port/tools/mp_test.py [-n 3] [--seconds 90] [--port 7790] [--kill-at 60]
                               [--shot 1800,2400] [--present] [--walk] [--testsnap] [--fuzz] [--nullgfx]

Instance 0 hosts (SMS_PORT_MP=host:<port>), the others join 127.0.0.1:<port> a few seconds
apart. Every instance runs off-screen with its own user dir, pipeline cache, settings and
port_multiplayer.ini under port/build_aurora/mp_test/<i>/, no controllers
(SMS_PORT_NO_PADS=1) and SMS_PORT_MP_LOG=1. Snapshots come from w15's capture (port_mp_puppet.cpp); --testsnap adds
port_mp_game.cpp's minimal snapshot for builds without it; --walk makes every
client walk in a different direction so positions change.
--kill-at <s> kills the last client after <s> seconds (an unclean disconnect): the host must
report the timeout and keep running.

Checks (from the logs): every instance reached free mode (stage 1), the host saw every join,
snapshots arrived on every instance from every other one, the remote stage is the plaza,
nobody crashed, and (with --kill-at) the host dropped the killed client.
Exit code 0 = all checks passed.
"""
import argparse
import os
import random
import re
import shutil
import socket
import struct
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).parent))
from smoke_all import DISC, bmp_to_png  # noqa: E402

EXE = ROOT / "port" / "build_aurora" / "sms_port.exe"
OUT = ROOT / "port" / "build_aurora" / "mp_test"


MAGIC = 0x31504D53  # "SMP1"
PROTO = 1


def header(ptype, slot, conn, seq, payload, proto=PROTO, magic=MAGIC, plen=None):
    plen = len(payload) if plen is None else plen
    return struct.pack("<IHBBIHH", magic, proto, ptype, slot, conn, seq, plen) + payload


def fuzz(port):
    """Sends garbage and malformed packets to the host, then a CONNECT with another protocol
    version (expects REJECT reason 1) and a DISCOVER (expects LOBBY_INFO)."""
    rnd = random.Random(1234)
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(0.5)
    dst = ("127.0.0.1", port)
    sent = 0
    for _ in range(3000):
        kind = rnd.randrange(6)
        if kind == 0:  # random bytes
            pkt = bytes(rnd.randrange(256) for _ in range(rnd.randrange(0, 1500)))
        elif kind == 1:  # valid magic, random type/length/payload
            pl = bytes(rnd.randrange(256) for _ in range(rnd.randrange(0, 200)))
            pkt = header(rnd.randrange(256), rnd.randrange(256), rnd.getrandbits(32), 0, pl,
                         plen=rnd.choice([len(pl), rnd.randrange(65536)]))
        elif kind == 2:  # truncated header
            pkt = header(rnd.randrange(1, 13), 0, 0, 0, b"")[:rnd.randrange(0, 16)]
        elif kind == 3:  # reliable/snapshot for a connection id that does not exist
            pl = bytes(rnd.randrange(256) for _ in range(rnd.randrange(0, 400)))
            pkt = header(rnd.choice([6, 7, 8, 12]), rnd.randrange(16), rnd.getrandbits(32), 0, pl)
        elif kind == 4:  # CONNECT with a short or garbage body
            pl = bytes(rnd.randrange(256) for _ in range(rnd.randrange(0, 60)))
            pkt = header(3, 0xFF, 0, 0, pl)
        else:  # wrong magic
            pkt = header(rnd.randrange(1, 13), 0, 0, 0, b"\0" * 20, magic=rnd.getrandbits(32))
        s.sendto(pkt, dst)
        sent += 1
    res = {"sent": sent, "reject_version": False, "lobby_info": False}

    def await_reply(pred, deadline=2.0):
        t_end = time.time() + deadline
        while time.time() < t_end:
            try:
                d, _ = s.recvfrom(2048)
            except socket.timeout:
                continue
            if pred(d):
                return True
        return False

    time.sleep(0.5)
    s.settimeout(0.05)
    while True:  # drop whatever the garbage provoked
        try:
            s.recvfrom(2048)
        except (socket.timeout, ConnectionResetError):
            break
    s.settimeout(0.2)
    body = struct.pack("<II", 0x4E494F4A, 0xABCD1234) + b"Fuzzer".ljust(24, b"\0") + b"\0" * 32
    s.sendto(header(3, 0xFF, 0, 0, body, proto=99), dst)
    res["reject_version"] = await_reply(lambda d: len(d) >= 21 and d[6] == 5 and d[20] == 1)
    s.sendto(header(1, 0xFF, 0, 0, struct.pack("<I", 0x5555)), dst)
    res["lobby_info"] = await_reply(lambda d: len(d) >= 16 and d[6] == 2)
    print(f"fuzz: sent {sent} malformed packets; {res}")
    return res


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-n", type=int, default=2, help="instances (2-4)")
    ap.add_argument("--seconds", type=float, default=90)
    ap.add_argument("--port", type=int, default=0, help="host port (default: random 20000-40000, so parallel test runs never meet)")
    ap.add_argument("--kill-at", type=float, default=0)
    ap.add_argument("--shot", default="", help="SMS_PORT_SHOT frames for every instance")
    ap.add_argument("--present", action="store_true", help="also SMS_PORT_SHOT_PRESENT=1")
    ap.add_argument("--walk", action="store_true")
    ap.add_argument("--host-pad", default="", help="SMS_PORT_PADSCRIPT for the host (e.g. 3300:START,3900:START to pause it for 10 s)")
    ap.add_argument("--testsnap", action="store_true",
                    help="SMS_PORT_MP_TESTSNAP=1 (w10's minimal snapshot; only for builds without w15's capture)")
    ap.add_argument("--fuzz", action="store_true",
                    help="40 s in: 3000 malformed packets at the host, a wrong-version CONNECT and a DISCOVER")
    ap.add_argument("--nullgfx", action="store_true", help="Aurora Null backend (no GPU; for 5-8 instances)")
    ap.add_argument("--migrate-at", type=int, default=0,
                    help="the host leaves at this frame (SMS_PORT_MP_LEAVE_AT): guest 1 must become the host and the others re-join it")
    ap.add_argument("--exe", default=str(EXE))
    args = ap.parse_args()
    n = max(2, min(args.n, 8))
    if args.port == 0:
        args.port = random.randint(20000, 40000)
    if OUT.exists():
        shutil.rmtree(OUT, ignore_errors=True)
    procs, logs = [], []
    t0 = time.time()
    # stick directions per client (PADSCRIPT S<deg>), from when Mario gets control
    walks = ["", "+60-400:S90,+600-900:S270", "+60-400:S0,+600-900:S180", "+60-400:S270,+600-900:S90",
             "+60-400:S180,+600-900:S0", "+60-400:S45", "+60-400:S135", "+60-400:S225"]
    for i in range(n):
        d = OUT / str(i)
        d.mkdir(parents=True)
        env = dict(os.environ)
        for k in ("SMS_PORT_WARP", "SMS_PORT_PADSCRIPT", "SMS_PORT_KEYSCRIPT", "SMS_PORT_SHOT", "SMS_PORT_MP"):
            env.pop(k, None)
        env.update({
            "SMS_PORT_DISC": str(DISC), "SMS_PORT_OFFSCREEN": "1", "SMS_PORT_AUDIO": "0", "SMS_PORT_NO_PADS": "1",
            "SMS_PORT_USER_DIR": str(d / "user"), "SMS_PORT_CACHE_DIR": str(d / "cache"),
            "SMS_PORT_SETTINGS": str(d / "port_settings.ini"), "SMS_PORT_MP_INI": str(d / "port_multiplayer.ini"),
            "SMS_PORT_THP_SPEED": "8", "SMS_PORT_MP_LOG": "1", "SMS_PORT_MP_TESTMSG": "1",
            "SMS_PORT_MP": f"host:{args.port}" if i == 0 else f"join:127.0.0.1:{args.port}",
            "SMS_PORT_MP_NAME": "Host" if i == 0 else f"Guest{i}",
        })
        if args.nullgfx:
            env["SMS_PORT_NULLGFX"] = "1"
        if args.migrate_at and i == 0:
            env["SMS_PORT_MP_LEAVE_AT"] = str(args.migrate_at)
        if args.testsnap:
            env["SMS_PORT_MP_TESTSNAP"] = "1"
        if args.walk and walks[i % len(walks)]:
            env["SMS_PORT_PADSCRIPT"] = walks[i % len(walks)]
        if i == 0 and args.host_pad:
            env["SMS_PORT_PADSCRIPT"] = args.host_pad
        if args.shot:
            env["SMS_PORT_SHOT"] = args.shot
            if args.present:
                env["SMS_PORT_SHOT_PRESENT"] = "1"
        log = open(d / "log.txt", "wb")
        logs.append(d / "log.txt")
        procs.append(subprocess.Popen([args.exe], cwd=d, env=env, stdout=log, stderr=subprocess.STDOUT,
                                      creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0)))
        time.sleep(6 if i == 0 else 3)
    killed = False
    exited = {}
    fuzz_result = None
    while time.time() - t0 < args.seconds:
        time.sleep(1)
        if args.fuzz and fuzz_result is None and time.time() - t0 >= 40:
            fuzz_result = fuzz(args.port)
        if args.kill_at and not killed and time.time() - t0 >= args.kill_at:
            procs[-1].kill()
            killed = True
            print(f"killed instance {n - 1} at {time.time() - t0:.0f} s")
        for i, p in enumerate(procs):
            if p.poll() is not None and i not in exited and not (killed and i == n - 1):
                exited[i] = p.returncode & 0xFFFFFFFF
    for p in procs:
        if p.poll() is None:
            p.kill()
            p.wait()

    ok = True

    def check(cond, msg):
        nonlocal ok
        print(("PASS " if cond else "FAIL ") + msg)
        ok = ok and cond

    texts = [l.read_text(encoding="utf-8", errors="replace") for l in logs]
    for i, t in enumerate(texts):
        name = "host" if i == 0 else f"guest {i}"
        if not (killed and i == n - 1):
            check(i not in exited, f"{name}: still running at the end" + (f" (exit {exited[i]:#x})" if i in exited else ""))
        check("[port] CRASH" not in t and "[aurora FATAL" not in t, f"{name}: no crash/fatal")
        check(re.search(r"TApplication state 5 \(area 1/\d+\)", t) is not None, f"{name}: reached Delfino Plaza")
        if i > 0:
            check("Joined \"" in t, f"{name}: handshake accepted")
        if args.nullgfx:
            env["SMS_PORT_NULLGFX"] = "1"
        if True:  # snapshots: w15's capture, or --testsnap
            remotes = set(int(m) for m in re.findall(r"\[mp\] remote (\d+) \(.*?\): tick \d+ stage 1/", t))
            want = set(range(n)) - {i}
            if killed and i != n - 1:
                want.discard(n - 1)
            check(want <= remotes, f"{name}: snapshots in the plaza from slots {sorted(want)} (got {sorted(remotes)})")
            msgs = set(int(m) for m in re.findall(r"\[mp\] testmsg from (\d+):", t))
            check(want <= msgs, f"{name}: app messages (PORT_MP_CH_TEST, relayed by the host) from slots {sorted(want)} (got {sorted(msgs)})")
    joins = len(re.findall(r"\[mp\] player \d+ joined:", texts[0]))
    check(joins == n - 1, f"host: saw {joins} joins (want exactly {n - 1})")
    if args.migrate_at:
        check("You are the new host" in texts[1], "guest 1: became the host after the host left (lowest slot)")
        for i in range(2, n):
            check("Back in the lobby with the new host" in texts[i], f"guest {i}: re-joined the new host")
            after = texts[i].split("Back in the lobby with the new host", 1)[-1]
            check(re.search(r"\[mp\] testmsg from 1:", after) is not None, f"guest {i}: messages from the new host")
    if killed:
        check(re.search(r"\[mp\] player \d+ \(.*?\) left: timed out", texts[0]) is not None,
              "host: dropped the killed client after the timeout")
        for i in range(1, n - 1):
            check(re.search(r"\[mp\] player \d+ left:", texts[i]) is not None, f"guest {i}: told about the leave")
    if args.fuzz:
        check(fuzz_result is not None and fuzz_result["reject_version"],
              "host: refused a CONNECT with another protocol version (REJECT reason 1)")
        check(fuzz_result is not None and fuzz_result["lobby_info"], "host: answered a DISCOVER after the garbage")
    if args.shot:
        for i in range(n):
            for b in sorted((OUT / str(i)).glob("*.bmp")):
                bmp_to_png(b, b.with_suffix(".png"))
                b.unlink()
    print("ALL PASSED" if ok else "SOME CHECKS FAILED", f"({time.time() - t0:.0f} s, logs in {OUT})")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
