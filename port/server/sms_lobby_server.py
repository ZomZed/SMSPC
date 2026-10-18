#!/usr/bin/env python3
"""Super Mario Sunshine PC port: lobby, rendezvous and relay server (w10).

One UDP port, no dependencies (Python 3.8+ standard library), no persistent data.
  - Lobby registry: hosts register a lobby name (unique, case-insensitive) and keep it
    alive with heartbeats; a lobby expires 30 s after its last heartbeat.
  - Rendezvous: a client looks a name up; the server tells it the host's observed
    (public) endpoint and the host's LAN endpoints, and tells the host the client's
    observed endpoint, so both send to each other at once (UDP hole punching).
  - Relay: when punching fails, the client opens a relay session and the server forwards
    the datagrams between client and host (sessions expire after 30 s idle).
  - Limits: packet size cap, per-IP request rate (token bucket), lobbies per IP, total
    lobbies, relay sessions, relay bandwidth per session.
  - Logs: lobby ids, names and counts; client addresses only with --log-addresses.

Wire format (little-endian; see port/MULTIPLAYER.md "Lobby server"):
  u32 magic 'SMLS' (0x534C4D53), u8 server protocol version, u8 type, then the body.

Usage:  python3 sms_lobby_server.py [--port 7700] [--bind 0.0.0.0] [--no-relay] [-v]
"""
import argparse
import hashlib
import os
import random
import select
import signal
import socket
import struct
import sys
import time

MAGIC = 0x534C4D53  # "SMLS"
SERVER_PROTO = 1
MAX_PACKET = 1450  # a relayed game datagram (<= 1400) plus 10 bytes of framing

# types
T_REGISTER, T_REGISTERED, T_REJECT, T_HEARTBEAT, T_UNREGISTER, T_LOOKUP, T_FOUND, T_PUNCH, \
    T_RELAY_OPEN, T_RELAY_OK, T_RELAY_DATA, T_STATUS, T_STATUS_OK, T_TAKEOVER, T_LIST, T_LIST_OK = range(1, 17)
# reject reasons
R_VERSION, R_NAME_TAKEN, R_BAD_NAME, R_LIMIT, R_NOT_FOUND, R_BAD_PASSWORD, R_FULL, R_RATE, \
    R_NO_RELAY = range(1, 10)
REASON = {R_VERSION: "version", R_NAME_TAKEN: "name taken", R_BAD_NAME: "bad name", R_LIMIT: "limit",
          R_NOT_FOUND: "not found", R_BAD_PASSWORD: "bad password", R_FULL: "full", R_RATE: "rate",
          R_NO_RELAY: "no relay"}

LOBBY_TTL = 30.0  # longer than the game's 20 s peer timeout, so a new host can take the name over
RELAY_TTL = 30.0
NAME_MAX = 32
HOSTNAME_MAX = 24
LIST_MAX = 20  # lobbies per LIST_OK (20 x 60 bytes fits one datagram)


def now():
    return time.monotonic()


def cstr(b):
    b = b.split(b"\0", 1)[0]
    return b.decode("utf-8", "replace")


def clean_name(raw):
    """Printable, trimmed, 1..31 UTF-8 bytes; None when unusable."""
    s = "".join(ch for ch in raw if ch.isprintable()).strip()
    if not s or len(s.encode("utf-8")) > NAME_MAX - 1:
        return None
    return s


def name_key(s):
    return " ".join(s.casefold().split())


def ip_tag(addr):
    """A short non-reversible tag for logs (no addresses in logs by default)."""
    return hashlib.sha256(("sms:" + addr[0]).encode()).hexdigest()[:6]


class Bucket:
    def __init__(self, rate, burst):
        self.rate, self.burst, self.tokens, self.t = rate, burst, burst, now()

    def take(self, n=1.0):
        t = now()
        self.tokens = min(self.burst, self.tokens + (t - self.t) * self.rate)
        self.t = t
        if self.tokens >= n:
            self.tokens -= n
            return True
        return False


class Lobby:
    __slots__ = ("id", "token", "name", "key", "addr", "local", "game_proto", "max_players", "players",
                 "mode", "pw", "has_pw", "host_name", "last", "created")


class Relay:
    __slots__ = ("id", "lobby", "client", "host", "last", "bucket_c", "bucket_h", "bytes")


class Server:
    def __init__(self, args):
        self.args = args
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind((args.bind, args.port))
        self.sock.setblocking(False)
        self.lobbies = {}      # id -> Lobby
        self.by_key = {}       # name key -> Lobby
        self.relays = {}       # id -> Relay
        self.buckets = {}      # ip -> Bucket
        self.stats = {"rx": 0, "tx": 0, "drop": 0, "relayed": 0}
        self.last_stats = now()
        self.rng = random.SystemRandom()
        self.log(f"listening on UDP {args.bind}:{args.port} (relay {'off' if args.no_relay else 'on'})")

    # -- helpers --------------------------------------------------------------------
    def log(self, msg):
        print(time.strftime("%Y-%m-%d %H:%M:%S ") + msg, flush=True)

    def who(self, addr):
        return f"{addr[0]}:{addr[1]}" if self.args.log_addresses else ip_tag(addr)

    def send(self, addr, ptype, body=b""):
        pkt = struct.pack("<IBB", MAGIC, SERVER_PROTO, ptype) + body
        try:
            self.sock.sendto(pkt, addr)
            self.stats["tx"] += 1
        except OSError:
            pass

    def observed(self, addr):
        """The sender's address as this server sees it (its public endpoint), u32 BE + u16 BE."""
        return struct.pack(">IH", struct.unpack(">I", socket.inet_aton(addr[0]))[0], addr[1])

    def reject(self, addr, reason, req, nonce=0):
        self.send(addr, T_REJECT, struct.pack("<BBI", reason, req, nonce))

    def new_id(self, table):
        while True:
            i = self.rng.getrandbits(32)
            if i and i not in table:
                return i

    def drop_lobby(self, lb, why):
        self.lobbies.pop(lb.id, None)
        if self.by_key.get(lb.key) is lb:
            del self.by_key[lb.key]
        for rid in [r.id for r in self.relays.values() if r.lobby == lb.id]:
            del self.relays[rid]
        self.log(f"lobby {lb.id:08x} '{lb.name}' closed ({why}); {len(self.lobbies)} open")

    # -- packets ----------------------------------------------------------------------
    def handle(self, data, addr):
        self.stats["rx"] += 1
        if len(data) < 6 or len(data) > MAX_PACKET:
            self.stats["drop"] += 1
            return
        magic, ver, ptype = struct.unpack_from("<IBB", data)
        if magic != MAGIC:
            self.stats["drop"] += 1
            return
        body = data[6:]
        # Relay data has its own (bandwidth) limit; everything else the per-IP rate.
        if ptype == T_RELAY_DATA:
            return self.relay_data(body, addr)
        b = self.buckets.get(addr[0])
        if b is None:
            if len(self.buckets) > 20000:
                self.buckets.clear()
            b = self.buckets[addr[0]] = Bucket(self.args.rate, self.args.rate * 3)
        if not b.take():
            self.stats["drop"] += 1
            return
        if ver != SERVER_PROTO:
            return self.reject(addr, R_VERSION, ptype)
        try:
            {T_REGISTER: self.register, T_HEARTBEAT: self.heartbeat, T_UNREGISTER: self.unregister,
             T_LOOKUP: self.lookup, T_RELAY_OPEN: self.relay_open, T_STATUS: self.status,
             T_TAKEOVER: self.takeover, T_LIST: self.list_lobbies}.get(
                ptype, self.unknown)(body, addr)
        except struct.error:
            self.stats["drop"] += 1

    def unknown(self, body, addr):
        self.stats["drop"] += 1

    def status(self, body, addr):
        (nonce,) = struct.unpack_from("<I", body)
        self.send(addr, T_STATUS_OK, struct.pack("<IHBB", nonce, min(len(self.lobbies), 65535),
                                                 0 if self.args.no_relay else 1, SERVER_PROTO) + self.observed(addr))

    def list_lobbies(self, body, addr):
        """The public lobby browser: u32 nonce, u16 game protocol -> LIST_OK u32 nonce, u16 total
        open, u8 n, n x (name[32], host name[24], u8 players, u8 max, u8 mode, u8 flags: 1 password,
        2 same game protocol). Up to LIST_MAX lobbies, same protocol first, then the fullest."""
        nonce, gproto = struct.unpack_from("<IH", body)
        rows = sorted(self.lobbies.values(),
                      key=lambda lb: (lb.game_proto != gproto, lb.players >= lb.max_players, -lb.players, lb.name))
        out = []
        for lb in rows[:LIST_MAX]:
            flags = (1 if lb.has_pw else 0) | (2 if lb.game_proto == gproto else 0)
            out.append(lb.name.encode()[:NAME_MAX - 1].ljust(NAME_MAX, b"\0")
                       + lb.host_name.encode()[:HOSTNAME_MAX - 1].ljust(HOSTNAME_MAX, b"\0")
                       + struct.pack("<BBBB", lb.players, lb.max_players, lb.mode, flags))
        self.send(addr, T_LIST_OK, struct.pack("<IHB", nonce, min(len(self.lobbies), 65535), len(out)) + b"".join(out))

    def register(self, body, addr):
        # u32 nonce, u16 gameProto, u8 max, u8 players, u8 mode, u8 hasPw, u64 pw, name[32],
        # hostName[24], u8 nLocal, nLocal x (u32 ip (network order), u16 port)
        nonce, gproto, maxp, players, mode, has_pw, pw = struct.unpack_from("<IHBBBBQ", body)
        o = 18
        name = clean_name(cstr(body[o:o + NAME_MAX]))
        o += NAME_MAX
        host_name = clean_name(cstr(body[o:o + HOSTNAME_MAX])) or "Host"
        o += HOSTNAME_MAX
        nlocal = body[o] if len(body) > o else 0
        o += 1
        local = []
        for _ in range(min(nlocal, 4)):
            ip_be, port = struct.unpack_from(">IH", body, o)
            o += 6
            local.append((socket.inet_ntoa(struct.pack(">I", ip_be)), port))
        if name is None:
            return self.reject(addr, R_BAD_NAME, T_REGISTER, nonce)
        key = name_key(name)
        old = self.by_key.get(key)
        if old is not None:
            if old.addr == addr:  # the same host registering again (lost reply): same lobby
                old.last = now()
                return self.send(addr, T_REGISTERED, struct.pack("<III", nonce, old.id, old.token) + self.observed(addr))
            if now() - old.last <= LOBBY_TTL:
                return self.reject(addr, R_NAME_TAKEN, T_REGISTER, nonce)
            self.drop_lobby(old, "expired")
        per_ip = sum(1 for lb in self.lobbies.values() if lb.addr[0] == addr[0])
        if per_ip >= self.args.max_per_ip or len(self.lobbies) >= self.args.max_lobbies:
            return self.reject(addr, R_LIMIT, T_REGISTER, nonce)
        lb = Lobby()
        lb.id, lb.token = self.new_id(self.lobbies), self.rng.getrandbits(32)
        lb.name, lb.key, lb.addr, lb.local = name, key, addr, local
        lb.game_proto, lb.max_players, lb.players, lb.mode = gproto, max(2, min(maxp, 16)), players, mode
        lb.has_pw, lb.pw, lb.host_name = bool(has_pw), pw, host_name
        lb.last = lb.created = now()
        self.lobbies[lb.id], self.by_key[key] = lb, lb
        self.send(addr, T_REGISTERED, struct.pack("<III", nonce, lb.id, lb.token) + self.observed(addr))
        self.log(f"lobby {lb.id:08x} '{name}' opened by {self.who(addr)} (game protocol {gproto}, "
                 f"{'password, ' if lb.has_pw else ''}{len(local)} LAN endpoints); {len(self.lobbies)} open")

    def owned(self, body, addr):
        lid, token = struct.unpack_from("<II", body)
        lb = self.lobbies.get(lid)
        if lb is None or lb.token != token or lb.addr != addr:
            return None
        return lb

    def takeover(self, body, addr):
        """A player of the lobby becomes its host (the host left or vanished): the lobby id and
        token the old host shared with its players move the name to the sender."""
        nonce, lid, token = struct.unpack_from("<III", body)
        lb = self.lobbies.get(lid)
        if lb is None or lb.token != token:
            return self.reject(addr, R_NOT_FOUND, T_TAKEOVER, nonce)
        o = 12
        nlocal = body[o] if len(body) > o else 0
        o += 1
        local = []
        for _ in range(min(nlocal, 4)):
            ip_be, port = struct.unpack_from(">IH", body, o)
            o += 6
            local.append((socket.inet_ntoa(struct.pack(">I", ip_be)), port))
        lb.addr, lb.local, lb.last = addr, local, now()
        for rid in [r.id for r in self.relays.values() if r.lobby == lid]:  # relays to the old host
            del self.relays[rid]
        self.send(addr, T_REGISTERED, struct.pack("<III", nonce, lb.id, lb.token) + self.observed(addr))
        self.log(f"lobby {lb.id:08x} '{lb.name}' taken over by {self.who(addr)} (new host)")

    def heartbeat(self, body, addr):
        lb = self.owned(body, addr)
        if lb is None:
            lid = struct.unpack_from("<I", body)[0]
            return self.reject(addr, R_NOT_FOUND, T_HEARTBEAT, lid)  # the host re-registers
        if len(body) >= 9:
            lb.players = body[8]
        lb.last = now()

    def unregister(self, body, addr):
        lb = self.owned(body, addr)
        if lb is not None:
            self.drop_lobby(lb, "host left")

    def lookup(self, body, addr):
        # u32 nonce, u16 gameProto, u64 pw, name[32]
        nonce, gproto, pw = struct.unpack_from("<IHQ", body)
        name = clean_name(cstr(body[14:14 + NAME_MAX]))
        lb = self.by_key.get(name_key(name)) if name else None
        if lb is None or now() - lb.last > LOBBY_TTL:
            return self.reject(addr, R_NOT_FOUND, T_LOOKUP, nonce)
        if gproto != lb.game_proto:
            return self.reject(addr, R_VERSION, T_LOOKUP, nonce)
        if lb.has_pw and pw != lb.pw:
            return self.reject(addr, R_BAD_PASSWORD, T_LOOKUP, nonce)
        if lb.players >= lb.max_players:
            return self.reject(addr, R_FULL, T_LOOKUP, nonce)
        hip = struct.unpack(">I", socket.inet_aton(lb.addr[0]))[0]
        body = struct.pack("<II", nonce, lb.id) + struct.pack(">IH", hip, lb.addr[1])
        body += bytes([len(lb.local)])
        for ip, port in lb.local:
            body += struct.pack(">IH", struct.unpack(">I", socket.inet_aton(ip))[0], port)
        body += bytes([lb.players, lb.max_players, 0 if self.args.no_relay else 1])
        self.send(addr, T_FOUND, body)
        # rendezvous: the host starts sending to the client's observed endpoint now
        cip = struct.unpack(">I", socket.inet_aton(addr[0]))[0]
        self.send(lb.addr, T_PUNCH, struct.pack("<I", lb.id) + struct.pack(">IH", cip, addr[1])
                  + struct.pack("<I", nonce))
        if self.args.verbose:
            self.log(f"lookup '{lb.name}' by {self.who(addr)}: found {lb.id:08x}")

    def relay_open(self, body, addr):
        nonce, lid = struct.unpack_from("<II", body)
        lb = self.lobbies.get(lid)
        if self.args.no_relay:
            return self.reject(addr, R_NO_RELAY, T_RELAY_OPEN, nonce)
        if lb is None:
            return self.reject(addr, R_NOT_FOUND, T_RELAY_OPEN, nonce)
        for r in self.relays.values():  # a resent RELAY_OPEN: same session
            if r.lobby == lid and r.client == addr:
                r.last = now()
                return self.send(addr, T_RELAY_OK, struct.pack("<II", nonce, r.id))
        if len(self.relays) >= self.args.max_relays or \
                sum(1 for r in self.relays.values() if r.client[0] == addr[0]) >= self.args.max_per_ip * 4:
            return self.reject(addr, R_LIMIT, T_RELAY_OPEN, nonce)
        r = Relay()
        r.id, r.lobby, r.client, r.host, r.last, r.bytes = self.new_id(self.relays), lid, addr, lb.addr, now(), 0
        rate = self.args.relay_kbps * 1024
        r.bucket_c, r.bucket_h = Bucket(rate, rate), Bucket(rate, rate)
        self.relays[r.id] = r
        self.send(addr, T_RELAY_OK, struct.pack("<II", nonce, r.id))
        self.log(f"relay {r.id:08x} opened for lobby {lid:08x} by {self.who(addr)}; {len(self.relays)} relays")

    def relay_data(self, body, addr):
        if len(body) < 4:
            self.stats["drop"] += 1
            return
        (rid,) = struct.unpack_from("<I", body)
        r = self.relays.get(rid)
        if r is None:
            self.stats["drop"] += 1
            return
        if addr == r.client:
            dst, bucket = r.host, r.bucket_c
        elif addr == r.host:
            dst, bucket = r.client, r.bucket_h
        else:
            self.stats["drop"] += 1
            return
        if not bucket.take(len(body)):
            self.stats["drop"] += 1
            return
        r.last = now()
        r.bytes += len(body)
        self.stats["relayed"] += 1
        self.send(dst, T_RELAY_DATA, body)

    # -- housekeeping --------------------------------------------------------------------
    def tick(self):
        t = now()
        for lb in [lb for lb in self.lobbies.values() if t - lb.last > LOBBY_TTL]:
            self.drop_lobby(lb, "expired")
        for rid in [r.id for r in self.relays.values() if t - r.last > RELAY_TTL]:
            r = self.relays.pop(rid)
            self.log(f"relay {rid:08x} closed (idle, {r.bytes} bytes)")
        if t - self.last_stats > self.args.stats_every:
            self.last_stats = t
            s = self.stats
            self.log(f"stats: {len(self.lobbies)} lobbies, {len(self.relays)} relays, rx {s['rx']} tx {s['tx']} "
                     f"relayed {s['relayed']} dropped {s['drop']}")

    def run(self):
        while True:
            r, _, _ = select.select([self.sock], [], [], 0.5)
            if r:
                for _ in range(512):
                    try:
                        data, addr = self.sock.recvfrom(2048)
                    except (BlockingIOError, InterruptedError):
                        break
                    except ConnectionResetError:  # Windows: ICMP port unreachable from a peer
                        continue
                    self.handle(data, addr)
            self.tick()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=int(os.environ.get("SMS_LOBBY_PORT", 7700)))
    ap.add_argument("--bind", default=os.environ.get("SMS_LOBBY_BIND", "0.0.0.0"))
    ap.add_argument("--no-relay", action="store_true", help="rendezvous only, never forward game traffic")
    ap.add_argument("--max-lobbies", type=int, default=500)
    ap.add_argument("--max-per-ip", type=int, default=3, help="lobbies per IP address")
    ap.add_argument("--max-relays", type=int, default=200)
    ap.add_argument("--relay-kbps", type=int, default=256, help="relay budget per session and direction, KiB/s")
    ap.add_argument("--rate", type=float, default=20.0, help="control packets per second per IP")
    ap.add_argument("--stats-every", type=float, default=300.0, help="seconds between stats lines")
    ap.add_argument("--log-addresses", action="store_true", help="log client IP:port (off: short hashed tags)")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()
    if hasattr(signal, "SIGTERM"):
        signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))
    try:
        Server(args).run()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
