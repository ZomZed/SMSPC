// Multiplayer API shared by the network/session layer (w10) and the in-game remote
// player side (w15). Design, protocol and rules: port/MULTIPLAYER.md.
//
// Ownership:
//   w10  port_mp_net.cpp   UDP transport, sessions/lobbies, LAN discovery (own thread)
//        port_mp_menu.cpp  ImGui multiplayer menu, lobby screen, host panel
//        port_mp_game.cpp  game-side session glue: file-select entry, free-mode start,
//                          in-memory save, stage presence
//   w15  remote players: capture the local Mario into a PortMpSnapshot every game frame
//        (port_mp_set_local_snapshot), and draw/animate puppets from the received ones.
//
// Threading: every function here may be called from the game thread or the main
// (render/ImGui) thread; the network thread is internal. All calls are cheap (one mutex,
// a copy). Nothing here ever blocks on the network.
//
// Plain C ABI (no game headers, no windows.h) so game code under SMS_PORT can include it.
#ifndef PORT_MP_H
#define PORT_MP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PORT_MP_MAX_PLAYERS   16
#define PORT_MP_NAME_MAX      24   // bytes including the terminating NUL (UTF-8)
#define PORT_MP_LOBBY_NAME_MAX 32
#define PORT_MP_DEFAULT_PORT  7777
#define PORT_MP_DEFAULT_SERVER_PORT 7700 // port/server/sms_lobby_server.py
// Bump PORT_MP_PROTOCOL_VERSION whenever a packet layout, PortMpSnapshot or
// PortMpCharacter changes; peers with another version are refused at the handshake.
#define PORT_MP_PROTOCOL_VERSION 5 // 5: game modes None (0, default) / Free (1), start counter reserved[8] (w15); 4: host migration (CONNECT re-join slot/serial, PEERS, LOBBYKEY), world events, objectives extras; 2: w15 block v1.1 (held object, s8 emitter offset); 3: CONNECT password hash, LOBBY_INFO flags, PUNCH, PortMpCharacter colors[7]

// ---------------------------------------------------------------------------------
// Snapshot of one player's Mario (unreliable, 30 Hz). Written by w15's capture code,
// sent as is (little-endian x64 on both ends, packed, fixed size). Fields w15 does not
// fill stay 0. The network layer only reads `tick`, `stage`, `scenario` and `flags`.
// ---------------------------------------------------------------------------------
enum PortMpSnapFlags {
	PORT_MP_SNAP_VALID     = 0x0001, // the sender has a Mario in a stage (gameplay)
	PORT_MP_SNAP_HOLDING   = 0x0002, // carrying something (mHeldObject != null)
	PORT_MP_SNAP_ON_YOSHI  = 0x0004,
	PORT_MP_SNAP_SPRAYING  = 0x0008, // FLUDD emitting this frame
	PORT_MP_SNAP_HAS_FLUDD = 0x0010,
	PORT_MP_SNAP_HIDDEN    = 0x0020, // do not draw (pipe/warp, cutscene, demo camera)
	PORT_MP_SNAP_IN_WATER  = 0x0040,
	PORT_MP_SNAP_PAUSED    = 0x0080, // sender is in the pause menu / a talk
	PORT_MP_SNAP_FX_OFF    = 0x0100, // w15 puppets: the sender's joint springs (arm/cap/FLUDD time
	                                 // lag) are off: stage's first 0x4B0 ticks, or pumping
};

#pragma pack(push, 1)
typedef struct PortMpSnapshot {
	uint32_t tick;          // sender's game-frame counter (+1 per 30 Hz logic frame)
	uint8_t stage;          // TApplication::mCurrArea stage / scenario
	uint8_t scenario;
	uint16_t flags;         // PortMpSnapFlags
	float pos[3];           // TMario::mPosition
	float vel[3];           // TMario::mVel
	float forwardVel;       // TMario::mForwardVel
	int16_t faceAngle[3];   // TMario::mFaceAngle
	int16_t modelFaceAngle; // TMario::mModelFaceAngle
	uint32_t action;        // TMario::mStatus
	uint32_t actionArg;     // TMario::mStatusArg
	uint32_t marioFlags;    // TMario::mFlag
	uint16_t animId;        // TMario::mAnimationId (body animation)
	uint16_t animAux;       // w15: e.g. hand/face/cap animation or blend state
	float animFrame;        // current frame of animId
	float animRate;         // playback rate
	uint8_t nozzle;         // TWaterGun::mCurrentNozzle
	uint8_t secondNozzle;   // TWaterGun::mSecondNozzle
	uint16_t water;         // TWaterGun::mCurrentWater
	float sprayStrength;    // 0..1 trigger pressure / emit strength
	int16_t nozzleAngle[2]; // w15: FLUDD nozzle pitch/yaw if animated separately
	uint8_t health;         // TMario::mHealth
	uint8_t yoshiState;     // TYoshi::mState when on Yoshi
	uint8_t yoshiColor;     // TYoshi juice colour / type
	uint8_t pad0;
	uint8_t extra[44];      // reserved for w15 (send zeros if unused)
} PortMpSnapshot;           // 128 bytes

// Character customisation (stub for this milestone; carried in the handshake and
// PLAYER_INFO so it can be filled later without a protocol change of the table).
typedef struct PortMpCharacter {
	uint8_t preset;          // 0 = default Mario
	uint8_t colors[7][3];    // cap, shirt, overalls, gloves, shoes, skin, hair (RGB); 0 = default
	uint8_t reserved[10];    // [0] = 1: [1..3] is the player's name-tag colour (RGB), from the
	                         // Customize menu (older builds leave it 0 and ignore it);
	                         // [4] game behaviour + 1, [5] video 1 = 60 Hz, 2 = 50 Hz (0 = unknown)
	                         // (port_mp_net.cpp stamps them; mismatches are warned about)
} PortMpCharacter;           // 32 bytes
#pragma pack(pop)

// ---------------------------------------------------------------------------------
// Session / lobby state.
// ---------------------------------------------------------------------------------
enum PortMpGameMode {
	PORT_MP_MODE_NONE = 0, // Delfino Plaza only, every entrance closed (default; w15 port_mp_modes.cpp)
	PORT_MP_MODE_FREE = 1, // free roam in Delfino Plaza, every entrance open
	PORT_MP_MODE_HIDE = 2, // Hide and Seek in the locked plaza (w15: port_mp_hideseek.cpp)
	PORT_MP_MODE_FREEZE = 3, // Freeze Tag in the locked plaza (port_mp_freezetag.cpp)
	PORT_MP_MODE_PROP = 4, // Prop Hunt (port_mp_prophunt.cpp)
	PORT_MP_MODE_MANHUNT = 5, // Manhunt (port_mp_manhunt.cpp)
	PORT_MP_MODE_SHINERUN = 6, // Shine Runners (port_mp_shinerun.cpp)
	PORT_MP_MODE_CNR = 7, // Cops and Robbers (port_mp_cnr.cpp)
	PORT_MP_MODE_COUNT
};

typedef struct PortMpSessionSettings {
	uint8_t gameMode;          // PortMpGameMode
	uint8_t sharedObjectives;  // 0 = every Mario tracks his own shines/coins (default);
	                           // 1 = shines, blue/red/yellow coins shared (see MULTIPLAYER.md)
	uint8_t maxPlayers;        // 2..PORT_MP_MAX_PLAYERS
	uint8_t startStage;        // free mode: 1 (Delfino Plaza)
	uint8_t startScenario;     // free mode: plaza scenario with every entrance open
	// Taken: [0] interaction mode, [1] chat/emote bits, [2] enemy sync, [3] level select
	// (w11 presence), [4] FLUDD pushes players, [5] goop sync (w6), [6] world events (w10; 0 = on, the default),
	// [7] remote Shine banner (w8 objectives extras), [8] game-mode start counter (w15:
	// gameMode is the running mode; each host "Start" bumps [8]), [9] where Hide and Seek / Freeze
	// Tag runs (0 = the plaza, stage + 1 = one level, 0xFF = all levels), [10] its exit lock in
	// seconds (port_mp_arena.cpp). List new bytes here.
	uint8_t reserved[11];
} PortMpSessionSettings;       // 16 bytes

enum PortMpState {
	PORT_MP_OFF = 0,     // not in multiplayer
	PORT_MP_HOSTING,     // we are the host (session running)
	PORT_MP_CONNECTING,  // client: handshake in progress
	PORT_MP_CONNECTED,   // client: accepted, session running
	PORT_MP_FAILED,      // client: refused / timed out (see port_mp_status_text)
};

typedef struct PortMpPlayerInfo {
	uint8_t used;          // slot holds a player
	uint8_t isLocal;       // this is us
	uint8_t isHost;
	uint8_t hasSnapshot;   // at least one snapshot received
	uint32_t serial;       // changes when the slot is given to another player (despawn/respawn)
	char name[PORT_MP_NAME_MAX];
	uint8_t stage;         // latest known stage/scenario (0xFF = not in a stage)
	uint8_t scenario;
	uint16_t pingMs;       // round trip to the host (host: to that client)
	uint8_t lagging;       // no packets for 3 s: reconnecting (host: that client; client: the host)
	uint8_t pad1[3];
	PortMpCharacter character;
} PortMpPlayerInfo;

// A received snapshot with the local receive time (port_mp_time_ms clock).
typedef struct PortMpSnapshotRx {
	PortMpSnapshot snap;
	double rxMs;
} PortMpSnapshotRx;

// --- state ------------------------------------------------------------------------
int port_mp_enabled(void);             // feature flag ([multiplayer] enabled, default 1)
int port_mp_state(void);               // PortMpState
int port_mp_active(void);              // HOSTING or CONNECTED
int port_mp_is_host(void);
int port_mp_local_slot(void);          // -1 when not active
const char* port_mp_status_text(void); // last status line for the UI (static buffer)
double port_mp_time_ms(void);          // monotonic clock used for rxMs
void port_mp_get_settings(PortMpSessionSettings* out);
// Bumped at every stage load in a session (port_mp_game.cpp). A sync module starts a new visit
// when it changes: a new TMarDirector can sit at the old one's address.
extern unsigned gPortMpStageSerial;
// The Delfino Plaza scenario the running game mode uses (port_mp_modes.cpp).
int port_mp_plaza_scenario(const PortMpSessionSettings* s);

// --- players (w15 polls these every frame) ------------------------------------------
// Fills `out` and returns 1 when the slot holds a player (including the local one).
int port_mp_get_player(int slot, PortMpPlayerInfo* out);
// Up to `max` most recent snapshots of a remote slot, newest first; returns the count.
// Snapshots older than the newest by tick are dropped on receive, so ticks decrease.
int port_mp_get_snapshots(int slot, PortMpSnapshotRx* out, int max);
// 1 when that player's latest snapshot is in our current stage and scenario (and valid).
int port_mp_same_stage(int slot);

// --- local player (w15) ---------------------------------------------------------------
// Call once per game logic frame from the game thread while port_mp_active(). The
// network thread sends the latest one at 30 Hz.
void port_mp_set_local_snapshot(const PortMpSnapshot* snap);

// --- session control (menu / debug knobs) ------------------------------------------
int port_mp_host(const char* lobbyName, int port, const PortMpSessionSettings* settings);
int port_mp_join(const char* host, int port);
void port_mp_leave(void);
void port_mp_set_name(const char* name);       // also sent to the session if active
const char* port_mp_get_name(void);
void port_mp_set_character(const PortMpCharacter* c);
// Host only: change the session settings (game mode, shared objectives); synced to all.
void port_mp_host_set_settings(const PortMpSessionSettings* s);

// --- connection type: lobby server (join by lobby name) or direct IP -----------------------
enum PortMpConnType { PORT_MP_CONN_SERVER = 0, PORT_MP_CONN_DIRECT = 1 };
// Host: `connType` SERVER registers `lobbyName` on the lobby server (unique, case-insensitive;
// "name taken" fails the call asynchronously: watch port_mp_state / status text) and also
// answers LAN lookups by name; DIRECT is port_mp_host. `password` may be null/empty.
int port_mp_host2(const char* lobbyName, int port, const PortMpSessionSettings* settings, int connType,
                  const char* password);
// Client: join the lobby called `name` through the lobby server (hole punching, relay fallback)
// and on the LAN at the same time (whichever answers first).
int port_mp_join_name(const char* name, const char* password);
int port_mp_conn_type(void);   // of the current/last session
int port_mp_relayed(void);     // client: 1 when the host is reached through the server's relay

enum PortMpServerState { PORT_MP_SERVER_UNKNOWN = 0, PORT_MP_SERVER_CHECKING, PORT_MP_SERVER_ONLINE,
	                     PORT_MP_SERVER_OFFLINE, PORT_MP_SERVER_UNSET };
typedef struct PortMpServerStatus {
	int state;               // PortMpServerState
	int lobbies;             // open lobbies on the server (when online)
	int relay;               // the server offers relaying
	char address[128];       // configured "host[:port]"
	char publicAddr[48];     // our public endpoint as the server sees it ("" = unknown)
} PortMpServerStatus;
void port_mp_server_status(PortMpServerStatus* out);
void port_mp_server_refresh(void); // ping it now (the menu calls this every few seconds)
void port_mp_set_server(const char* hostPort);

// This machine's IPv4 addresses for "join me at" (LAN / virtual LAN), loopback and
// link-local skipped. kind: 0 LAN, 1 virtual LAN (Tailscale/ZeroTier/Radmin/Hamachi).
typedef struct PortMpLocalAddr {
	char ip[16];
	char adapter[64];
	int kind;
} PortMpLocalAddr;
int port_mp_local_addresses(PortMpLocalAddr* out, int max);
int port_mp_host_port(void); // the UDP port we host on (0 when not hosting)

// LAN lobby list (filled by port_mp_lan_refresh's broadcast replies).
typedef struct PortMpLobbyInfo {
	char name[PORT_MP_LOBBY_NAME_MAX];
	char hostName[PORT_MP_NAME_MAX];
	char address[48];      // "a.b.c.d"
	uint16_t port;
	uint8_t players;
	uint8_t maxPlayers;
	uint8_t gameMode;
	uint8_t protocolOk;    // same protocol version as ours
	uint8_t hasPassword;
	uint8_t connType;      // PortMpConnType the host chose
	uint16_t pingMs;
	double seenMs;
} PortMpLobbyInfo;
void port_mp_lan_refresh(void);
int port_mp_lan_lobbies(PortMpLobbyInfo* out, int max);
// The lobby server's public list (server mode; refreshed with the status ping while the menu
// asks, port_mp_server_refresh). address/port are empty: join with port_mp_join_name.
int port_mp_server_lobbies(PortMpLobbyInfo* out, int max);

// --- app messages: the extension point for multiplayer features ----------------------------
// Each feature (chat, interaction, teleport, object sync, ...) owns a channel and its own
// payload format, so it lives in its own file and never edits port_mp_net.cpp. The host
// relays: `to` = -1 sends to every other player, otherwise to one slot. reliable = 1:
// ordered, resent until acked (per sender); 0: unreliable datagram. Payload <= 1000 bytes.
// Messages are queued per channel (256 deep, oldest dropped) until port_mp_recv takes them;
// queues are emptied when the session ends. Thread-safe, never blocks.
#define PORT_MP_APP_MAX 1000
enum PortMpChannel {
	PORT_MP_CH_CHAT     = 1,  // text chat (task board: chat)
	PORT_MP_CH_EMOTE    = 2,  // emotes
	PORT_MP_CH_INTERACT = 3,  // player interaction (bump, stomp, knockback)
	PORT_MP_CH_WARP     = 4,  // stage presence: warp-to-player requests/answers
	PORT_MP_CH_OBJECTS  = 5,  // enemy/object sync
	PORT_MP_CH_CUSTOM   = 6,  // character customisation extras beyond PortMpCharacter
	PORT_MP_CH_TEST     = 7,  // automated tests
	PORT_MP_CH_GOOP     = 16, // goop cleaning sync (P8, w6: port_mp_goop.cpp)
	PORT_MP_CH_NPCS     = 17, // NPC sync (P8 NPC sub-package, w12: port_mp_npcs.cpp)
	PORT_MP_CH_EVENTS   = 18, // world events (P8 EVENTS, w10: port_mp_events.cpp)
	PORT_MP_CH_OBJX     = 19, // shared-objective extras (P1b, w8: port_mp_objectives_extra.cpp)
	PORT_MP_CH_WATER    = 20, // remote FLUDD water emits (w15: port_mp_water.cpp, "Puppet block")
	PORT_MP_CH_AMBIENT  = 22, // fruit and animals (w15: port_mp_ambient.cpp); 21 kept for w11
	PORT_MP_CH_MAPREACT = 21, // water hits on map objects + linked coins (w11: port_mp_mapreact.cpp)
	PORT_MP_CH_MODECTL  = 23, // game mode countdown / queue / votes / AutoStart (port_mp_modes.cpp)
	PORT_MP_CH_HIDE     = 24, // Hide and Seek round state and tags (w15: port_mp_hideseek.cpp)
	PORT_MP_CH_FREEZE   = 25, // Freeze Tag round state, freezes and thaws (port_mp_freezetag.cpp)
	PORT_MP_CH_PROP     = 26, // Prop Hunt round state, disguises, sprays (port_mp_prophunt.cpp)
	PORT_MP_CH_MANHUNT  = 27, // Manhunt round state, Shines, hits (port_mp_manhunt.cpp)
	PORT_MP_CH_SHINERUN = 28, // Shine Runners round state, Shines, drops (port_mp_shinerun.cpp)
	PORT_MP_CH_CNR      = 29, // Cops and Robbers round state, loot, arrests (port_mp_cnr.cpp)
	PORT_MP_CH_FIRST_FREE = 30 // new features: take the next number and list it here
};
int port_mp_send(uint8_t channel, const void* data, int len, int reliable, int to);
// Returns the byte count copied into buf (-1 for an empty message, 0 = nothing queued) and
// the sender's slot.
int port_mp_recv(uint8_t channel, void* buf, int cap, int* fromSlot);

// --- feature hooks: register from a static constructor in the feature's own file -------------
// (no edits to w10's files needed). Up to 16 of each; called only while port_mp_active().
//   game frame: game thread, once per logic frame while a stage runs (after TMarDirector::direct)
//   overlay:    main thread inside the ImGui frame, every presented frame (HUD-like drawing)
//   lobby:      main thread, inside the lobby panel window (host options, extra info); `isHost`
typedef void (*PortMpFrameFn)(void);
typedef void (*PortMpLobbyFn)(int isHost);
void port_mp_register_game_frame(PortMpFrameFn fn);
void port_mp_register_overlay(PortMpFrameFn fn);
void port_mp_register_lobby_section(const char* title, PortMpLobbyFn fn);
// Feature menus may need the game's pads idle while they take input (e.g. a chat box):
void port_mp_request_input(int on); // reference-counted per call site: pass 1 / 0 in pairs

// --- shared objectives (stage 4; port_mp_game.cpp <-> port_mp_net.cpp) -----------------
// Objective id: kind (3 bits) | stage (8) | scenario (8) | index (13).
// SHINE/BLUE are persistent for the session (scenario 0xFF; index = shine id / blue coin
// event id, stage = SMS_getShineStage for blue coins, 0 for shines); COIN/RED are per stage
// visit (index = position in gpItemManager's object list, scene-placed objects only).
enum PortMpObjKind { PORT_MP_OBJ_COIN = 1, PORT_MP_OBJ_RED = 2, PORT_MP_OBJ_BLUE = 3, PORT_MP_OBJ_SHINE = 4 };
#define PORT_MP_OBJ_ID(kind, stage, scenario, index)                                            \
	(((uint32_t)(kind) << 29) | ((uint32_t)((stage)&0xFF) << 21) | ((uint32_t)((scenario)&0xFF) << 13) \
	 | ((uint32_t)(index)&0x1FFF))
void port_mp_objective_send(uint32_t id);
int port_mp_objective_poll(uint32_t* ids, uint8_t* slots, int max);

// --- game glue (port_mp_game.cpp; called by tiny SMS_PORT hooks in game code) -----------
// The current stage/scenario as the game knows it (Application.cpp hook).
void port_mp_note_area(int appState, int stage, int scenario);
// While this returns 1, TCardManager's CARD calls go to an in-memory card
// (port_mp_savecard.cpp): the multiplayer save never touches the player's memory card.
int port_mp_block_card_writes(void);

// Multiplayer menu (port_mp_menu.cpp, ImGui). Drawn from port_window.cpp each frame.
void port_mp_menu_draw(void);
int port_mp_menu_wants_input(void);  // the game gets idle pads while this is 1
void port_mp_menu_open(int page);    // 0 = main multiplayer menu, 1 = host/lobby panel
void port_mp_menu_close(void);       // close whatever page is open (any thread; e.g. after a warp button)

#ifdef __cplusplus
}
#endif

#endif // PORT_MP_H
