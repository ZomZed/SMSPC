# Multiplayer (sm64coopdx-style) — design and interfaces

Owners: **w10** (network, sessions/lobbies, menus, file-select entry, free-mode start, host panel)
and **w15** (remote players in game: snapshot capture, puppet Marios, animation, FLUDD, name tags).
The shared API is `port/include/port_mp.h`; this file is the contract. Change either only
after noting it in the "Change log" at the end, and bump `PORT_MP_PROTOCOL_VERSION` when
a wire layout changes.

## Model
- Every client simulates its own world and its own Mario. Other players are puppets driven by snapshots.
- No world-object sync in this milestone (enemies, coins, NPCs are local to each client).
  Shared objectives (below) are the first exception, planned for stage 4.
- Players may be in different stages. A puppet is drawn only for players whose latest
  snapshot is in the same stage **and** scenario (`port_mp_same_stage`).
- Host-authoritative: the host owns the player table and session settings and relays
  every client's snapshots and reliable messages to the others. Clients only talk to the host.
- Up to 16 players (`PORT_MP_MAX_PLAYERS`); 4-8 is the tested range.

## Files
| File | Owner | What |
|---|---|---|
| `port/include/port_mp.h` | w10 (+w15 for snapshot fields) | shared C API |
| `port/src/port_mp_net.cpp` | w10 | Winsock UDP transport, handshake, reliable channel, LAN discovery, session table; own thread |
| `port/src/port_mp_menu.cpp` | w10 | ImGui menus: username, create/join (LAN list + direct IP), lobby/host panel, character stub |
| `port/src/port_mp_game.cpp` | w10 | game glue: file-select "run left" entry, free-mode start, in-memory save, stage presence, debug knobs |
| `port/src/port_mp_savecard.cpp` | w10 | in-memory memory card for the session's save (CARD API emulation) |
| w15 files (puppets) | w15 | capture/apply, `port_mp_set_local_snapshot`, puppet drawing |

Tiny hooks in other owners' files (all `SMS_PORT`, multiplayer-only):
- `port_window.cpp` (w9): `port_mp_menu_draw()` after `port_menu_draw`.
- `port_input.cpp` (w9): pads idle while `port_mp_menu_wants_input()`.
- `src/GC2D/CardLoad.cpp`: `TCardLoad::perform` calls the file-select entry check.
- `src/System/Application.cpp`: `port_mp_note_area()` on every app state; debug auto-start next to `SMS_PORT_WARP`.
- `src/System/CardManager.cpp`: its CARD calls are renamed to `port_mp_CARD*` (`port_mp_savecard.cpp`), which use an in-memory card while `port_mp_block_card_writes()` and the real API otherwise.
- `src/System/Application.cpp`: `port_mp_redirect_title()` before every app state change (a session never reaches the title).
- `src/MoveBG/Item.cpp` (coin `taken`), `src/MoveBG/ItemManager.cpp` (`loadAfter`), `src/System/MarDirectorDirect.cpp` (shine flag): shared objectives.

## Snapshot (unreliable, 30 Hz)
`PortMpSnapshot` (128 bytes, packed, little-endian; see the header for every field):
tick, stage, scenario, flags (valid, holding, on Yoshi, spraying, has FLUDD, hidden, in water, paused),
position, velocity, forward speed, face angles, `TMario::mStatus` (action) + arg, `mFlag`,
animation id/aux/frame/rate, FLUDD nozzle/second nozzle/water/spray strength/nozzle angles,
health, Yoshi state/colour, and 44 reserved bytes for w15.
- w15 fills it once per game logic frame and calls `port_mp_set_local_snapshot()`.
  The net thread sends the latest one at 30 Hz (only when its tick changed, plus a 1 Hz
  repeat so a paused player stays visible).
- Receivers keep the last 8 per slot (`port_mp_get_snapshots`, newest first, with the
  local receive time `rxMs`). Out-of-order or duplicate ticks are dropped. w15 interpolates
  (suggested: render ~100 ms behind the newest, extrapolate briefly on loss).
- A slot whose `serial` changes is a different player: despawn the old puppet.

## Puppets (w15)
(Written by w10 from `port_mp_puppet.cpp`; w15: add your suggestions for the next protocol version.)
- Palettes (w15 round 2, `port_mp_palette.cpp`): 11 presets over `PortMpCharacter`:
  `colors[0..6]` = cap, shirt, overalls, gloves, shoes, skin, hair (all 0 = GameCube colours),
  applied to the CMPR atlas of each puppet's own model data (and the local Mario's, at stage
  load). `colors` grew from 6 to 7 entries in protocol 3: hair is the same 3 bytes that
  `reserved[0..2]` were in protocol 2.
- Files: `port/src/port_mp_puppet.{h,cpp}` (capture, puppets, ghost harness),
  `port/src/port_mp_overlay.cpp` (name tags). Hooks: `port_mp_logic_frame()` in
  `TMarDirector::direct` (last simulation tick), the puppet view object in
  `TMarDirector::preEntry`, `port_mp_overlay_draw()` in `port_window.cpp` (exactly one call,
  before `port_menu_draw`). Calls into w10's API go through `/alternatename` fallbacks, so the
  puppet code links with or without `port_mp_net.cpp`.
- w15's full `PortMpPuppetState` is packed into the 128-byte `PortMpSnapshot` by
  `port_mp_pack_wire` / `port_mp_unpack_wire`. Standard fields as defined above (`tick` = logic
  frame counter, `flags` from the PMS_* state, `animFrame`/`animRate` = body slot, `sprayStrength`
  = pressure/255). The w15 block:
  - `pad0`: `warpSeq` (+1 on teleports: the puppet snaps instead of blending).
  - `animAux`: bits 0-1 hand model, 2-6 eye BTP (31 = none), 7-8 / 9-10 blend calc of slot 0 /
    1 (3 = off), 11 cap, 12 helmet, 13 sunglasses, 14 shirt, 15 4th hand (shine demo).
  - `nozzleAngle[2]`: FLUDD emit direction as yaw, pitch (s16 angle units).
  - `extra[44]`: `bck[4]` (body new/old, upper new/old), `u16 upperFrame` (x32), `u8 eyeFrame`,
    `u8 stageSeq` (+1 per stage load: snap), `u8 blend[2]` (x255), `s16 quat[3]` (base matrix
    rotation x/y/z with w >= 0, x32767), `s16 baseOfs[3]` (base matrix translation - pos, x4),
    `s8 head[4]`, `s8 waist[3]` (head/waist rotations >> 8), `s8 fluddBck, nozzleBck, yoshiBck`,
    `u16 fluddFrame, nozzleFrame, yoshiFrame` (x32), then (block v1.1, protocol 2) `s8 emitOfs[3]`
    (emitter - pos, /2: extra[38..40]), `u8 heldLo[2]` (held object's actor type, low 16 bits:
    extra[41..42]), `u8 heldCat` (its category bit + 1; 0 = nothing held, 0xFF = no bit: extra[43]).
- Receivers interpolate 100 ms behind the newest snapshot (receive time on `port_mp_time_ms`),
  snap on `warpSeq`/`stageSeq` changes or gaps over 500 ms, hide puppets outside our
  stage+scenario, when `HIDDEN`, or after 3 s without data, and respawn a puppet when the slot's
  `serial` changes. Puppets never touch game state (no collision, hit actor, water, sound).
- `SMS_PORT_MP_TESTSNAP` (w10's transport test) yields to this capture automatically; leave it
  off in normal use.
- **Pose fidelity harness** (`SMS_PORT_MP_POSE_DUMP=<file>`, `port_mp_puppet.cpp` "3b"): writes
  the joint matrices of the local Mario after every logic frame's capture (kind 0, keyed by the
  snapshot's seq; body, FLUDD and nozzle models), of every puppet after its calc (kind 1, with
  the two snapshots it was interpolated between and the fraction), and the ghost playback's map
  from local push frame to recorded frame (kind 2). Record a ghost with a movement script
  (`SMS_PORT_WARP=4,0 SMS_PORT_MP_GHOST=g.bin SMS_PORT_MP_POSE_DUMP=rec.pose
  SMS_PORT_MOVE_SCRIPT=... SMS_PORT_MOVE_EXIT=1`), play it back in the same stage
  (`SMS_PORT_MP_GHOST_PLAY=g.bin SMS_PORT_MP_POSE_DUMP=play.pose SMS_PORT_MOVE_SCRIPT=0-10:
  SMS_PORT_MOVE_FRAMES=<n> SMS_PORT_MOVE_EXIT=1`), then
  `python port/tools/mp_pose_compare.py rec.pose play.pose [--joints] [--csv ...]` pairs the
  puppet's frames with the recorded Mario's (same logic frame: the ghost feeds one recorded frame
  per logic frame and the 100 ms delay is exactly 3 frames, so the fraction is 0) and lists, per
  joint, translation (world units) and rotation (degrees) differences, the runs of frames over
  the thresholds with both sides' animation state, and the worst frames. The log also prints
  Mario's joint table once (`[mp] pose dump: joint <i> parent <p> <name>`). A frame-number or
  BCK difference between the two state columns is a capture/wire issue, identical inputs with a
  different matrix is a puppet calc issue.
- **Pose fidelity findings (Gelato 4/0 scripts: walk/run/skid, single/double/triple jump, side
  flip, spin jump, ground pound, dive, standing and walking spray, swimming and strokes, hover /
  rocket / turbo nozzles).** The capture, wire packing and interpolation were exact: in every
  paired frame the puppet had the same status, animation ids, frames, blend ratio, hands and eye
  pattern as the recorded Mario, and legs, hips and chest matched to the float. Everything
  that differed was in the puppet's model calc, i.e. game-side joint work the puppet did not
  reproduce:
  - **Feet on the ground** (`MarioFootPos*/Dir*Ctrl`, MarioDraw.cpp): while Mario stands, each
    foot is dropped onto the ground under it (up to 15 units) and turned to the ground normal.
    Puppets got the same callbacks on `chn_foot_*` / `jnt_foot_*`, gated by the snapshot's status
    (waiting type, not brake end, not on Yoshi, not sleeping), using the local collision.
  - **Joint springs** (`TMultiMtxEffect` / `TMtxTimeLag`, MtxUtil.cpp): `TMario::initModel`
    puts a time-lag spring on `jnt_chest`, `jnt_arm_R1` and `jnt_arm_L1` (the arms and head trail
    behind in jumps, dives and the hip drop: up to 40 units / 140 degrees of difference at the
    hands), `TMarioCap` one on the cap's bone 1, `TWaterGun` a time lag on FLUDD's root and a
    `TMtxSwingRZ` on `jnt_G_handle`. Puppets build the same effects on their own models.
    Two things had to follow: the `.prm` parameters live in the "params" archive that only exists
    while the stage's objects are set up, so a puppet built during play copies the values from the
    local Mario's effects; and Mario's model is calculated on every simulation tick (`TMario::perform`
    calls `calcAnim` in its CUE_MOVE pass; TMarDirector runs 4 ticks per logic frame, measured
    constant with the pose dump, which records the count), so the springs and his frame controls
    step four times a frame: the puppet's body, cap and FLUDD are calculated in 4 passes (body frame
    k rate steps back, base matrix interpolated from last frame's, then the snapshot's pose; the
    feet are aligned on the last pass only, as `addCallBack` installs the foot callbacks on the
    last tick only). The on/off rule (`TMario::addCallBack`: off in a stage's first 0x4B0
    movement ticks and while pumping, on for animations flagged in `gMarioAnimeData.unk6` with the
    upper body idle) needs the sender's tick count, so the snapshot carries `PORT_MP_SNAP_FX_OFF`
    (flags bit 0x0100, `PMS_FX_OFF`); the springs restart on a snap.
  - **Nozzle joints** (`TWaterGun::init`, WaterGun.cpp): the spray nozzle's `chn_muzzle_1` and
    the turbo nozzle's `jnt_back_nozzle_neck` turn with the gun angle while Mario aims down
    (`NozzleCtrl`; the puppet takes it from `headRot[3]`, which carries half the gun angle), and
    the turbo nozzle's `chn_back_nozzle_prop` spins (`RotateCtrl`, `TWaterGun::unk1CD0`): the
    puppet runs its own propeller angle with the water gun's parameters, driven by the snapshot's
    pressure, once per simulation tick. The hover nozzle's side-nozzle tilt (`WaterGunDivingCtrl*`,
    stick input while hovering) is not mirrored (the hover test showed no measurable difference).
  - **Measured** (worst joint per frame, body; frames over 4 units / 3 degrees): general
    movement script, before any fix 5.8 units / 13.9 degrees mean, 67% of frames over (hands up
    to 42 units / 144 degrees off in jumps) -> 0.48 / 0.66, 4.4%; swimming 3.5 / 6.0, 50% ->
    0.27 / 0.43, 2.8%; hover 6.0 / 4.4, 57% -> 0.16 / 0.48, 3.3%; rocket 7.8 / 4.0, 77% ->
    0.23 / 0.48, 3.2%; turbo 6.9 / 10.2, 63% -> 3.4 / 2.6, 20% (the rest is the foot drop below
    and the propeller's phase). FLUDD + nozzle frames over threshold: 591 -> 23 (general),
    359 -> 5 (hover), 450 -> 8 (rocket); turbo stays high because the propeller's phase differs.
  - Left as is: head and waist rotations go over the wire in 1.4-degree steps (`s8` of the s16
    angle); the ghost's residual is below 1 degree on the head chain. The feet's ground alignment
    can still differ where the two clients' `checkGround` from the foot position answers
    differently (seen once, standing on a Gelato structure after the turbo run: a 15-unit foot drop).
## Transport (port_mp_net.cpp)
UDP over Winsock, one socket per process. Host binds `0.0.0.0:<port>` (default 7777,
`[multiplayer] port`), clients bind an ephemeral port. No library downloads.

Every datagram starts with a 16-byte header:

| Off | Size | Field |
|---|---|---|
| 0 | 4 | magic `'SMP1'` |
| 4 | 2 | protocol version (`PORT_MP_PROTOCOL_VERSION`) |
| 6 | 1 | packet type |
| 7 | 1 | sender slot (0xFF before accept) |
| 8 | 4 | connection id (random, assigned by the host at accept; 0 before) |
| 12 | 2 | packet sequence (per connection, for loss stats/ordering of unreliable data) |
| 14 | 2 | payload length |

Anything that is too short, has the wrong magic/version/length, an unknown type, or a
connection id that does not match the sender address is dropped silently (counted in stats).

Packet types:
- `DISCOVER` / `LOBBY_INFO`: LAN lobby query (broadcast) and the host's answer (lobby name,
  host name, players, max, game mode, protocol). The same `LOBBY_INFO` payload is what a
  future lobby server would list.
- `CONNECT` (client: `JOIN` tag, nonce, name, character; protocol in the header) / `ACCEPT` (host: slot, connection id,
  session settings, full player table) / `REJECT` (reason: version, full, bad name, closed).
  The client resends `CONNECT` every 250 ms for 5 s, then fails.
- `RELIABLE`: one or more reliable messages `[u16 id][u8 kind][u16 len][payload]`, plus a
  cumulative ack (`u16` last in-order id) and a 32-bit selective ack bitfield.
  Unacked messages are resent after max(100 ms, 1.5 × RTT); delivery is in order per connection.
- `ACK`: acks only (sent when there is nothing else to piggy-back on within 50 ms).
- `SNAPSHOT`: `[u8 slot][PortMpSnapshot]`; the host relays each client's snapshot to the others.
- `APP`: unreliable app message `[u8 channel][u8 to][u8 from][data]` (`port_mp_send`); the host
  delivers it to itself and forwards it (`to` = 0xFF: everyone but the sender).
- `ROSTER` (host, 1 Hz): every player's ping and stage.
- `PING` / `PONG`: every 1 s, RTT and keepalive. `DISCONNECT`: polite leave (sent 3 times).
- **Host migration (protocol 4):** the host shares every player's endpoint (`PEERS`) and, in
  server mode, the lobby's id + token (`LOBBYKEY`) with its guests. When the host leaves (clean
  `DISCONNECT`) or is silent for 20 s, every guest picks the lowest remaining slot as the new host:
  that player becomes host on its own socket (`TAKEOVER` moves the lobby name on the server; if the
  old entry is gone it registers the name anew), the others re-join it (server mode: by name;
  direct IP: at the endpoint the old host shared) with `CONNECT` carrying their old slot + serial,
  so slots and names stay. Players not back within 20 s are dropped. Every game keeps running
  through it (`port_mp_active()` stays 1 while re-joining). It fails cleanly (session ends with a
  message) when nobody else is left or the new host cannot be reached (direct IP across NATs).
  The new host starts with empty shared-objective / world-event lists.
- Short drops: after 3 s without packets the peer is shown as reconnecting (toast, lobby panel);
  the session survives up to 20 s of silence (reliable messages are resent when it comes back,
  and the host accepts the client's packets from a new address after a 2 s gap: NAT rebinding,
  Wi-Fi change; the random connection id identifies it). Checked with `SMS_PORT_MP_BLACKOUT`:
  an 8 s drop reconnects, a 26 s drop ends the guest's session.
- Timeout: no packet from a peer for 20 s = disconnected (the host frees the slot and tells
  everyone; a client returns to "connection lost" and leaves the session).

Reliable message kinds: `PLAYER_JOIN` (slot, serial, name, character), `PLAYER_LEAVE` (slot,
reason), `PLAYER_INFO` (name/character change), `SETTINGS` (PortMpSessionSettings),
`STAGE` (slot, stage, scenario), `OBJECTIVE` (op COLLECT / COLLECTED / SET + ids), `APP` (reliable
app message, same body as the `APP` packet). Chat, kick and the rest go through app channels.

## Lobbies and discovery
- LAN: "Refresh" broadcasts `DISCOVER` to 255.255.255.255 and to 127.0.0.1 on ports
  7777-7792 (so several hosts on one machine are found); hosts answer with `LOBBY_INFO`.
- Direct join: host name or IP and port.
- **Internet play:** answered by the lobby server below (rendezvous + relay, `port/server`);
  it runs only where the user deploys it. Earlier note kept for history: The protocol is ready for one:
  a lobby server would accept `LOBBY_INFO` registrations from hosts (plus a heartbeat) and answer
  `DISCOVER` with the list; hole punching would add a server-introduced `CONNECT` to both ends;
  a relay would forward datagrams by connection id. Who hosts it, and its privacy/abuse
  policy, is for the user to decide.

## Connection types and the lobby server (protocol 3)
Create and Join pages offer **Server (lobby name)** (default) or **Direct IP** (as before).
- **Server mode, host:** enters a lobby name (+ optional password). `port_mp_host2(...,
  PORT_MP_CONN_SERVER, password)` binds a game port (the next free one in 7777-7792; it is
  never shown) and registers the name on the lobby server from the game socket, so the server
  sees the host's public (NAT) endpoint; heartbeats every 2 s keep the lobby and the NAT
  mapping alive. "Name taken" (case-insensitive, unique while the lobby lives) fails the
  session with a message. If the server is unreachable the lobby still works on the LAN
  ("can be joined on this network only"), and registration keeps retrying.
- **Server mode, client:** types the name (+ password). `port_mp_join_name` asks the server
  (`LOOKUP`) and the LAN (`DISCOVER`, matched by name) at the same time. The server answers
  with the host's public endpoint and its LAN endpoints and tells the host the client's
  endpoint (`PUNCH`): both start sending, which opens the NATs (UDP hole punching). The
  client sends `CONNECT` to every candidate; the first `ACCEPT` wins (the host follows the
  path the client settles on). After 2.5 s without an answer (or at once with
  `SMS_PORT_MP_FORCE_RELAY=1`) it asks the server for a relay session and connects through it.
  No IPs or ports are shown in server mode.
- **Passwords:** an FNV-1a 64 hash of name + password (casual protection, not a secret) goes
  in `REGISTER`, `LOOKUP` and `CONNECT`; the server and the host both check it.
- **Relay in the client:** relayed peers get virtual addresses `240.0.0.n:1` (one per relay
  session), so the rest of the transport is unchanged; datagrams to them are wrapped in
  `RELAY_DATA` to the server.
- **Server status** in the menus: online (lobby count) / offline / not set up; the server
  address is `server = host[:port]` in `port_multiplayer.ini` (placeholder
  `lobby.example.invalid:7700` until the user deploys one; `SMS_PORT_MP_SERVER` overrides)
  or "Lobby server address" in the multiplayer menu.
- **Join info** in the lobby panel: server mode shows the lobby name with a Copy button;
  Direct IP shows this PC's LAN / virtual-LAN (Tailscale, ZeroTier, Radmin, Hamachi)
  addresses with the port and Copy buttons, and the Internet address only when our own lobby
  server reports a public one (never a third-party lookup), else "needs port forwarding of
  UDP <port>".

### Lobby server protocol (`port/server/sms_lobby_server.py`, UDP 7700)
Datagram: `u32 'SMLS' (0x534C4D53)`, `u8 version (1)`, `u8 type`, body (little-endian
except IPv4 address/port, which are network order).

| Type | Direction | Body |
|---|---|---|
| 1 REGISTER | host -> S | u32 nonce, u16 game protocol, u8 max players, u8 players, u8 mode, u8 has password, u64 password hash, name[32], host name[24], u8 n, n x (ip, port) LAN endpoints |
| 2 REGISTERED | S -> host | u32 nonce, u32 lobby id, u32 token, observed ip, port |
| 3 REJECT | S -> any | u8 reason (1 version, 2 name taken, 3 bad name, 4 limit, 5 not found, 6 bad password, 7 full, 8 rate, 9 no relay), u8 request type, u32 nonce / lobby id |
| 4 HEARTBEAT | host -> S | u32 lobby id, u32 token, u8 players (unknown id -> REJECT not found -> re-register) |
| 5 UNREGISTER | host -> S | u32 lobby id, u32 token |
| 6 LOOKUP | client -> S | u32 nonce, u16 game protocol, u64 password hash, name[32] |
| 7 FOUND | S -> client | u32 nonce, u32 lobby id, host ip, port, u8 n, n x LAN (ip, port), u8 players, u8 max, u8 relay available |
| 8 PUNCH | S -> host | u32 lobby id, client ip, port, u32 nonce |
| 9 RELAY_OPEN | client -> S | u32 nonce, u32 lobby id |
| 10 RELAY_OK | S -> client | u32 nonce, u32 relay id |
| 11 RELAY_DATA | both | u32 relay id, one game datagram |
| 12 STATUS / 13 STATUS_OK | client <-> S | u32 nonce / u32 nonce, u16 lobbies, u8 relay, u8 version, observed ip, port |
| 14 TAKEOVER | new host -> S | u32 nonce, u32 lobby id, u32 token, u8 n, n x LAN (ip, port); answered with REGISTERED, or REJECT not found |
| 15 LIST | client -> S | u32 nonce, u16 game protocol (the Join page's lobby browser, sent with each status ping while not in a session) |
| 16 LIST_OK | S -> client | u32 nonce, u16 lobbies open, u8 n, n x (name[32], host name[24], u8 players, u8 max, u8 mode, u8 flags: 1 password, 2 same game protocol); up to 20, same protocol and not full first. Older servers ignore LIST (the list stays empty) |

Server rules: names 1-31 bytes, compared case-folded with spaces collapsed; a lobby expires
30 s after its last heartbeat (longer than the game's 20 s timeout, so a new host can take it over); relay sessions after 30 s idle; limits (defaults): 1450-byte
datagrams, 20 control packets/s per IP, 3 lobbies per IP, 500 lobbies, 200 relays, 256 KiB/s
per relay direction. Logs carry lobby ids/names and hashed address tags only. Deployment on a
free VM: `port/server/DEPLOY.md` (Oracle Cloud Always Free, Google Cloud e2-micro), with
`install.sh` and the systemd unit `sms-lobby.service`. Nothing is public until the user runs it.
## Entry, menus
- **File select:** when Mario, with control on the save-block screen (area 15, `TCardLoad`
  state 0, pad not in menu mode), runs into the left boundary (x ≤ 870 with the stick held
  left for 15 game frames), the multiplayer menu opens instead. A small "◀ Multiplayer" hint
  is drawn at the left edge there. Otherwise the file select is unchanged. Off when
  `[multiplayer] enabled = 0`.
- **Multiplayer menu (ImGui, controller-navigable):** username (saved in
  `port_multiplayer.ini` next to `port_settings.ini`), Create lobby (name, max players,
  game mode = Free, shared objectives, port), Join (LAN list with Refresh + direct IP:port),
  Character (stub), Back. While open, the game gets idle pads.
- **Host / lobby panel** (in game: F2 or the pause menu's Back button while in a session):
  player list with ping and stage, Game mode list (`Free` only, an extensible list),
  Shared objectives Off/On (host), Leave. Clients see the same panel read-only.

## Free mode
- Creating a lobby starts the session immediately and loads Delfino Plaza; a client that
  joins loads the plaza after `ACCEPT` (sm64coopdx style: no separate "Start" step).
- Save: `TFlagManager::firstStart()`, then the story flags of a finished game (w13's
  `story2` preset: every shine, every nozzle box, all plaza events and movies watched,
  Corona visited), then `saveSuccess()` so a game over restores the same state.
  Plaza scenario 2 (post-game): every entrance open, no goop on the Pianta statue, no
  Shadow Mario event, no unlock demos. The scenario comes from the host's settings, so
  all players load the same layout.
- The save lives only in memory: while a session's save is active, TCardManager's CARD calls
  go to an in-memory card (`port_mp_savecard.cpp`, renamed in `CardManager.cpp`): an empty
  59-block card holding at most the game's one file, so every save prompt (Shine get, pause
  menu, game over, the ending) completes normally (`File created.`, `Saved.`). The real card is
  unmounted before and after, and is never read or written by multiplayer.
- A session never lands on the title screen (`port_mp_redirect_title`, hook in
  `TApplication::proc`): the ending plays (movies, staff roll, save prompt) and then, instead of
  the title, the player is sent back to the session's Delfino Plaza (still in the lobby, toast).
  A game over restores the multiplayer save and returns to the plaza by itself (checked). The
  reset combo leaves the lobby cleanly (toast) and goes to the title as usual. Safety net: a
  player who still reaches the title / file select in a session leaves the lobby with a message.
- Leaving (lobby panel, host closed the lobby, connection lost) returns to the file-select
  stage (`setNextStage(15)`, which starts at its title screen); normal file loading then
  replaces the flags again, and card writes are allowed again from there.
- Corona Mountain: plaza scenario 2 keeps the Corona entrance (a `MapObjChangeStage` at
  (-2160, 2850, -12750)); touching it loads stage 52 (checked, BOOTLOG MP9).

## Shared objectives (host setting; implemented in stage 4)
`PortMpSessionSettings.sharedObjectives`: Create-lobby page and the host's lobby panel,
synced to all (`SETTINGS`), saved as `shared_objectives` in `port_multiplayer.ini`.
- **Off (default):** every Mario tracks his own shines, blue/red/yellow coins. This is what
  separate simulations give: each client has its own in-memory save and its own HUD counts.
- **On:** collecting a Shine, blue coin, red coin or yellow coin on one client removes it
  for everyone in that stage and credits it to everyone's counters.
  - IDs (`PORT_MP_OBJ_ID` in `port_mp.h`, 32 bits: kind | stage | scenario | index):
    yellow/red coins = index in `gpItemManager`'s object list, **scene-placed only**
    (`TItemManager::loadAfter` reports where the scene's items end, before it creates its
    20-coin pool; scene objects are created in file order on every client, so the index is
    the same everywhere). Coins from the pool, from enemies or created at run time are
    local only. Blue coins = (area, blue-coin event id), the game's own flag key. Shines =
    shine id (`TShine::getEventId`, the flag `setShineFlag` uses).
  - Collect: hooks at the top of `TCoin/TCoinRed/TCoinBlue::taken` (`Item.cpp`) and where
    the Shine-get demo sets the shine flag (`MarDirectorDirect.cpp`) send `OBJECTIVE
    COLLECT(id)`. The host is authoritative: the first claim for an id wins and is broadcast
    (`COLLECTED(id, slot)`); later claims are ignored. If two players touch the same coin in
    the same frame, both got it locally (each +1); everybody else gets it once.
  - Apply on the others (`port_mp_game.cpp`, game thread, once the stage is loaded): coins
    get the count (`incGoldCoinFlag` / `MSF_RED_COIN_COUNT`), their container's message (red
    coin managers count them, so the red-coin Shine appears), then `kill()` (respawning coins
    respawn as in `TItem::taken`). No Mario message, no sound, no reward for the local Mario.
    A red coin that is not out yet is credited and removed when it appears. Reaching 100
    coins this way makes the 100-coin Shine appear, as in `TCoin::taken`. Blue coins:
    `setBlueCoinFlag` (+ remove the coin if it is in this stage), no save prompt. Shines:
    `setShineFlag` (count) + remove the Shine if present, no demo, a toast "<name> got a
    Shine Sprite!". The collector plays the demo and leaves the stage as usual.
  - Late joiners get the session's shines/blue coins at `ACCEPT`; every stage entry gets
    that stage+scenario's collected coins. The host forgets a stage's coins when nobody is
    in it any more (a revisit respawns them, like on GameCube; blue coins and shines stay).
  - The red-coin timer runs on the client that started it (its switch is local).
  - Toggling mid-session: the new value applies at each client's next stage load
    (latched when the stage's items are loaded).
## Stage presence (P5, w11: `port/src/port_mp_presence.cpp`)
Plugs in only through `port_mp.h` (game frame, overlay and lobby-section hooks, `PORT_MP_CH_WARP`,
`reserved[3]`); no edits to w10's or w15's files, nothing runs outside a session.
- **Lobby panel section "Where is everyone / level select":** every player with where they
  are (stage + episode, "Delfino Plaza", "Secret course coro_ex2", or File select / Watching a
  movie / In the menus / Loading, plus "(paused)", "(talking)", "(cutscene)"), a "Warp to"
  button per player, "Return to Delfino Plaza", "Arrows to off-screen players in your stage",
  the host's "Level select for everyone" and the level select (plaza, every main stage with
  its episodes, the Pinna Park interior, casino and Noki undersea, bosses, the 33 secret
  courses). The panel opens with F2, or Z in the pause menu, so return-to-plaza works from any stage.
- **Stage changes** use the game's path: `TApplication::mNextArea` + the director's
  `DIRECTOR_FLAG_STAGE_TRANSITION_PENDING` (what `TMarDirector::setNextStage` sets, minus its
  plaza gate-camera demos), so `moveStage` fades and loads as for a pipe. A request waits
  while Mario talks or a cutscene runs (toast), starts from the pause menu when it closes,
  and is ignored in the file select.
- **Warp to player:** load that player's stage + scenario (no reload if it is ours), wait
  until Mario is visible, on the ground (or swimming) and the director is in normal play,
  then send `SPAWN_REQ` (reliable) to that player, who answers `SPAWN_POS` with where its
  Mario last stood on solid ground in that stage (not jumping, swimming, held or on Yoshi).
  The answer is applied with the game's `TMario::warpRequest` (moves the camera too). No
  answer within 5 s, a "no position" answer, or 90 s overall: Mario keeps the stage's start.
- **Off-screen arrows:** for players in our stage + scenario (`port_mp_same_stage`, not
  `HIDDEN`, snapshot < 3 s old), projected with the game camera (position, target, fovy,
  widened aspect); on-screen players are left to w15's name tags. Arrow + "name 12m" at the
  picture's edge, towards the player (behind the camera: towards that side).
- **Host setting:** `reserved[3]` = 1: level select for the host only (warp-to-player and
  return-to-plaza stay available to everyone).
- **Wire (`PORT_MP_CH_WARP`, packed, little-endian):** `PRESENCE {u8 type=1, appState,
  stage, scenario, activity (0 play, 1 paused, 2 talk, 3 cutscene, 4 loading), pad[3]}` to
  everyone, reliable on change (at most every 250 ms) and whenever a new player joins,
  unreliable every 3 s; `SPAWN_REQ {u8 type=2, stage, scenario, pad, u32 nonce}` and
  `SPAWN_POS {u8 type=3, stage, scenario, valid, u32 nonce, f32 pos[3], f32 yawDeg}`, one slot.
  Peers without this file just show the roster's stage.
- Debug: `SMS_PORT_MP_PRESENCE_TEST="<name>:<action>@<frame>[=<arg>];..."` (actions `goto=<stage>,<scenario>`,
  `warp=<player name>`, `plaza`, `lsoff`, `lson`, `panel`; names as `mp_test.py` gives them:
  Host, Guest1, ...), `SMS_PORT_MP_PRESENCE_LOG=1` (markers every 5 s). Example run:
  `SMS_PORT_MP_PRESENCE_TEST='Host:goto@1800=2,0;Guest1:warp@2700=Host' python port/tools/mp_test.py -n 3 --walk`.
- Suggestion for w10 (not done, your file): the lobby panel's own "Where" column could go
  now (this section shows the same and more), and a `port_mp_menu_close()` would let a warp
  button close the panel.

## Player interaction (P2, w8)
Files: `port/src/port_mp_interact.cpp` (game thread, registered with
`port_mp_register_game_frame`) and `port/src/port_mp_interact_ui.cpp` (lobby section
"Player interaction"). No game-source hook: the reactions call TMario's own functions from
the game-frame hook, between logic frames.
- **Authority:** every client moves only its own Mario. The attacker detects a hit against
  the other player as drawn: its snapshots interpolated 100 ms behind the newest receive time,
  as w15's puppets do. The attacker reacts at once, locally, and sends a hit message to the
  victim; the victim applies the knockback to its own Mario.
- **Bump (Full and Solid):** no message. Each client pushes its own Mario out of the other
  players' bodies (radius 2 x `TMario::unk15C`, overlapping heights) with `TMario::keepDistance`.
  In Full mode that includes the game's clash: running into someone faster than
  `mDeParams.mClashSpeed` knocks you back (`JUMP_SHORT_BACK_DOWN`). Solid mode does the same
  push (wall and floor checked) without the clash.
- **Stomp (Full):** falling (`MARIO_STATUS_FLAG_JUMPING`, `vel.y < 0`, not diving) onto the
  other player's head: feet between half and full body height above its position. The
  attacker bounces with `TMario::trampleExec` on a proxy `THitActor` (as off an enemy: trample
  jump, kick sound, particles, rumble), then sends `stomp`.
- **Ground pound (Full):** the same contact while in `MARIO_STATUS_HIP_DROP`. The pound
  continues, and `ground pound` is sent.
- **FLUDD (Full, `reserved[4]` = 0):** one of our own water particles with its hit flag set
  (`TModelWaterManager`, the same test as `TObjHitCheck::checkWater`) inside the other
  player's body sends `FLUDD`. Only our own water is in the water manager; the puppets'
  spray is a drawn stand-in.
- Per victim, an attacker waits 20 logic frames after a stomp or pound and 30 after FLUDD
  before it can hit that player again.
- **Victim:** `TMario::damageExec` with damage 0 from a proxy at the attacker's position
  (knocked away from the attacker; when the attacker is straight overhead, away from its facing):
  - stomp: anim type 1 (short knock-down), speed 16, 45 invincibility frames;
  - ground pound: type 2 (full knock-down), speed 24, 60 frames;
  - FLUDD: type 0 (light stagger), speed 10, 30 frames.

  This gives the game's voice, particles, camera shake and invincibility, and no health loss.
  On Yoshi, the game's own rule applies (you are knocked off Yoshi).
- A hit is ignored with a log line when:
  - the mode is not Full, or it is a FLUDD hit with `reserved[4]` = 1;
  - our Mario is not in control (director not in its normal state: pause, talk, demo, save prompt; game over);
  - the attacker is not in our stage;
  - the message is older than 15 logic frames by the attacker's snapshot ticks;
  - the attacker is too far away: over 600 units (2000 for FLUDD).
- **Hit message** (`PORT_MP_CH_INTERACT`, reliable, sent to the victim's slot), 24 bytes:
  `u8 version (1), u8 kind (1 stomp, 2 ground pound, 3 FLUDD), u8 victim slot, u8 pad,
  u32 attacker snapshot tick, f32 attacker position[3], s16 attacker facing, s16 pad`.
- **Host settings:** `reserved[0]`: 0 = Full (default), 1 = Solid only, 2 = Off. `reserved[4]`:
  FLUDD pushes players, 0 = yes (default), 1 = no. The host changes them in the lobby panel;
  clients see them read-only.
- Debug knobs:
  - `SMS_PORT_MP_INTERACT=full|solid|off` and `SMS_PORT_MP_FLUDD_PUSH=0|1` (host, applied once).
  - `SMS_PORT_MP_INTERACT_TEST=<frame>:stomp|pound|spray|bump[,...]`: at that frame, puts the
    local Mario next to the first other player in the stage: 300 above, 400 above, 250 beside
    and facing it, or overlapping it. The pound needs PADSCRIPT `L` (the ground pound is L in
    the air; Z opens the map) and the spray `R`.
  - Every hit sent, applied or ignored is logged as `[mp-interact]`.

## Enemy and object sync (P8, w11): design
Goal: players in the same stage + scenario see the same enemies (positions, states, HP,
deaths) and the same moving map objects, without host-simulating stages the host is not in
and without changing a single-player frame. Prototype: `port/src/port_mp_objects.cpp` (below).

**Authority: per-object ownership by the nearest player, arbitrated by the stage leader.**
- Host-authoritative simulation per stage does not fit: every client already simulates its
  own copy of its stage, the host may be in another stage (or the plaza), and a host-run enemy
  would react to remote Marios only through snapshots (100+ ms late, so stomps and hits feel
  wrong for everyone but the host).
- Instead every synced object has exactly one **owner**, who runs it with the game's own code
  (AI, nerves, collision, its reactions to that owner's Mario) and sends its state; all other
  clients show a **puppet**. The owner is the player nearest to the object (sm64coopdx's sync
  objects do the same), so the player interacting with an enemy runs it with no lag.
- The **stage leader** (lowest slot among the players whose latest snapshot is in that stage +
  scenario, so no extra messages to elect it) decides ownership every 0.5 s: nearest player by
  snapshot position, with hysteresis (the new player must be nearer than 0.7 x the current
  owner's distance - 100 units, at most one change per object per second). The table is sent
  as exceptions to "owned by the leader" (reliable on change, full every 2 s). A player who
  leaves the stage gives its objects back to the leader; when the leader leaves, the next
  lowest slot takes over with the table it already has.
- Hand-off: the new owner continues from the puppet pose it shows (interpolated latest state)
  with its local AI state. For nerve-driven enemies the next version should send the nerve id
  (index into a per-class nerve table) and HP with the state, so the new owner resumes in the
  right nerve.

**What to sync, in order.**
1. Enemies managed by `TLiveManager`/`TEnemyManager` (all `TSpineEnemy`): position, rotation,
   alive/dead (+ the death nerve), HP, current nerve, animation id/frame for the non-walk
   cases. Prototype: the Strollin' Stu family (`THamuKuri` and variants, actor types
   `HAMU_KURI`, `HANE_HAMU_KURI`, `DANGO_HAMU_KURI`, `FIRE_HAMU_KURI`, `DORO_HAMU_KURI`,
   `DORO_HANE_KURI`). Then the other `TSmallEnemy` walkers (same base, same messages), then
   flying/water enemies, then bosses (phase 2: owned by the nearest player = the one fighting;
   the others keep their own boss AI running but follow the owner's nerve, HP and pose, and
   their damage goes to the owner; see "Enemy sync, phase 2").
2. Map objects that move: lifts, rail blocks, rotating/swinging platforms (`TRailMapObj`,
   `TNormalLift`, `TRailBlock`, `TRollBlock`, `TSwingBoard`, ...). Prototype: every map object
   with map collision outside the item manager, sent only while it moves (static ones cost
   nothing). Riders follow automatically: `calcRideMomentum` reads the platform's matrix.
3. State switches and Shine gates: switches, cages, breakable blocks, sand birds, Shine-gating
   objects. These are events, not streams: a reliable "object X did Y" message from whoever
   triggered it (the P1 pattern: the first claim wins at the host, then everyone applies the
   game's own change). Persistent ones go into the late-join table like P1's shines.
4. Goop / pollution cleaning: a separate stream. The pollution layers are textures
   (`TPollutionLayer`, cleaned by water hits); send cleaned cells as run-length deltas of the
   layer bitmaps (reliable, coalesced every 0.5 s), plus a full snapshot for late joiners.
   Not in the prototype.
5. Not synced: NPCs (ambient), particles, sounds, items from the pools (coins dropped by
   enemies stay local; P1 covers the scene's coins), the camera.

**Stable ids.** `(manager index in TConductor's manager list) << 16 | (object index in that
manager)`. Scene objects are created in scene-file order on every client (P1 relies on the
same for coins), so ids agree without names. Messages carry stage + scenario and are dropped
outside it. A stage without a conductor keeps a stale `gpConductor` (the game never clears it):
the prototype checks the scene's name tree for the conductor before reading it. Objects created
at run time (generator pools beyond the scene's) are not addressable by this id; they need
an id from the owner (pool index + generator id), later.

**Spawn / despawn / late join.** The owner's alive flag is in every state entry (dead objects
included in the 1 s keyframes). Owner reports dead: the puppet plays its own death locally
(`kill()`: for `TSmallEnemy` the die nerve), then stays dead. Owner reports alive while ours is
dead (the game respawned it): `reset()` + the owner's position. A late joiner builds its list
on its second frame in the stage and converges within one keyframe (1 s); until it gets the
first state for an object, that object runs locally.

**Bandwidth and interpolation.** State entries are 24 bytes (id, alive flag, position f32 x3,
rotation s16 x3); owners send at 15 Hz, only objects that changed, plus all owned enemies (and
platforms that have ever moved) every second. Measured in Bianco Hills 1 (6 Stus + the moving
platforms): about 140 entries/s per stage (the receivers' counters), about 3.5 KB/s in total. The
budget for the full version: 16 KB/s per player at 15 Hz; send distant objects (beyond 3000
units from every non-owner) at 2 Hz. Receivers render 100 ms behind the newest state,
interpolating position and (unwrapped) angles between the two samples around that time.

**Puppet mode.** One `SMS_PORT_AURORA` hook in `TLiveActor::perform`: while any actor is a
puppet (`gPortMpObjPuppets > 0`), `port_mp_obj_puppet_move(actor)` sets the interpolated pose,
moves the object's map collision (`setGroundCollision`) and returns 1, and `moveObject()`
(control, nerves, bind, gravity) is skipped. Animation, drawing, shadows and hit collision
stay. Hits from the local Mario on a puppet still reach its `receiveMessage` (Mario takes contact
damage and bounces as usual), but the reaction nerve does not run there; phase 2 forwards the
hit to the owner (`HIT`), whose copy reacts and whose state everyone then follows.

**RNG and timing hazards.** The game's `rand()` (w9's GameCube-compatible generator) is shared
by everything: running fewer actors on a client (puppets skip their AI) changes the random
sequence for the rest of that client's world, so non-synced objects diverge more than before;
the owner model makes that harmless for synced ones. Never seed or step `rand()` for the sync.
Timers (state timers, `mStateTimer`, spawn delays) run per client; the owner's say wins
through the alive flag. Generators respawn enemies on every client: a respawn on a non-owner
is overridden by the owner's alive flag. Logic runs at 30 Hz on every client, but the clients
are not frame-locked: never compare frame counts across clients, use receive times.

**Risks, and what single player must never see.**
- Nothing runs unless a session is active and the host chose "Experimental"; the hook costs one
  load and branch per actor per frame (`gPortMpObjPuppets == 0`) otherwise. The hook also drops
  all state when the session or the stage changed, so no later actor at the same address is
  ever taken for a puppet. Single player and the GameCube build are unchanged (dtk shasum, smoke,
  and a main-build replay re-played on this build with 0 differing checkpoints).
- Skipping `moveObject` also skips side effects some objects do there (spawning, sounds, state
  timers). Each class added to the sync list needs a check of its `control()`.
- AI state on hand-off: phase 2 carries the nerve and HP (the time spent in the nerve and
  class-specific fields are not carried; the new owner starts the nerve over).
- Enemies that target "the" Mario (`gpMarioOriginal`) target the owner's Mario only; with
  nearest-player ownership that is the right one.

## Enemy sync, phase 2 (P8a, w11: `port/src/port_mp_objects.cpp`, hooks via `port/include/port_mp_obj.h`)
- Host setting in the lobby panel section "Enemy sync (experimental)": **Sync enemies: Off
  (default) / Experimental**, `reserved[2]` = 0 / 1, synced to all, applies from the next frame
  (latched per stage visit; switching off releases every puppet). Clients see it read-only,
  plus the object counts of their stage.
- **Synced enemies** (all `TSmallEnemy`): Strollin' Stu family (`HAMU_KURI`, `HANE_HAMU_KURI`,
  `DANGO_`, `FIRE_`, `DORO_HAMU_KURI`, `DORO_HANE_KURI`), `NAME_KURI`, Electrokoopas
  (`ELEC_NOKONOKO`), Cataquacks (`POI_HANA`), Bloopers (`GESSO`, `MAME_GESSO`), `TAMA_NOKO`,
  `BOMB_HEI`, rolling enemies (`IGAIGA`, `GOROGORO`). Left out: goop Piranha Plants (`TPakkun`)
  and Boos (they use the dead flag while hidden, so a puppet would play a real death), Pokeys
  and Wigglers (multi-part), Popo (Petey's wake-up trigger). Plus moving map objects with
  collision, as in the prototype. Bianco Hills 1: 109 candidates (13 enemies, 1 boss); Ricco
  Harbor 1: 129 (27 enemies, 1 boss).
- **Nerve and HP** in every state entry. Nerves travel as their offset in the exe image and are
  used only between peers whose exe has the same build id (PE timestamp + image size, in every
  header) and only if the offset points at an object with a vtable inside our image. On an
  owner change the new owner continues from the last state it received: the owner's nerve
  (`reset` + `setNext` + default after it, the game's own pattern) and HP.
- **Hit forwarding** (`HIT`, to the owner): before our Mario's stomp (`trampleExec`), hip drop /
  super hip drop / punch (`hitNormal`, `hitHipDrop`) or our water (`TModelWaterManager::move`,
  the point-blank spray in `hitNormal`) reaches an enemy another player runs, the hit is sent
  to its owner: reliable for Mario's hits, unreliable and at most 10 per second per enemy for
  water. The owner applies it with the game's `receiveMessage`: water through
  `TModelWaterManager::mStaticHitActor` at the reported particle position, Mario's hits with its
  own Mario moved to the attacker's position for the length of the call (knock-back direction).
  Electrokoopas get only water forwarded (their stomp reaction shocks "Mario", which on the
  owner would be the owner's Mario).
- **Bosses: Petey Piranha (`BOSS_PAKKUN`), Gooper Blooper (`BOSS_GESSO`).** Owned by the nearest
  player (the one fighting). The others run the boss AI themselves (animations, tentacles, goop,
  attacks on their own Mario) but follow the owner while their own copy is in its fight: every
  nerve change is forced, HP and pose are taken over. The bosses' damage functions
  (`TBossGesso::gotTentacleDamage/gotEyeDamage/gotBeakDamage`, `TBossPakkun::gotHipDropDamage/
  gotWaterDamage/gotFlyingDamage`) send `BOSS_DAMAGE` to the owner instead of applying it
  locally; the owner calls the same function. Petey's copy that is not in its fight (his
  manager's flag, as in `TBossPakkun::receiveMessage`) is never forced or damaged: forcing a
  fight nerve on it crashed in `changeBck` in a test.
  Follower nerves restart with the owner's nerve changes; how long a nerve lasts is still decided
  locally, so a long damage animation can end earlier on a follower than on the owner.
- **Wire** on `PORT_MP_CH_OBJECTS` (no change to `port_mp.h`): header `{u8 type, u8 stage,
  u8 scenario, u8 count, u32 seq, u32 build}`; `STATE` (type 1, unreliable, owner to all) entries
  `{u32 id, u8 flags (bit 0 alive, bit 1 nerve valid), u8 hp, s16 pad, f32 pos[3], s16 rot[3],
  s16 pad, u32 nerve}` (32 bytes, 30 per message); `OWNERS` (type 2, 0x82 continuation, leader to
  all) entries `{u32 id, u8 slot, u8 pad[3]}` = objects not owned by the leader; `HIT` (type 3,
  to the owner) `{u32 id, u32 message, f32 pos[3], u8 water, u8 pad[3]}`; `BOSS_DAMAGE` (type 4,
  reliable, to the owner) `{u32 id, u8 kind, u8 pad[3]}` (kinds in `port_mp_obj.h`).
- Game-side hooks, all `SMS_PORT_AURORA`, one load + branch while nothing is a puppet:
  `liveactor.cpp` (puppet move), `MarioCheckCol.cpp` (`hitNormal`, `hitHipDrop`),
  `MarioCollision.cpp` (`trampleExec`), `ModelWaterManager.cpp` (`move`), `bossgesso.cpp` and
  `bosspakkun.cpp` (damage functions).
- Debug: `SMS_PORT_MP_SYNC_ENEMIES=1` (host turns the setting on at start),
  `SMS_PORT_MP_OBJ_LOG=1` (every 5 s: counts, hits sent/applied, owner/HP/nerve/position of 4
  enemies, every boss and 3 moving platforms; ownership changes, take-overs, deaths, respawns,
  hits), `SMS_PORT_MP_OBJ_DUMP=1` (managers of each stage),
  `SMS_PORT_MP_OBJ_TEST="<name>:<action>@<frame>;..."` with `near` (our Mario next to the first
  synced enemy), `kill` (nearest enemy we own), `stomp` / `spray` (a stomp / water hit on the
  nearest enemy someone else runs, through the forwarding path), `boss` (the stage's boss takes
  one hit through its own damage function: Gooper beak, Petey hip drop; refused while that copy
  is not in its fight).
- Phase 3 (w11):
  - Goop Piranha Plants (`PAKKUN`) and Boos (`TELESA`) are synced now: they hide with
    `LIVE_FLAG_HIDDEN`, which the STATE carries (flags bit 2) and puppets mirror.
  - Pokeys (`SAMBO_HEAD`, `HANA_SAMBO`) and Wigglers (`CHUU_HANA`) run in follow mode like bosses
    (`KIND_FOLLOW`), because their own AI moves the body parts: the AI runs locally, and nerve
    changes, HP and pose come from the owner, plus deaths and respawns.
  - Bianco 2,0, 2 instances: alive/position agreement per type is Stu 91%/97%, goop Piranha
    100%/100%, Boo 92% (never both visible), Pokey head 89%/100%. NAME_KURI is at 70% alive: a local
    respawn while the owner's copy is still dead isn't killed again (a re-kill was tried, didn't
    help, and was taken out).
  - w7's moving-platform module (`port_mp_mapobj.cpp`, channel 21) is not merged. Moving
    platforms stay in `port_mp_objects.cpp` (pose from the nearest owner, tested since P8).
    Breakable objects belong to P8c.
  - w14's read-only checker `port/src/port_mp_synccheck.cpp` (`SMS_PORT_MP_SYNCCHECK=1`) is
    taken in unchanged. It was used for the checks above and for the pause check below.

## Local-only stops in a session (w11: `port/src/port_mp_world.cpp`)

In the original game, the pause menu, the guide map (Z) and the save prompt stop every actor:
`TMarDirector::direct` leaves `CUE_MOVE` and `CUE_CALC_ANIM` out of the movement pass. An NPC talk
stops enemies, enemy attachments, map objects and items, whose `perform()` checks
`isTalkModeNow()`. In a session that froze the world on one screen only, and the instances
desynced (user report: "pausing in multiplayer freezes npcs only for the player").

While `port_mp_active()`, things work like sm64coopdx:
- **Pause menu, guide map, save prompt** (director states 5, 10, 11):
  - The movement and animation passes keep running.
  - Only the local Mario stands still: `TMario::perform` drops `CUE_MOVE`, which also stops his
    FLUDD, cap and Yoshi, as in the original pause.
  - Remote puppets and every sync module keep running from `port_mp_logic_frame`.
  - The log shows `[mp] menu open (director state N): the world keeps running, Mario waits` /
    `[mp] menu closed`.
- **NPC talk**: TSmallEnemy, TEnemyAttachment, TMapObjBase, TItem, TTamaNoko and the Gatekeeper
  keep moving. Mario is invincible while he talks (`TMario::isInvincible` checks the talk mode).
- **Single player**: every check is false, so the game behaves exactly as the original. The
  GameCube build is unchanged (all hooks are under `SMS_PORT_AURORA`).
- **Not changed (still local-only stops):**
  - (Fixed by w15:) the Shine-get demo no longer stops the world in a session: the entries of the
    full perform lists that the Shine lists do not hold run as well (BOOTLOG P12).
  - Demo cameras / cutscenes (`unk124` 3/4): actors keep moving, but enemies don't look for
    Mario (`isFindMario`).
  - Map objects' own `isDemo()` checks (talk or demo), the Boo block (`TTelesaBlock`) and the
    Electrokoopa flower in a talk.
  - (Fixed since: the camera holds still behind the menu, CPolarSubCamera::perform drops CUE_MOVE like the original pause.)
- **Check:** `mp_test.py -n 2 --host-pad 3600:START,4200:START` with
  `SMS_PORT_MP_SYNC_ENEMIES=1 SMS_PORT_MP_SYNCCHECK=1` in Bianco.
  - During the host's 10 s pause, about 30 of its actors kept moving each second, and 95% of the
    actors alive on both instances stayed within 150 units: the same as before and after the
    pause.
  - The host's Mario didn't move.

## Map-object reactions (w11: `port/src/port_mp_mapreact.cpp`, channel 21)

User report from a Delfino Plaza playtest: spraying the rooster weathervane, a hanging sign or a coin spot only reacted on the sprayer's screen (remote water is visual only, w15), and so did the coin it dropped.
- **Water:** our water landing on a reviewed object goes through the TModelWaterManager hook (`port_mp_obj.h`, now also gated on `gPortMpMapReact`). It is counted per object and sent every 3 logic frames to everyone (reliable, `PORT_MP_CH_MAPREACT` = 21, the number main keeps for w11; w15 has 22): id, number of hits, last particle position. Receivers apply that many water hits through the object's own `receiveMessage`, so the game's own reaction runs: swing direction, the vane's speed-up per hit, the reveal.
- **Objects** (by map-object data name, `TMapObjBase::unkF4`): `billboard*` (TMapObjBillboard: hanging signs), DPT_WEATHERCOCK (TMapObjTurn), `WaterHitHideObj` (spray-here coin spots). Plaza 1/2 has 8 of these. Nozzle boxes, the red X, grates, fruit and birds stay with w15; switches, blocks and boxes stay with w10's events.
- **Coins:**
  - Every client's reveal throws a run-time pool coin.
  - A reveal (the object's hidden-coin flag going off) is linked to the pool coin that appeared nearest to it in that frame.
  - When the local Mario takes a linked coin (`TCoin::taken` hook in Item.cpp), TAKEN goes out and the others remove their copy, or remove it as soon as it appears.
  - The coin count is w8's run-time coin credit (channel 19, with shared objectives on), so one take gives one credit.
- **Off** outside a session or alone in the stage (`gPortMpMapReact` = 0). GameCube build unchanged.
- **Debug:** `SMS_PORT_MP_MAPREACT_TEST="<name>:sign|vane|spot [n]@<frame>;<name>:take@<frame>"`. Logged as `[mp-react]` lines, with per-message lines under `SMS_PORT_MP_OBJ_LOG=1`.
- **Check** (plaza 1/2, 2 instances):
  - Host: 1 hit on a sign. The guest got 1 hit applied, and both dropped the coin.
  - Host: 30 hits on the weathervane. Both dropped its coin.
  - The guest took it, and the host's copy was removed ("taken by another player").
- **Not covered:**
  - Mario bumping into a sign (`touchActor`, player collision; puppets don't collide).
  - Other water-reactive map objects (Bianco's windmills, Sirena's slot drums, curtains). Each needs a check of its reaction before it is added to the list.
## World events (P8 EVENTS, w10)
`port/src/port_mp_events.cpp`, channel `PORT_MP_CH_EVENTS` (18), host setting **Sync world events**
(`reserved[6]`, 0 = on, the default since w15; lobby panel section "World events"; latched per stage visit;
debug `SMS_PORT_MP_EVENTS=1`). Complements w11's per-object ownership (enemies, platforms) with
one-off changes, using w11's ids: `(manager index in TConductor's list) << 16 | object index`.
- **Events:** pound switch (`TMapObjSwitch`, hip drop), red-coin switch (`TRedCoinSwitch`), Ricco's
  fruit switch (`TFruitSwitch`), nozzle box trampled open (`TNozzleBox`), brick block broken
  (`TBrickBlock`, head attack), super-hip-drop slab (`TSuperHipDropBlock`). A one-line hook where
  each reacts (`port_mp_event_local(this, sender, kind)`, `SMS_PORT_AURORA`) records it when the
  sender is the local Mario.
- **First claim wins:** event key = (object id, kind, n), n = how many times this player did that
  to that object in this visit; the host keeps the first claim per key per stage visit and
  broadcasts it (`DONE`); a player who did the same thing at the same time keeps his own.
- **Apply** (game thread, once the stage plays): the object's own reaction on our copy: the hit
  message it reacts to, from a stand-in "Mario" actor that ignores the replies (the nozzle box's
  bounce), or `kill()` for bricks (their head-attack test reads the local Mario). The switch
  demo camera is suppressed during a replay (`port_mp_event_applying()` in
  `TMarDirector::fireStartDemoCamera`) and the red-coin switch's pad freeze is undone, so other
  players keep control of their camera and Mario.
- **Late join / stage entry:** a player starting a visit asks the host (`REQ`); the host answers
  with that stage + scenario's events in order (`SET`); they are replayed once. The host forgets
  a stage's events when nobody is in it (a revisit starts fresh, as on GameCube).
- **Not covered yet** (next): sand castles / sand birds (Gelato), Shine gates and Shine-spawning
  tasks, goop gates (pollution = w11's P8 item 4), doors, cages; runtime-spawned objects (enemy
  drops, generated items) would need a spawner id + spawn index from the owner and stay local.
- **Checked** (debug `SMS_PORT_MP_EV_TEST=<frames>` makes the local Mario trigger the next object;
  short sync checks, no playthrough): plaza 1/2 super-hip-drop slab, Ricco 3/2 both fruit switches
  and a nozzle box, Pinna Park inside 13/0 two brick blocks: the guest applies each (objects dead
  3 s later where they break), and a late joiner gets and replays the stage's events.
**Testing rule (user):** no bot playthroughs of boss fights, episodes or the story: the user
plays those. Short movement / debug / sync-check runs are fine (warps, `SMS_PORT_DEBUG_CALL`
teleports, `SMS_PORT_MP_DEBUG_COLLECT`, `SMS_PORT_MP_EV_TEST`). Anything that needs real play goes
into a playtest list in the report. Every test instance runs silent and hidden
(`SMS_PORT_AUDIO=0`, `SMS_PORT_OFFSCREEN=1`) on random ports.

## NPC sync (P8, w12: `port/src/port_mp_npcs.cpp`)
Sibling of w11's enemy sync, same model (owner = nearest player, chosen by the stage leader; puppets elsewhere) and the same host setting: **Sync enemies: Experimental** (`reserved[2]`) also turns this on. No new settings byte.
- **Synced:** every `TBaseNPC` (hit category NPC) in the conductor's managers except the dummy talk targets and sign boards: Piantas, Nokis, Toads, Toadsworth, Peach, the plaza/Pianta/Noki/Bianco villagers. Delfino Plaza (free mode) has 35, Pianta Village 1 has 27.
- **Ids:** `(manager index in TConductor's list) << 16 | object index`, as w11's.
- **State, owner to all at 15 Hz (changes) + a 1 s keyframe:** position, angles, the NPC MActor's current BCK index, frame and rate (so walking, turning, the wet/knocked reactions to the owner's spray and trampling, and the carried pose show on everyone), flags (alive, held by its owner's Mario, hidden) and the goop amount (`mPollutionAmount`; the puppet also gets its colour alpha). About 3-4 KB/s per stage.
- **Puppet:** one `SMS_PORT_AURORA` hook in `TBaseNPC::perform` (NPCs clear `CUE_MOVE` before `TSpineEnemy::perform`, so w11's `TLiveActor::perform` hook never sees them): while `gPortMpNpcPuppets > 0`, `port_mp_npc_puppet_move(npc)` sets the pose 100 ms behind the newest sample (angles wrapped to (-180, 180]), switches the BCK when the owner's differs and keeps the frame within 4 frames, then `moveObject` and the walk/turn bookkeeping are skipped. `changeNerveProc_` still runs, so the talk cursor and talking work on a puppet.
- **Local overrides:** an NPC our Mario is talking to (`TMarDirector::unkA0` during talk/demo) or holding runs locally until that ends; the owner keeps sending and the puppet resumes afterwards. Dialogue, shines from talks and event scripts stay per player. A carried NPC keeps its owner (no hand-off while held).
- **Wire on `PORT_MP_CH_NPCS` (17):** header `{u8 type, u8 stage, u8 scenario, u8 count, u32 seq}`; `STATE` (1, unreliable) entries 40 bytes `{u32 id, u8 flags, u8 pollution, s16 bck, f32 pos[3], s16 rot[3], u16 frame x8, s16 rate x256, u8 btp+1, u8 btk+1, s8 parts[5], u8 state1D8, u8 scaleY x64, u8 partBtp+1}`; `OWNERS` (2, 0x82 continuation, leader to all, reliable on change, every 2 s) entries `{u32 id, u8 slot, u8 pad[3]}` = NPCs not owned by the leader; `HIT` (3, to the owner) `{u32 id, u32 message, f32 pos[3], u8 water, u8 pad[3]}`.
- **Reactions to other players (Peach's umbrella report):** "when a player sprays Peach, the others don't see her block it with her umbrella". Two causes, both fixed:
  - A non-owner's water or stomp never reached the NPC: his copy is a puppet (AI skipped), and w11's hit forwarding only knows enemies. Now `TBaseNPC::receiveMessage` (one `SMS_PORT_AURORA` hook, `port_mp_npc_puppet_hit`) sends `HIT_MESSAGE_SPRAYED_BY_WATER` (unreliable, 10 Hz) and `HIT_MESSAGE_TRAMPLE` (reliable) on a puppet to its owner, who runs them through its copy's own `receiveMessage` (water: the water manager's hit actor at the particle's position; stomps: our Mario standing in at the stomper's position). The puppet copy only acknowledges the hit (splash, Mario's bounce) and does not push the reaction nerve, which could otherwise replay when the NPC runs locally again. Taking and hits from objects (barrels) stay local.
  - The owner's state only carried the body BCK: Peach's parasol, hair and ponytail are parts models (`TNpcParts`, slots 0/3/4 at LOD row 0; the Noki elders' parts 9/10) with their own BCK that `TBaseNPC::setNpcAnm_` switches with the body's (the parasol goes to "no animation" while she blocks), and her `unk1D8` state (parasol in / tired / recovered) picks which parts are drawn. The state now carries the parts' BCK indices (started at the body's frame, kept within 4 frames), `unk1D8`, the trample squash (`mScaling.y / mInitialScale.y`) and the slot-10 part's BTP; the puppet also starts the NPC's own motion blend when the body BCK changes (`onStopMotionBlend`, as the owner's `setNpcAnm_` does), so a new animation blends in instead of snapping.
- **Checked** (2 and 3 instances, `SMS_PORT_MP_START_AREA` for Pianta Village): the guest's walker NPC follows the host's within the interpolation delay (same position, angle and BCK in both logs); walking up to an NPC moves its ownership to that player; the guest spraying an NPC plays the wet reaction (BCK 21/20) on the host's puppet; 3 instances in Pianta Village with one guest owning all 27 NPCs. Not checked: carrying (NPCs are only carriable in specific events, `setFlagNPCCanTaken`) and goop-covered NPCs being cleaned (needs a stage with polluted NPCs).
- **Checked (umbrella report, 2 instances, plaza 1/5, `SMS_PORT_MP_NPC_WATCH` on both):** host next to Peach sprays her: the guest's puppet plays the same sequence (BCK 16 -> 18 parasol-wet with the parasol part's BCK 17 -> none, BTP 5 -> 2, blend 9, back to 16 after 224 frames), ~100-150 ms behind; the far guest sprays her: the hit goes to the host, Peach blocks on both. Same for a Toad: the host's stomp squashes it (scale 64 -> 29 -> 75 -> 64) on both, the far guest's stomp is forwarded and squashes it on both, spray plays the wet BCKs 10 -> 8 -> 6 on both.
- **Debug:** `SMS_PORT_MP_NPC_LOG=1` (every 5 s: counts, owners, hits sent/applied, 5 NPCs with position, angle, BCK, goop; ownership changes). `SMS_PORT_MP_NPC_TEST="<name>:<action>[:<type>]@<frame>;..."` (`list` all synced NPCs; `near`, `spray`, `stomp` the nearest NPC or the first of `<type>` = `peach` | `kinojii` | `kinopio` | hex actor type, through the real `receiveMessage` path). `SMS_PORT_MP_NPC_WATCH=<type|id>`: one `[npc-watch]` line per logic frame with that NPC's body BCK/frame/rate, BTP, parts BCKs, `unk1D8`, squash scale and blend timer, for comparing an owner's log with a puppet's.
- **Note for w11:** the game-frame hook runs while a stage is still loading. `port_mp_objects.cpp` builds its list at the visit's 2nd frame; in the free-mode plaza that is before the scene exists, so it logs "no conductor, nothing to sync" there. `port_mp_npcs.cpp` waits for `gpMarDirector->mState == 4`.
## Debug knobs (port_mp_game.cpp)
- `SMS_PORT_MP=host[:port]` or `SMS_PORT_MP=join:<ip>:<port>`: after the Nintendo logo,
  host or join and start free mode directly (skips title/file select, like `SMS_PORT_WARP`).
  Server mode: `SMS_PORT_MP=hostname:<lobby name>` / `joinname:<lobby name>`, with
  `SMS_PORT_MP_PASSWORD`, `SMS_PORT_MP_SERVER=<host:port>`, `SMS_PORT_MP_FORCE_RELAY=1`.
- `SMS_PORT_MP_MENU_AUTO=copytest`: opens the lobby panel, presses the first Copy button,
  reads the clipboard back (logged) and restores the previous clipboard text.
- `port/tools/mp_server_test.py [--phase unit|1|2|3|all]`: the lobby server on 127.0.0.1 with
  several instances (unique names, join by name, relay, expiry, LAN fallback, password, reuse).
- `SMS_PORT_MP_NAME=<name>`, `SMS_PORT_MP_INI=<path>` (isolated name/settings per instance),
  `SMS_PORT_MP_LOG=1` (net log every 5 s: per-peer RTT, snapshots rx/tx, resends, and each
  remote player's latest tick/stage/position), `SMS_PORT_MP_LOBBY=<name>`, `SMS_PORT_MP_SHARED=1`.
- `SMS_PORT_MP_MENU_AUTO=host[:port]|join:<ip>[:port]`: presses Create/Join two seconds after
  the menu opens (unattended test of the file-select entry).
- `SMS_PORT_MP_LEAVE_AT=<frame>`: leave the session at that frame (tests the return path).
- `SMS_PORT_MP_DROP=<percent>`: drop that share of sent and received datagrams (loss tests).
- `SMS_PORT_MP_BLACKOUT=<start s>,<duration s>`: nothing in or out in that window (drop tests).
- `SMS_PORT_MP_EVENTS=1` (debug host: world events on), `SMS_PORT_MP_EV_TEST=<frames>` (the local
  Mario triggers the next switch / box / block at those logic frames of the stage).
- `SMS_PORT_MP_START_AREA=<area>,<scenario>`: the debug auto-start loads this stage instead of
  the plaza (e.g. 60,0 + `SMS_PORT_DEBUG_CALL=bathdemo@...` for the ending in a session).
- `SMS_PORT_MP_FLAGS=<items>`: extra save flags after the saved copy (e.g. `20001=0`: no spare
  lives, for a game over in a session).
- `SMS_PORT_MP_DEBUG_COLLECT=<frame>[,<frame>...][:coin|blue|red]`: at each frame, put Mario on
  the next scene-placed item of that kind, so the real touch path collects it.
- `SMS_PORT_MP_TESTSNAP=1`: a minimal snapshot of the local Mario (position, angles, action,
  animation id, stage) from `port_mp_game.cpp`, to test the transport without w15's capture.
  Ignored while w15's capture is calling `port_mp_set_local_snapshot` (2 s window), so it is
  safe to leave on in a merged build.
- `port/tools/mp_test.py`: runs 2-4 instances off-screen with isolated user dirs and checks
  joins, leaves, snapshot flow and stage presence in their logs.

## Task board (lead: w10)
Pick a package whose owner is `free`, write your worker id into **Owner** (in main and in
your worker), and keep to its files. Everything shared goes through `port_mp.h`; ask the lead
(w10, via the coordinator / BOOTLOG "Multiplayer (w10)") before changing it.

**How a feature plugs in without touching w10's files** (all in `port_mp.h`):
- its own network channel: `port_mp_send(channel, data, len, reliable, to)` /
  `port_mp_recv(channel, ...)`; the host relays. Take a `PORT_MP_CH_*` value listed in the
  header (or the next free one, and add it there).
  Taken beyond the header so far: 16 (w6 goop, and w8 `port_mp_objectives_extra.cpp`, which clash),
  20 `PORT_MP_CH_WATER` (w15, `port_mp_water.cpp`: the other players' FLUDD water, visual-only
  particles; BOOTLOG P10).
- per-frame code: `port_mp_register_game_frame(fn)` (game thread, logic frames in a stage),
  `port_mp_register_overlay(fn)` (ImGui, every presented frame), `port_mp_register_lobby_section
  (title, fn)` (a collapsible section in the lobby panel; host options go here), all from a
  static constructor in the feature's own .cpp. `port_mp_request_input(1/0)` idles the game's
  pads while the feature takes input.
- host options: a byte of `PortMpSessionSettings.reserved[]` (allocation below), changed with
  `port_mp_host_set_settings`, synced to all, read with `port_mp_get_settings`.
- game-side hooks: only under `#ifdef SMS_PORT_AURORA`, inert outside a session; check the
  GameCube build (`dtk shasum`) after every shared edit.

`PortMpSessionSettings.reserved[]` allocation: [0] interaction mode (P2), [1] chat/emotes (P3: bit 0 = chat off, bit 1 = emotes off; 0 = both on),
[2] object sync on/off (P8; also NPC sync, w12), [3] level select / warp menu (P5: 0 = allowed, default; 1 = host disabled it), [4] FLUDD pushes players (P2: 0 = yes, default; 1 = no), [5] goop sync (w6), [6] sync world events (P8 EVENTS: 0 = on, default; 1 = off), [7] remote Shines (P1b: 0 = toast only, default; 1 = also the "Shine Get" banner); the rest free (list it here when you take one).

Depends on netcode = needs w10's stage-2 files (`port_mp_net.cpp`, `port_mp_menu.cpp`,
`port_mp_game.cpp`), which are in the w10 worker and merge-ready; until merged, build on top of
a copy of them (do not edit them).

| # | Package | Owner | Files it owns | Interfaces | Netcode |
|---|---|---|---|---|---|
| P0 | Core: transport, sessions, menus, file-select entry, free mode, in-memory save | w10 | `port_mp_net.cpp`, `port_mp_menu.cpp`, `port_mp_game.cpp`, `port_mp.h`, `mp_test.py`, this file | everything above | - |
| P1 | Shared objectives (coins, red/blue coins, Shines; host setting, late join) | w10 (done, stage 4) | inside `port_mp_game.cpp`/`port_mp_net.cpp`; hooks in `Item.cpp`, `ItemManager.cpp`, `MarDirectorDirect.cpp` | `port_mp_objective_send/poll`, ids `PORT_MP_OBJ_ID` | yes |
| P1b | Shared objectives follow-ups: coin rings / flower coins, shared red-coin timer, pickup sparkle on the collecting puppet (with w15), toast polish | free | new `port_mp_objectives_extra.cpp` + small hooks | P1's ids; `PORT_MP_CH_OBJECTS` sub-range or a new channel | yes |
| P2 | Player interaction (coopdx-style bump, stomp, knockback, ground-pound, FLUDD; host modes: full / solid only / off) | w8 (done, see "Player interaction (P2)") | `port_mp_interact.cpp` (game side), `port_mp_interact_ui.cpp` (lobby section); no game-source hook needed | reads puppets via `port_mp_get_snapshots`; `PORT_MP_CH_INTERACT` (attacker -> victim hit message); `reserved[0]`, `reserved[4]` | yes |
| P3 | Text chat + emotes (chat box: T / controller; emote wheel plays a Mario animation, puppets follow via snapshots) | w6 (done, see "Chat and emotes") | `port_mp_chat.cpp`, `port_mp_emote.cpp`, `tools/mp_chat_test.py` | `PORT_MP_CH_CHAT`, `PORT_MP_CH_EMOTE`, overlay + `port_mp_request_input`; `reserved[1]` | yes |
| P4 | Character customisation menu UI (palette model by w15) | w15 (done: F4 window, lobby section; w10 please make the stub "Character" page call `if (port_mp_custom_draw()) openPage(PAGE_MAIN);`) | new `port_mp_custom.cpp`; w10 replaces the stub "Character" page with one call `port_mp_custom_draw()` on request | `PortMpCharacter`, `port_mp_set_character` (synced as PLAYER_INFO), `PORT_MP_CH_CUSTOM` for extras | yes (sync) |
| P5 | Stage presence: where-is-everyone list/HUD, "warp to player", free-mode level select / teleport menu | w11 (done, see "Stage presence (P5)") | `port_mp_presence.cpp` | `port_mp_get_player` (stage/scenario), `PORT_MP_CH_WARP`, `reserved[3]`, game frame hook (next area + the director's stage-transition flag, on the game thread) | yes |
| P6 | Network robustness: loss/latency simulation (`SMS_PORT_MP_DROP`, `SMS_PORT_MP_BLACKOUT`), reconnection after short drops, graceful host leave, host migration (lowest slot takes over; server `TAKEOVER`) - all done | w10 | `port_mp_net.cpp`, `port/server` | protocol 4 | yes |
| P7 | Automated multiplayer tests in `smoke_all.py` (2-4 instances: join, leave, snapshots, app messages, shared objectives, fuzz) | free | `smoke_all.py` multiplayer scene, `mp_test.py` (co-owned with w10) | `mp_test.py` checks; `SMS_PORT_MP_*` debug knobs | yes |
| P8 | Enemy/object sync (the big one): design, ownership (nearest player, chosen by the stage leader), stable ids, bandwidth | w11 (design done, see "Enemy and object sync (P8)") | design section in this file | `PORT_MP_CH_OBJECTS`; `reserved[2]` | yes |
| P8a | Enemies + moving platforms + bosses: nerve/HP in the state with take-over, hit forwarding, TSmallEnemy families, Petey Piranha and Gooper Blooper | w11 (phase 2 done, see "Enemy sync, phase 2") | `port_mp_objects.cpp`, `port_mp_obj.h`; hooks in `liveactor.cpp`, `MarioCheckCol.cpp`, `MarioCollision.cpp`, `ModelWaterManager.cpp`, `bossgesso.cpp`, `bosspakkun.cpp` | `PORT_MP_CH_OBJECTS` types 1-4; `reserved[2]` | yes |
| P8b | NPC sync (positions, animation, spray/trample reactions, carried NPCs, goop amount; talk stays local) | w12 (first version, see "NPC sync (P8, w12)") | `port_mp_npcs.cpp`; hook in `NpcBase.cpp` (`TBaseNPC::perform`) | `PORT_MP_CH_NPCS`; `reserved[2]` (shared with P8) | yes |
| P8c | Switches, cages, Shine gates (events, first claim wins, late-join table) and run-time spawned objects (owner-assigned ids) | w10 | w10's files | its own channel (the next free `PORT_MP_CH_*`: `port_mp_recv` takes a channel's messages for one reader, and `port_mp_objects.cpp` reads `PORT_MP_CH_OBJECTS`); builds on P8's stage leader / ownership (ask w11 for an accessor if needed) | yes |
| P8d | Goop / pollution cleaning stream (cleaned cells as deltas of the layer bitmaps, late-join snapshot) | w6 | w6's files | its own channel (see P8c) | yes |
| P8e | World events: switches, nozzle boxes, blocks (first claim, late join; host setting "Sync world events") | w10 (first set done, see "World events (P8 EVENTS)") | `port_mp_events.cpp` + one-line hooks in Item.cpp, MapObjTown.cpp, MapObjRicco.cpp, MapObjBlock.cpp, MarDirectorEvent.cpp | `PORT_MP_CH_EVENTS`, `reserved[5]`, w11's ids | yes |
| P9 | Lobby / rendezvous / relay server for internet play (join by lobby name) | w10 (done: `port/server`, client in `port_mp_net.cpp`; deployment is the user's) | `port/server/*` | lobby server protocol above | yes |
| P10 | Puppets: model, animation, FLUDD, Yoshi, name tags, ghosts | w15 | `port_mp_puppet.{h,cpp}`, `port_mp_overlay.cpp` | snapshot w15 block | yes |
## Chat and emotes (P3, w6)
Files: `port/src/port_mp_chat.cpp` (ImGui overlay, input, network; no game
headers), `port/src/port_mp_emote.cpp` (game side, a registered game-frame callback). Only
`port_mp.h` extension points; nothing runs outside a session.
- **Chat:** T opens an input line (Enter too, when Enter is not bound to a pad button in
  `[keyboard]`; `/` opens it with "/"), the right-stick click opens an on-screen keyboard (same
  layout as the lobby menus; a copy, since that one is file-local in `port_mp_menu.cpp`) (GameCube pads:
  the right-stick click). Pads are idle while it is open
  (`port_mp_request_input`; a watchdog thread hands them back if the session ends meanwhile).
  Lines: bottom-left of the picture (above the plaza news ticker, around w10's toasts), 10 s
  then a 2 s fade; the last 12 while typing. System lines for joins/leaves/refusals.
  Limits: 120 bytes of printable UTF-8; we send at most 5 per 10 s, 0.5 s apart; receivers drop
  more than 8 per 10 s per player. Mute per player with `/mute <name>` (`/unmute`). Commands: `/<emote>`,
  `/emotes`, `/me <text>`, `/help`.
- **Emotes:** D-pad (unused by Mario) = quick emotes (up Shine pose, right Pianta pose, down
  Sleep, left Aww); G or the left-stick click opens the emote menu; chat commands. Seven: Shine pose (`ANIM_DEMO_SHINE_GET`), Phew (`ANIM_HOT_WAIT`),
  Aww (`ANIM_GET_FAIL`), Sleep (the game's own `MARIO_STATUS_SLEEPY` chain), Pianta pose
  (`TMario::changeMontemanWaitingAnim`), Yawn (`ANIM_YAWN`), Sit (`ANIM_SIT` then
  `ANIM_SIT_WAIT`). The animation ones put the local Mario in `MARIO_STATUS_NOMOTION` and call
  `TMario::setAnimation` each frame, so **w15's capture carries them to puppets unchanged** (no
  snapshot field added; checked: the host sees the guest's puppet raise its arm, stand with
  hands on hips and sit). Only from a plain idle stance (WAIT / dozing, upper body idle, not
  holding, not on Yoshi, not in water, no talk/demo); stick, an action button, damage, the
  floor moving or a talk/demo ends it (forced back to WAIT: NOMOTION refuses unforced status
  changes, so a hit during an emote takes HP without its knockback status; the emote ends at
  once). Others get a dim "* name: emote" line.
- **Host options:** Chat / Emotes checkboxes on the title menu's Host a Lobby page (saved as
  `chat` / `emotes` in port_multiplayer.ini), put in `reserved[1]` when the lobby is created. The F2
  lobby panel has no chat, emote or character section (character: the Customize block, or F4).
- **Wire (version byte 1):** `PORT_MP_CH_CHAT` reliable to all: `[u8 1][u8 flags: bit0 =
  /me][u8 r,g,b][u8 n][n bytes name][text]` (the name shown is the player table's; the colour is
  the cap colour of the sender's character, else one per slot). `PORT_MP_CH_EMOTE` reliable to
  all: `[u8 1][u8 emote id]` (ids are the table order in `port_mp_emote.cpp`; append only).
- **Debug knobs:** `SMS_PORT_MP_CHAT_AUTO=<slot>@<frame>:<text>|...` (send as typed; `!open:`,
  `!osk:`, `!emotes`, `!close`, `!chatoff`), `SMS_PORT_MP_EMOTE_AUTO=<slot>@<frame>:<id>|...`.
  `python port/tools/mp_chat_test.py -n 3` runs host + 2 guests through chat, /me, the rate
  limit, emotes (keyboard D-pad, command, knob), the input line / on-screen keyboard / emote
  menu, the lobby panel and "chat off", and checks every log.
- **With P2 (w8):** a stomp/pound/FLUDD hit on an emoting Mario goes through `damageExec`, whose
  knock-back status NOMOTION refuses; its invincibility frames end the emote at once, so the
  hit counts but that one knock-back animation is skipped. w8: if that matters, end the local
  emote first (e.g. `changePlayerStatus(MARIO_STATUS_WAIT, 0, true)` when status is NOMOTION).
- **Not done:** speech-bubble pings over puppets: the name-tag screen positions live in
  `port_mp_overlay.cpp` (w15) without the slot; a getter `port_mp_overlay_get_tag(slot, &x, &y)`
  there would let the chat draw bubbles. Suggestion for w10: export the on-screen keyboard
  (`port_mp_osk_*`) so features share one.

## Status
- Stage 1: design + API published (this file, `port_mp.h`).
- Stage 2 (w10 worker): transport, LAN/direct join, menus, file-select entry, free-mode start,
  in-memory save, leave path; `mp_test.py -n 3 --kill-at 65` passes on loopback.
- Stage 3: puppets (w15) in the free-mode plaza: checked with w15's puppet files linked into
  a w10 test build (3 instances, `mp_test.py -n 3 --walk --no-testsnap --shot ... --present`):
  each instance shows the others as Marios with name tags (w10 worker
  `port/build_aurora/mp_shots/stage3_*.png`).
- Robustness: `mp_test.py --fuzz` sends 3000 malformed packets (random bytes, bad lengths,
  truncated headers, unknown connection ids, short CONNECTs, wrong magic) at a running host:
  no crash, no fake player, then a wrong-version CONNECT gets `REJECT` (version) and a
  `DISCOVER` is still answered. Names are cleaned to printable ASCII / well-formed UTF-8.
- Stage 4 (w10 worker): shared objectives sync for yellow, red and blue coins and Shines;
  checked with `SMS_PORT_MP_DEBUG_COLLECT` (host collects 4 coins and a blue coin, the guest's
  counts and flags follow).

## Ambient sync and more world events (w15)
- `port/src/port_mp_ambient.cpp`, channel `PORT_MP_CH_AMBIENT` (22): fruit and animals; authority = the
  stage leader (lowest slot in the stage + scenario), or for a fruit the player who holds it / threw it
  in the last 2 s (latest claim wins). Entries `{u32 id, u8 kind (1 animal, 2 fruit), u8 flags (1 claim,
  2 held, 4 waiting to appear, 8 dead), s16 yaw, f32 pos[3], f32 vel[3]}` after `{u8 version 1, u8 stage,
  u8 scenario, u8 count}`; ids as w10/w11.
- World events: `EV_MANHOLE` (7) grates, `EV_PICTURE` (8) water-hit pictures (red X graffiti, posters,
  finished state only). "Sync world events" is on by default (`reserved[6]` 0 = on).
- Fruit an NPC carries: flag 16 (`F_NPC`); our NPC lets go of a fruit nobody carries there; with NPC
  sync on the fruit goes into the NPC puppet's hands, without it it is hidden while their NPC has it.
- Coordination with w11's generic water-hit forwarding for map objects: pictures are covered here by
  their finished state; forwarding their hits as well is harmless (a cleared picture ignores the event).

## Game modes and the player list (w15)
- `PORT_MP_MODE_NONE` (0, default): Delfino Plaza only; every stage change out of it is refused
  (`TMarDirector::setNextStage`, warp floors, gate warp-in; level select off).
  `PORT_MP_MODE_FREE` (1): every entrance open. `gameMode` = the running mode; the host's "Start"
  (lobby panel) sets it and bumps `reserved[8]`; every client then reloads the plaza start. Late
  joiners take the running mode. Test knob (host): `SMS_PORT_MP_MODE=free|none@<frames>`.
- `PORT_MP_MODE_HIDE` (2): Hide and Seek (locked plaza, one level or all levels: the Map setting below) (`port/src/port_mp_hideseek.cpp`, channel
  `PORT_MP_CH_HIDE` 24): host-run rounds (hide / seek / result), roles per slot, tags asked by the
  seeker's client and decided by the host; settings in the mode control state (BOOTLOG P15).
- `PORT_MP_MODE_FREEZE` (3): Freeze Tag (as Hide and Seek: the Map setting below) (`port/src/port_mp_freezetag.cpp`, channel
  `PORT_MP_CH_FREEZE` 25): host-run rounds (get ready / play / result). Taggers freeze runners by
  touching them (asked by the tagger's client, decided by the host); a frozen runner's pads are idle
  and his name tag is ice blue; another runner's touch thaws him (2 s immunity). Taggers win when every
  runner is frozen, runners when the time runs out. Settings in the mode control state: head start
  (default 10 s), round time (180 s), number of taggers (1), tagger pick (random). Test knobs:
  `SMS_PORT_MP_MODE=freeze@<frames>`, `SMS_PORT_MP_FREEZE_TEST=1` (a tagger jumps onto a runner and a
  runner onto a frozen player once per round).
- `PORT_MP_MODE_PROP` (4): Prop Hunt (`port/src/port_mp_prophunt.cpp`, channel `PORT_MP_CH_PROP` 26;
  the Map setting below). Hiders are Shadow Mario: the game's own `/kagemario/default.bmd`, brush and
  `kagemario_scroll.btk`, read once from the disc's Shadow Mario episode archive (`bianco6.szs`, or
  another episode 7), loaded per stage like `TEMario::init` (flags `0x11300000`, fog packets, the
  `H_kagemario_dummy` screen texture) and posed from the player's Mario joints like
  `TEnemyMario::perform`. Hiders have no FLUDD (`MARIO_FLAG_HAS_FLUDD` off). R next to an object
  (map objects and items with a model, within 400) turns a hider into a copy of it (actor type +
  scene name key, sent to the host, which shares it in the round state); R away from objects turns
  him back. The local hider's own Mario models are skipped (`TMario::perform`, `MarioMain.cpp`, under
  `SMS_PORT_AURORA`) and his look is drawn by the puppet view object (`port_mp_prop_perform`). Hunters
  are normal Marios whose tank empties faster (default 4x). A hunter's water hitting a hider as
  drawn (the disguise's size) is sent to the hider, whose client applies `damageExec` with the
  "Spray damage" (default 2 of 8): knock-back, health meter, invincibility frames. At 0 health he
  dies and is caught (hunts, or is out). FLUDD's player push is off during Prop Hunt. R reads the analog trigger too (the pad's R edge is used up before the mode's frame hook runs); objects bigger than kPropMaxSize (320 units, model bounding box) cannot be become; nothing in reach gives a toast. Hider sound (host setting "Hider sound every", default 30 s, 0 = off; reserved in CtlState propTauntS): during the hunt, every client plays MSD_SE_MA_KAGE_FIELD_APPEAR at each hider's position on the shared round clock (no message); test option taunt=<s>, hidetime=<s>. Hunters see no
  hider name tags, markers or locations in the player list (also for Hide and Seek seekers). Settings:
  hide 45 s, hunt 240 s, damage, water use, caught hiders hunt, hunter pick. Test knobs:
  `SMS_PORT_MP_MODE=prop[...]@<frames>`, `SMS_PORT_MP_PROP_TEST=1` (a hider disguises as the nearest
  object during the hide phase; a hunter sends a spray hit to every hider every 2 s).
- `PORT_MP_MODE_MANHUNT` (5): Manhunt (`port/src/port_mp_manhunt.cpp`, channel `PORT_MP_CH_MANHUNT`
  27). One runner (normal Mario, FLUDD) must collect N Shine Sprites (default 3; counted from the
  shine-get demo, `port_mp_shine_got`, each Shine once); everyone else hunts as Shadow Mario (no
  FLUDD; Prop Hunt's look code). Every level is open (the free-mode plaza) on a copy of the save
  without its Shine flags (`port_mp_mode_flags_for_load` view 2), so every Shine is collectible
  again and episode selects start over. Head start (default 20 s, hunters' pads idle); then a
  hunter touching or stomping the runner costs him health (default 3 of 8, the runner's own
  `damageExec`; interaction stomps/pounds on the runner are off so their invincibility frames do
  not swallow the hits). A hunter entering the runner's level skips the episode select and joins
  his episode (`port_mp_manhunt_route`). The runner wins at N Shines; the hunters when he dies,
  leaves or the time limit (default 20 min, 0 = none) runs out. No warping to players. Test knobs:
  Switch (host option): a hunter who kills the runner (death within 10 s of his hit) becomes the
  runner, the old runner a hunter; a new runner gets FLUDD back and 10 s of grace; a runner who dies
  without a hunter's hit stays the runner. Per-player Shine counts (only while running; each Shine
  once per round, marked taken in everyone's save); first to the goal wins, else the most at the
  time limit (a tie is a draw). Test knobs:
  `SMS_PORT_MP_MODE=manhunt[:switch][:pick=<slot>][:goal=<n>][:head=<s>]@<frames>`,
  `SMS_PORT_MP_MANHUNT_TEST=1` (a hunter jumps onto the runner every 2.5 s; each runner reports one
  Shine twice when his turn starts: 5, then 6 after a switch, ...).
- `PORT_MP_MODE_SHINERUN` (6): Shine Runners (`port/src/port_mp_shinerun.cpp`, channel
  `PORT_MP_CH_SHINERUN` 28). Everyone races for Shine Sprites on Manhunt's save view (every level
  open, every Shine collectible again; each level Shine counts once per round and is then marked
  taken in everyone's save). A player hit by another (the interaction module's stomp, ground pound
  or FLUDD push, applied on the victim: `port_mp_shinerun_hit`) while holding Shines loses one: the
  host puts a drop in the round state (stage, scenario, position, velocity) and every client in that
  stage bounces a copy of the stage's `mapobj/shine.bmd` against the map's floors; the first player
  to touch it (host decides) gets it; it vanishes after 30 s; one knock-out per 2.5 s per player.
  Dying loses the host's death loss (default 3). First to the goal (default 5) wins, else the most
  at the time limit (default 15 min, 0 = none; a tie is a draw). Needs player interaction Full.
  Test knobs: `SMS_PORT_MP_MODE=shine[:goal=<n>][:loss=<n>]@<frames>`, `SMS_PORT_MP_SHINERUN_TEST=1`
  (once play starts, report level Shine 5 + slot twice, knock one out of ourselves 3 s later, straight up;
  when it rests, move onto it, take two screenshots, then pick it up).
- `PORT_MP_MODE_CNR` (7): Cops and Robbers (`port/src/port_mp_cnr.cpp`, channel `PORT_MP_CH_CNR` 29).
  Cops (host setting, default about a third) and robbers on Manhunt's save view (every level open,
  every Shine collectible again). Robbers steal coins (yellow 1, red 2, blue 5: `port_mp_objective_taken`)
  and Shines (`port_mp_shine_got`, each once per round, then marked taken in everyone's save) for a team
  quota (default 100 coins and 3 Shines). A cop knocks a robber over (interaction stomp, ground pound
  or FLUDD push, needs interaction Full) and touches him while he is down or within 1.5 s: the robber's
  own client reports the arrest. Prisoners are sent to the plaza and held inside the police station's
  interior (the room under its bell: x 6380..6620, z 4650..5480, floor y 305; a "PRISON" label over the
  door); leaving the plaza is refused
  (`port_mp_cnr_route`); a free robber's touch releases them (3 s of protection). A free robber who
  rings the police bell (TBellDolpic police, hook in `TBellDolpic::ring`, MapObjDolpic.cpp under
  SMS_PORT_AURORA) opens the prison: every prisoner is released and everyone's police bell swings;
  30 s before the bell works again. Test knob `SMS_PORT_MP_CNR_BELL=1`: robbers ring the bell instead
  of walking in. Robbers win with both
  quotas; cops when every robber is in prison at once or the time (default 12 min) runs out. Head
  start (default 15 s) holds the cops. Name tags: cops blue, prisoners grey "(jailed)". Test knobs:
  `SMS_PORT_MP_MODE=cops[:pick=<slot>][:coins=<n>][:shines=<n>][:head=<s>]@<frames>`,
  `SMS_PORT_MP_CNR_TEST=1` (a cop drops onto a robber every 3 s; a robber reports 10 coins and a Shine).
- Dying (a lost life, a game over) during a round: a hider counts as tagged (seeks, or is out with
  "Tagged players become seekers" off); a runner (or a frozen one) joins the taggers, or is out (role
  "Out") with the Freeze Tag setting "Runners who die become taggers" off. The dying client tells the
  host (`MSG_DIED` on the mode's channel); everyone gets a toast.
- Map setting of Hide and Seek, Freeze Tag and Prop Hunt (`port/src/port_mp_arena.cpp`; the running choice in `reserved[9]`, the
  exit lock in `reserved[10]`, set with the mode by the host's Start):
  - Single level (default: Delfino Plaza, as before; or one of Airstrip, Bianco, Ricco, Gelato,
    Pinna Park beach / park, Sirena, Pianta, Noki): everyone reloads into that level and the match
    stays there. Stage changes out of it are sent back (`port_mp_arena_route`, from
    `port_mp_redirect_title`; areas of the same level such as Sirena's hotel stay reachable, secret
    courses do not), the pause menu has no "Exit Area", "Go to plaza" is refused.
  - All levels: everyone starts in the plaza with every entrance (the free-mode plaza, full save).
    A level entered from the plaza skips the episode select and loads one fixed plain episode of it
    (the same for everyone, so tags see each other); Shine Sprites and red coins are removed in
    both arenas. Exit lock (host setting, default 30 s, 0 = off): each time a hider / runner gets
    control in a level he cannot leave it for that long (Exit Area refused in `PauseMenu2.cpp`,
    "Go to plaza" refused, any other trip to the plaza sent back into the level).
  - "Warp to player" is refused in both modes.
  - Test knobs: `SMS_PORT_MP_MODE=hide|freeze[:all|:<level>][:lock=<s>][:pick=<slot>]@<frames>`,
    `SMS_PORT_MP_ARENA_TEST="enter=<stage>@<n>;exit@<n>;force@<n>;die@<n>"` (n = frames of normal play).
- Player list: top right of the game picture, game font (`gpSystemFont`), cap colours; only in a
  session. `port/src/port_mp_modes.cpp`.

## Change log
- (w10) v1: initial API.
- (coordinator) v2: w15 block v1.1 (held object type in extra[41..43], s8 emitter offset).
- (w10) v4: host migration: `CONNECT` appends u8 re-join slot + u32 serial, reliable kinds `PEERS` and
  `LOBBYKEY`, lobby server `TAKEOVER` (14) and a 30 s lobby expiry.
- (w15, merging) v4 also: world events on `PORT_MP_CH_EVENTS` (18) with `reserved[6]` (w10),
  objectives extras on `PORT_MP_CH_OBJX` (19) with `reserved[7]` (w8).
- (w10) v3 (no wire change): `PortMpPlayerInfo.lagging`, `port_mp_menu_close()`; lobby panel without
  the Where column (w11's presence section shows it); 20 s timeout with reconnection.
- (w10) v3: CONNECT carries a password hash; LOBBY_INFO adds has-password and connection type;
  `PUNCH` packet; `PortMpCharacter.colors[7]` (hair, same bytes as the old reserved[0..2]);
  lobby server client (`port_mp_host2`, `port_mp_join_name`, `port_mp_server_status/refresh`,
  `port_mp_set_server`, `port_mp_local_addresses`, `port_mp_host_port`, `port_mp_conn_type`,
  `port_mp_relayed`).
- (w10) v1: + app messages (`port_mp_send/recv`, `PORT_MP_CH_*`, packet `APP` / reliable kind `APP`) and feature hooks (`port_mp_register_*`, `port_mp_request_input`).
- (w6) P3 chat + emotes: payloads on `PORT_MP_CH_CHAT` / `PORT_MP_CH_EMOTE` (own version byte),
  `reserved[1]` bits; no change to `port_mp.h` or the protocol version.
- (w10) v1: + `PortMpObjKind`, `PORT_MP_OBJ_ID`, `port_mp_objective_send/poll` (shared objectives; no wire change for snapshots).
- (w11) P5 stage presence: `PORT_MP_CH_WARP` payloads (PRESENCE / SPAWN_REQ / SPAWN_POS) and `reserved[3]`; no change to `port_mp.h` or the core protocol.
- (w11) P8 enemy sync prototype: `PORT_MP_CH_OBJECTS` payloads (STATE / OWNERS) and `reserved[2]` = 1 for "Experimental"; one `SMS_PORT_AURORA` hook in `TLiveActor::perform` (liveactor.cpp); no change to `port_mp.h` or the core protocol.
- (w11) P8a phase 2: `PORT_MP_CH_OBJECTS` header gains `u32 build`, STATE entries 32 bytes (HP, nerve), new types HIT (3) and BOSS_DAMAGE (4); `port/include/port_mp_obj.h` hooks; P8 split into P8a (w11), P8b NPCs (w12), P8c (w10), P8d (w6). No change to `port_mp.h` or the core protocol.
- (w12) `PORT_MP_CH_NPCS = 17` (NPC sync, `port_mp_npcs.cpp`); no change to other wire layouts.
- (w11) P8a phase 3: STATE flags bit 2 = hidden (goop Piranha Plants, Boos). Local-only stops (`port_mp_world.cpp`): no wire change.
- (w11) Map-object reactions: channel 21 (`PORT_MP_CH_MAPREACT`, local constant in `port_mp_mapreact.cpp`; name it in `port_mp.h` where 21 is "kept for w11"): WATER (id, count, position) and TAKEN (id).
- (animation sync) NPC sync STATE entries grow from 32 to 40 bytes (parts BCKs, `unk1D8`, trample squash, part BTP) and `PORT_MP_CH_NPCS` gains HIT (3) to the owner; one `SMS_PORT_AURORA` hook in `TBaseNPC::receiveMessage`. Snapshot flag `PORT_MP_SNAP_FX_OFF` (0x0100, a spare bit: joint springs off) for the puppets. No change to the protocol version or other layouts.
