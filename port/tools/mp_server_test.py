"""Lobby-server test (w10): runs port/server/sms_lobby_server.py on 127.0.0.1 and several
sms_port.exe instances against it, then checks their logs. See port/MULTIPLAYER.md.

  python port/tools/mp_server_test.py [--phase 1|2|all] [--gfx]

Phase 1 (server):
  H1 hosts "Bob's Lobby" by name; H2 tries "BOB'S LOBBY" (must get "name taken");
  G1 joins "bob's lobby" (case-insensitive; direct path after the rendezvous);
  G2 joins it with SMS_PORT_MP_FORCE_RELAY=1 (through the server's relay);
  G3 looks for "No Such Lobby" (must fail with "No lobby called");
  H1 is killed: after the 20 s timeout G1 becomes the host and takes the name over (migration).
Phase 2 (no server running, LAN fallback, password, leave):
  H3 hosts "Home Lobby" with password "pw" (server address points nowhere); G4 joins
  "home lobby" with "pw" over the LAN lookup; G5 tries the wrong password (refused);
  H3 leaves at a set frame (G4 gets "host closed").
Instances run off-screen with null graphics (--gfx: real D3D12), isolated user dirs,
SMS_PORT_NO_PADS=1. Exit code 0 = all checks passed.
"""
import argparse
import os
import random
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).parent))
from smoke_all import DISC  # noqa: E402

EXE = ROOT / "port" / "build_aurora" / "sms_port.exe"
SERVER = ROOT / "port" / "server" / "sms_lobby_server.py"
OUT = ROOT / "port" / "build_aurora" / "mp_server_test"
SRV_PORT = random.randint(20000, 40000)  # parallel test runs never share a server


class Run:
    def __init__(self, gfx):
        self.gfx, self.procs, self.ok = gfx, {}, True

    def start(self, name, env_extra):
        d = OUT / name
        d.mkdir(parents=True, exist_ok=True)
        env = dict(os.environ)
        for k in list(env):
            if k.startswith("SMS_PORT_"):
                del env[k]
        env.update({
            "SMS_PORT_DISC": str(DISC), "SMS_PORT_OFFSCREEN": "1", "SMS_PORT_AUDIO": "0", "SMS_PORT_NO_PADS": "1",
            "SMS_PORT_USER_DIR": str(d / "user"), "SMS_PORT_CACHE_DIR": str(d / "cache"),
            "SMS_PORT_SETTINGS": str(d / "port_settings.ini"), "SMS_PORT_MP_INI": str(d / "port_multiplayer.ini"),
            "SMS_PORT_THP_SPEED": "8", "SMS_PORT_MP_LOG": "1", "SMS_PORT_MP_NAME": name,
        })
        if not self.gfx:
            env["SMS_PORT_NULLGFX"] = "1"
        env.update(env_extra)
        f = open(d / "log.txt", "wb")
        self.procs[name] = subprocess.Popen([str(EXE)], cwd=d, env=env, stdout=f, stderr=subprocess.STDOUT,
                                            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))

    def kill(self, name):
        p = self.procs.get(name)
        if p and p.poll() is None:
            p.kill()
            p.wait()

    def kill_all(self):
        for n in list(self.procs):
            self.kill(n)

    def log(self, name):
        return (OUT / name / "log.txt").read_text(encoding="utf-8", errors="replace")

    def check(self, cond, msg):
        print(("PASS " if cond else "FAIL ") + msg)
        self.ok = self.ok and cond


def start_server(extra=()):
    f = open(OUT / "server.log", "ab")
    return subprocess.Popen([sys.executable, "-u", str(SERVER), "--bind", "127.0.0.1", "--port", str(SRV_PORT),
                             "--stats-every", "20", "-v", *extra], stdout=f, stderr=subprocess.STDOUT,
                            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))


def phase1(r):
    srv = start_server()
    server = {"SMS_PORT_MP_SERVER": f"127.0.0.1:{SRV_PORT}"}
    t0 = time.time()
    r.start("H1", {**server, "SMS_PORT_MP": "hostname:Bob's Lobby", "SMS_PORT_MP_TESTMSG": "1"})
    time.sleep(8)
    r.start("H2", {**server, "SMS_PORT_MP": "hostname:BOB'S LOBBY"})
    time.sleep(4)
    r.start("G1", {**server, "SMS_PORT_MP": "joinname:bob's lobby", "SMS_PORT_MP_TESTMSG": "1"})
    time.sleep(3)
    r.start("G2", {**server, "SMS_PORT_MP": "joinname:  Bob's   Lobby ", "SMS_PORT_MP_FORCE_RELAY": "1",
                   "SMS_PORT_MP_TESTMSG": "1"})
    time.sleep(3)
    r.start("G3", {**server, "SMS_PORT_MP": "joinname:No Such Lobby"})
    while time.time() - t0 < 75:
        time.sleep(1)
    r.kill("H1")
    t_kill = time.time()
    while time.time() - t_kill < 32:
        time.sleep(1)
    r.kill_all()
    srv.kill()
    srv.wait()
    slog = (OUT / "server.log").read_text(encoding="utf-8", errors="replace")
    h1, h2, g1, g2, g3 = (r.log(n) for n in ("H1", "H2", "G1", "G2", "G3"))
    r.check("is open: players join by typing its name" in h1, "H1: lobby registered on the server")
    r.check("is already taken" in h2, "H2: duplicate name (other case) refused: name taken")
    r.check(re.search(r"Joined \"Bob's Lobby\"", g1) is not None, "G1: joined by name (case-insensitive)")
    r.check("relaying through the lobby server" not in g1, "G1: direct path (no relay)")
    r.check(re.search(r"Joined \"Bob's Lobby\"", g2) is not None and "relaying through the lobby server" in g2,
            "G2: joined through the relay (forced)")
    r.check(re.search(r"relay [0-9a-f]{8} opened", slog) is not None, "server: relay session opened")
    for n, t in (("G1", g1), ("G2", g2)):
        r.check(re.search(r"\[mp\] testmsg from 0:", t) is not None, f"{n}: app messages from the host arrive")
    r.check(re.search(r"\[mp\] testmsg from \d: hello \d+ from G2", g1) is not None,
            "G1: G2's messages arrive (host relays relayed traffic)")
    r.check("No lobby called \"No Such Lobby\"" in g3, "G3: unknown name refused")
    r.check("Lost the connection to the host" in g1 and "You are the new host" in g1,
            "G1: noticed the host is gone and took over (lowest slot)")
    r.check("'Bob's Lobby' taken over" in slog, "server: the name moved to G1")
    opened = [l for l in slog.splitlines() if " opened by " in l]
    r.check(bool(opened) and all("127.0.0.1" not in l for l in opened), "server: no client addresses in the log by default")


def phase2(r):
    nowhere = {"SMS_PORT_MP_SERVER": f"127.0.0.1:{SRV_PORT}"}  # nothing listens there now
    t0 = time.time()
    r.start("H3", {**nowhere, "SMS_PORT_MP": "hostname:Home Lobby", "SMS_PORT_MP_PASSWORD": "pw",
                   "SMS_PORT_MP_LEAVE_AT": "3600"})
    time.sleep(8)
    r.start("G4", {**nowhere, "SMS_PORT_MP": "joinname:home lobby", "SMS_PORT_MP_PASSWORD": "pw"})
    time.sleep(3)
    r.start("G5", {**nowhere, "SMS_PORT_MP": "joinname:Home Lobby", "SMS_PORT_MP_PASSWORD": "wrong"})
    while time.time() - t0 < 90:
        time.sleep(1)
    r.kill_all()
    h3, g4, g5 = (r.log(n) for n in ("H3", "G4", "G5"))
    r.check("can be joined on this network only" in h3, "H3: server offline -> LAN-only lobby")
    r.check("trying LAN" in g4 and re.search(r"Joined \"Home Lobby\"", g4) is not None,
            "G4: joined by name over the LAN lookup (no server)")
    r.check("Wrong password" in g5, "G5: wrong password refused")
    r.check("The host left. The lobby has ended." in g4, "G4: told when the host left (nobody else to take over)")


def unit(r):
    """Server alone (no game): registration rules, limits, version, oversize, expiry, relay."""
    import socket
    import struct
    srv = start_server(["--max-per-ip", "3"])
    time.sleep(1.0)
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(1.0)
    dst = ("127.0.0.1", SRV_PORT)

    def pkt(t, body, ver=1):
        return struct.pack("<IBB", 0x534C4D53, ver, t) + body

    def reg(name, nonce, sock=s):
        body = struct.pack("<IHBBBBQ", nonce, 3, 8, 1, 0, 0, 0) + name.encode().ljust(32, b"\0") + b"Host".ljust(24, b"\0") + b"\0"
        sock.sendto(pkt(1, body), dst)
        d, _ = sock.recvfrom(2048)
        return d[5], d

    t1, d1 = reg("Alpha", 1)
    r.check(t1 == 2, "unit: register 'Alpha'")
    lid, token = struct.unpack_from("<II", d1, 10)
    t2, d2 = reg("ALPHA", 2, socket.socket(socket.AF_INET, socket.SOCK_DGRAM))
    r.check(t2 == 3 and d2[6] == 2, "unit: 'ALPHA' from another socket -> name taken")
    t3, _ = reg("Alpha", 3)
    r.check(t3 == 2, "unit: the same host re-registering gets its lobby back")
    reg("Beta", 4)
    reg("Gamma", 5)
    t6, d6 = reg("Delta", 6)
    r.check(t6 == 3 and d6[6] == 4, "unit: 4th lobby from one address -> limit")
    s.sendto(pkt(1, b"\0" * 40, ver=9), dst)
    d, _ = s.recvfrom(2048)
    r.check(d[5] == 3 and d[6] == 1, "unit: wrong server protocol version -> reject(version)")
    s.sendto(pkt(12, struct.pack("<I", 77)) + b"\0" * 1500, dst)
    try:
        s.recvfrom(2048)
        big = False
    except socket.timeout:
        big = True
    r.check(big, "unit: oversize datagram dropped")
    # lookup with the wrong game protocol and a wrong password
    c = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    c.settimeout(1.0)
    c.sendto(pkt(6, struct.pack("<IHQ", 9, 2, 0) + b"alpha".ljust(32, b"\0")), dst)
    d, _ = c.recvfrom(2048)
    r.check(d[5] == 3 and d[6] == 1, "unit: lookup with another game version -> reject(version)")
    c.sendto(pkt(6, struct.pack("<IHQ", 10, 3, 0) + b"  alpha ".ljust(32, b"\0")), dst)
    d, _ = c.recvfrom(2048)
    r.check(d[5] == 7, "unit: lookup '  alpha ' finds 'Alpha'")
    p, _ = s.recvfrom(2048)
    r.check(p[5] == 8, "unit: the host got PUNCH with the client's endpoint")
    # relay both ways
    c.sendto(pkt(9, struct.pack("<II", 11, lid)), dst)
    d, _ = c.recvfrom(2048)
    rid = struct.unpack_from("<II", d, 6)[1]
    c.sendto(pkt(11, struct.pack("<I", rid) + b"hello-host"), dst)
    d, _ = s.recvfrom(2048)
    ok1 = d[5] == 11 and d[10:] == b"hello-host"
    s.sendto(pkt(11, struct.pack("<I", rid) + b"hello-client"), dst)
    d, _ = c.recvfrom(2048)
    r.check(ok1 and d[5] == 11 and d[10:] == b"hello-client", "unit: relay forwards both ways")
    x = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    x.settimeout(0.5)
    x.sendto(pkt(11, struct.pack("<I", rid) + b"intruder"), dst)
    try:
        s.settimeout(0.5)
        d, _ = s.recvfrom(2048)
        r.check(d[10:] != b"intruder", "unit: relay ignores a third address")
    except socket.timeout:
        r.check(True, "unit: relay ignores a third address")
    # unregister frees the name
    s.sendto(pkt(5, struct.pack("<II", lid, token)), dst)
    time.sleep(0.3)
    t7, _ = reg("alpha", 12, socket.socket(socket.AF_INET, socket.SOCK_DGRAM))
    r.check(t7 == 2, "unit: name free again after the host unregistered")
    srv.kill()
    srv.wait()

def phase3(r):
    """The name frees up as soon as the host leaves (UNREGISTER), not only after expiry."""
    srv = start_server()
    server = {"SMS_PORT_MP_SERVER": f"127.0.0.1:{SRV_PORT}"}
    r.start("H6", {**server, "SMS_PORT_MP": "hostname:Reuse Me", "SMS_PORT_MP_LEAVE_AT": "1200"})
    t0 = time.time()
    while "SMS_PORT_MP_LEAVE_AT: leaving" not in r.log("H6") and time.time() - t0 < 90:
        time.sleep(1)
    t_left = time.time()
    r.start("H7", {**server, "SMS_PORT_MP": "hostname:reuse me"})
    while time.time() - t_left < 25:
        time.sleep(1)
    r.kill("H7")  # a host alone vanishing: the server must expire its lobby
    t_kill = time.time()
    while time.time() - t_kill < 36:
        time.sleep(1)
    r.kill_all()
    srv.kill()
    srv.wait()
    slog = (OUT / "server.log").read_text(encoding="utf-8", errors="replace")
    r.check("'Reuse Me' closed (host left)" in slog, "server: H6's lobby closed when its host left")
    r.check("is open: players join by typing its name" in r.log("H7"), "H7: the same name (other case) registers right after")
    r.check(re.search(r"'[Rr]euse [Mm]e' closed \(expired\)", slog) is not None, "server: lobby expired 30 s after its lone host vanished")

def phase4(r):
    """Host migration in server mode: the host vanishes (killed); after the 20 s timeout the
    lowest slot takes the lobby name over (TAKEOVER) and the other guest re-joins it by name."""
    srv = start_server()
    server = {"SMS_PORT_MP_SERVER": f"127.0.0.1:{SRV_PORT}", "SMS_PORT_MP_TESTMSG": "1"}
    t0 = time.time()
    r.start("H8", {**server, "SMS_PORT_MP": "hostname:Migrate Me"})
    time.sleep(8)
    r.start("G8", {**server, "SMS_PORT_MP": "joinname:migrate me"})
    time.sleep(3)
    r.start("G9", {**server, "SMS_PORT_MP": "joinname:migrate me"})
    while time.time() - t0 < 60:
        time.sleep(1)
    r.kill("H8")
    t_kill = time.time()
    while time.time() - t_kill < 50:
        time.sleep(1)
    r.kill_all()
    srv.kill()
    srv.wait()
    slog = (OUT / "server.log").read_text(encoding="utf-8", errors="replace")
    g8, g9 = r.log("G8"), r.log("G9")
    r.check("You are the new host" in g8, "G8: became the new host (lowest slot)")
    r.check("'Migrate Me' taken over" in slog, "server: the lobby name moved to the new host (TAKEOVER)")
    r.check("Back in the lobby with the new host" in g9, "G9: re-joined the new host by name")
    after = g9.split("Back in the lobby with the new host", 1)[-1]
    r.check(re.search(r"\[mp\] testmsg from 1:", after) is not None, "G9: messages from the new host after the change")
    r.check("re-joined after the host change" in g8, "G8: G9 kept its slot")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--phase", default="all", choices=["unit", "1", "2", "3", "4", "all"])
    ap.add_argument("--gfx", action="store_true")
    args = ap.parse_args()
    if OUT.exists():
        shutil.rmtree(OUT, ignore_errors=True)
    OUT.mkdir(parents=True)
    r = Run(args.gfx)
    try:
        if args.phase in ("unit", "all"):
            unit(r)
        if args.phase in ("1", "all"):
            phase1(r)
        if args.phase in ("2", "all"):
            phase2(r)
        if args.phase in ("3", "all"):
            phase3(r)
        if args.phase in ("4", "all"):
            phase4(r)
    finally:
        r.kill_all()
        for c in OUT.glob("*/cache"):
            shutil.rmtree(c, ignore_errors=True)
    print("ALL PASSED" if r.ok else "SOME CHECKS FAILED", f"(logs in {OUT})")
    return 0 if r.ok else 1


if __name__ == "__main__":
    sys.exit(main())
