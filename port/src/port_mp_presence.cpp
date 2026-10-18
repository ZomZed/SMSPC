// Multiplayer stage presence and navigation (w11, task board P5; see port/MULTIPLAYER.md):
//  - where everyone is (stage + episode, or menu / movie / file select / paused / cutscene),
//    as a section of the lobby panel;
//  - "Warp to player": load the stage + scenario that player is in, then ask them (reliable
//    message on PORT_MP_CH_WARP) for a spawn position near them; without an answer Mario
//    keeps the stage's normal start;
//  - a free-mode level select (every stage, episode, boss and secret course) that the host
//    can switch off for the guests (PortMpSessionSettings.reserved[3]);
//  - "Return to Delfino Plaza" (the session's plaza layout);
//  - optional arrows at the screen edge pointing at players in the same stage who are
//    off-screen.
// Stage changes use the game's own path: TApplication::mNextArea + the director's
// stage-transition flag (what TMarDirector::setNextStage sets), so TMarDirector::moveStage
// fades out and loads as for a pipe or the pause menu's exit. Everything here only runs
// while a session is active (the port_mp_register_* hooks); single-player is untouched.
#ifdef SMS_PORT_AURORA

#include "port_mp.h"
#include "port_settings.h" // port_get_display_aspect (w6)

#include <Camera/Camera.hpp>
#include <Player/Mario.hpp>
#include <Player/MarioAccess.hpp>
#include <Player/MarioFlags.hpp>
#include <Player/MarioStatus.hpp>
#include <System/Application.hpp>
#include <System/GameSequence.hpp>
#include <System/MarDirector.hpp>

extern "C" uint32_t port_mp_tag_rgb(int slot); // port_mp_palette.cpp
#include "port_mp_ui.h" // the game font (mpui_text); includes imgui

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

extern "C" unsigned port_frame_count(void);       // port_crash.cpp
extern "C" void port_mp_toast(const char* text);  // port_mp_menu.cpp
extern "C" int port_mp_arena_plaza_refused(void); // port_mp_arena.cpp (Hide and Seek / Freeze Tag)
extern "C" int port_mp_arena_warp_refused(void);
extern "C" int port_mp_hide_hide_tag(int slot); // port_mp_hideseek.cpp
extern "C" int port_mp_prop_hide_tag(int slot); // port_mp_prophunt.cpp

namespace {

// PortMpSessionSettings.reserved[] byte owned by P5 (MULTIPLAYER.md): 0 = level select
// allowed for everyone (default), 1 = host only.
const int kResLevelSelect = 3;

// ---- stages --------------------------------------------------------------------------
struct StageDef {
	u8 stage;
	u8 episodes; // scenarios offered (index = TApplication scenario)
	const char* name;
};
// Main levels and their extra areas (scenario counts from data/stageArc.bin).
const StageDef kLevels[] = {
	{ 0, 2, "Delfino Airstrip" },     { 2, 8, "Bianco Hills" },      { 3, 8, "Ricco Harbor" },
	{ 4, 8, "Gelato Beach" },         { 5, 8, "Pinna Park" },        { 13, 8, "Pinna Park (inside the park)" },
	{ 6, 8, "Sirena Beach" },         { 7, 5, "Hotel Delfino" },     { 14, 2, "Hotel Delfino casino" },
	{ 8, 8, "Pianta Village" },       { 9, 8, "Noki Bay" },          { 16, 1, "Noki Bay (undersea)" },
};
const StageDef kBosses[] = {
	{ 55, 1, "Petey Piranha (biancoBoss)" }, { 56, 1, "Boss stage (delfinoBoss)" },
	{ 57, 1, "Boss stage (mareBoss)" },      { 58, 2, "Mecha-Bowser (pinnaBoss)" },
	{ 59, 1, "Boss stage (ricco8)" },        { 60, 1, "Bowser (coronaBoss)" },
};
// Secret ("EX") courses: one scenario each, stage ids 20..52, named by their archives.
const char* const kExNames[] = {
	"dolpic_ex0", "dolpic_ex1", "dolpic_ex2", "dolpic_ex3", "dolpic_ex4", "dolpic_ex5", "dolpic_ex6",
	"dolpic_ex7", "bia_ex0",    "bia_ex1",    "rico_ex0",   "rico_ex1",   "mam_ex0",    "mam_ex1",
	"pinna_ex0",  "pinna_ex1",  "pinna_ex2",  "pinna_ex3",  "pinna_ex4",  "pinna_ex5",  "sirena_ex0",
	"sirena_ex1", "monte_ex0",  "monte_ex1",  "mare_ex0",   "mare_ex1",   "coro_ex0",   "coro_ex1",
	"coro_ex2",   "coro_ex3",   "coro_ex4",   "coro_ex5",   "coro_ex6",
};
const int kExFirst = 20;
const int kExCount = (int)(sizeof(kExNames) / sizeof(kExNames[0]));

const StageDef* findStage(int stage)
{
	for (const StageDef& d : kLevels)
		if (d.stage == stage)
			return &d;
	for (const StageDef& d : kBosses)
		if (d.stage == stage)
			return &d;
	return nullptr;
}

// "Bianco Hills, episode 3", "Delfino Plaza", "Secret course coro_ex2", ...
void stageLabel(int stage, int scenario, char* out, size_t cap)
{
	if (stage == 1) {
		std::snprintf(out, cap, "Delfino Plaza");
	} else if (stage == 15) {
		std::snprintf(out, cap, "File select");
	} else if (stage == 52) { // coro_ex6: the plaza's Corona Mountain entrance (w10, BOOTLOG MP9)
		std::snprintf(out, cap, "Corona Mountain");
	} else if (stage >= kExFirst && stage < kExFirst + kExCount) {
		std::snprintf(out, cap, "Secret course %s", kExNames[stage - kExFirst]);
	} else if (const StageDef* d = findStage(stage)) {
		if (d->episodes > 1)
			std::snprintf(out, cap, "%s, episode %d", d->name, scenario + 1);
		else
			std::snprintf(out, cap, "%s", d->name);
	} else {
		std::snprintf(out, cap, "Stage %d/%d", stage, scenario);
	}
}

bool warpableStage(int stage) { return stage != 0xFF && stage != 15 && stage < 0x40; }

// ---- wire (PORT_MP_CH_WARP; both ends little-endian x86) ----------------------------------
enum MsgType : u8 { MSG_PRESENCE = 1, MSG_SPAWN_REQ = 2, MSG_SPAWN_POS = 3 };
enum Activity : u8 { ACT_PLAY = 0, ACT_PAUSED = 1, ACT_TALK = 2, ACT_CUTSCENE = 3, ACT_LOADING = 4 };

#pragma pack(push, 1)
struct MsgPresence {
	u8 type; // MSG_PRESENCE
	u8 appState; // TApplication::APP_STATE_*
	u8 stage, scenario;
	u8 activity; // Activity (gameplay only)
	u8 pad[3];
};
struct MsgSpawnReq {
	u8 type; // MSG_SPAWN_REQ
	u8 stage, scenario, pad;
	u32 nonce;
};
struct MsgSpawnPos {
	u8 type; // MSG_SPAWN_POS
	u8 stage, scenario;
	u8 valid; // 1: pos/yaw are a place the sender stood on recently in that stage
	u32 nonce;
	float pos[3];
	float yawDeg;
};
#pragma pack(pop)

std::mutex sMu; // everything below that both threads touch

// Remote presence (main thread writes on receive; UI reads).
struct RemotePresence {
	bool have;
	u32 serial;
	MsgPresence msg;
	double at;
};
RemotePresence sRemote[PORT_MP_MAX_PLAYERS];

// Local game state published by the game thread for the main thread.
struct LocalGame {
	double at;    // port_mp_time_ms of the last game frame
	u8 activity;  // Activity
	bool safeValid;
	u8 safeStage, safeScenario;
	float safePos[3];
	float safeYaw;
};
LocalGame sLocal;

// Requests from the UI / test knob for the game thread.
enum ReqKind { REQ_NONE, REQ_GOTO, REQ_PLAZA, REQ_WARP_TO };
struct Request {
	ReqKind kind;
	int stage, scenario;
	int slot;
	u32 serial;
};
Request sReq;

// Spawn-near-player state (game thread; the answer comes in on the main thread).
struct Spawn {
	bool active;
	int slot;
	u32 serial;
	int stage, scenario;
	double startedAt;
	bool sent;
	double sentAt;
	u32 nonce;
	bool answered;
	MsgSpawnPos answer;
};
Spawn sSpawn;

// Off-screen markers (game thread computes, overlay draws).
struct Marker {
	float x, y;  // 0..1 in the game picture
	float dirX, dirY;
	float distM;
	char name[PORT_MP_NAME_MAX];
	uint32_t rgb; // the player's name-tag colour
};
Marker sMarkers[PORT_MP_MAX_PLAYERS];
int sMarkerCount;
double sMarkersAt;

std::atomic<bool> sShowMarkers{ true };

// ---- helpers ---------------------------------------------------------------------------
// The game thread's last published activity (sMu held). Game frames can stall for a few
// seconds (loads, a busy GPU), so only a long gap counts as loading.
u8 localActivityLocked() { return port_mp_time_ms() - sLocal.at < 5000.0 ? sLocal.activity : (u8)ACT_LOADING; }

void toast(const char* text)
{
	std::fprintf(stderr, "[mp-presence] %s\n", text);
	port_mp_toast(text);
}

bool levelSelectAllowed()
{
	PortMpSessionSettings s;
	port_mp_get_settings(&s);
	if (s.gameMode != PORT_MP_MODE_FREE)
		return false; // w15: None and Hide and Seek keep everyone in the plaza
	if (port_mp_is_host())
		return true;
	return s.reserved[kResLevelSelect] == 0;
}

void plazaArea(int* stage, int* scenario)
{
	PortMpSessionSettings s;
	port_mp_get_settings(&s);
	*stage    = s.startStage ? s.startStage : 1;
	*scenario = (u8)port_mp_plaza_scenario(&s);
}

void queueRequest(ReqKind kind, int stage, int scenario, int slot)
{
	std::lock_guard<std::mutex> lk(sMu);
	sReq.kind     = kind;
	sReq.stage    = stage;
	sReq.scenario = scenario;
	sReq.slot     = slot;
	sReq.serial   = 0;
	PortMpPlayerInfo p;
	if (slot >= 0 && port_mp_get_player(slot, &p))
		sReq.serial = p.serial;
}

// Describes where a player is, for the panel.
void describe(int slot, const PortMpPlayerInfo& p, char* out, size_t cap)
{
	MsgPresence m;
	bool have = false;
	if (p.isLocal) {
		TApplication* app = SMSGetApplication();
		m.appState        = app->mAppState;
		m.stage           = app->mCurrArea.getStage();
		m.scenario        = app->mCurrArea.getScenario();
		std::lock_guard<std::mutex> lk(sMu);
		m.activity = localActivityLocked();
		have       = true;
	} else {
		std::lock_guard<std::mutex> lk(sMu);
		const RemotePresence& r = sRemote[slot];
		if (r.have && r.serial == p.serial) {
			m    = r.msg;
			have = true;
		}
	}
	if (!have) { // a peer without this feature: the roster's stage only
		if (p.stage == 0xFF)
			std::snprintf(out, cap, "Loading / in a menu");
		else
			stageLabel(p.stage, p.scenario, out, cap);
		return;
	}
	switch (m.appState) {
	case TApplication::APP_STATE_GAMEPLAY: {
		if (m.stage == 15) {
			std::snprintf(out, cap, "File select");
			return;
		}
		char where[96];
		stageLabel(m.stage, m.scenario, where, sizeof(where));
		static const char* const kAct[] = { "", " (paused)", " (talking)", " (cutscene)", " (loading)" };
		std::snprintf(out, cap, "%s%s", where, m.activity < 5 ? kAct[m.activity] : "");
		return;
	}
	case TApplication::APP_STATE_MOVIE: std::snprintf(out, cap, "Watching a movie"); return;
	case TApplication::APP_STATE_TITLE: std::snprintf(out, cap, "Choosing an episode"); return; // TSelectDir
	case TApplication::APP_STATE_MENU: std::snprintf(out, cap, "In the menus"); return;
	default: std::snprintf(out, cap, "Loading"); return;
	}
}

// ---- game thread -------------------------------------------------------------------------
bool directorIdle()
{
	TMarDirector* d = gpMarDirector;
	return d != nullptr
	       && (d->mFlags
	           & (TMarDirector::DIRECTOR_FLAG_SHINE_GET_PENDING | TMarDirector::DIRECTOR_FLAG_STAGE_TRANSITION_PENDING
	              | TMarDirector::DIRECTOR_FLAG_ACTOR_DEMO_STAGE_TRANSITION_PENDING
	              | TMarDirector::DIRECTOR_FLAG_GATE_DEMO_STAGE_TRANSITION_PENDING
	              | TMarDirector::DIRECTOR_FLAG_GAME_OVER_PENDING))
	              == 0;
}

// The game's own stage change (TMarDirector::setNextStage minus its plaza gate demos, which
// expect Mario at that gate): next area + the transition flag; moveStage does the rest.
bool startStage(int stage, int scenario)
{
	if (!directorIdle() || SMSGetApplication()->mCurrArea.getStage() == 15)
		return false;
	SMSGetApplication()->setNextArea(TGameSequence((u8)stage, (u8)scenario));
	gpMarDirector->onFlag(TMarDirector::DIRECTOR_FLAG_STAGE_TRANSITION_PENDING);
	std::fprintf(stderr, "[mp-presence] loading stage %d scenario %d\n", stage, scenario);
	return true;
}

u8 currentActivity()
{
	TMarDirector* d = gpMarDirector;
	if (d == nullptr || !directorIdle()) {
		return ACT_LOADING;
	}
	if (d->mState == TMarDirector::STATE_PAUSE_MENU)
		return ACT_PAUSED;
	if (d->isTalkModeNow())
		return ACT_TALK;
	if (d->isDemoModeNow())
		return ACT_CUTSCENE;
	return ACT_PLAY;
}

// Where Mario last stood on solid ground (not jumping, swimming, held, on Yoshi).
void trackSafePosition(int stage, int scenario, bool newVisit)
{
	TMario* m = gpMarioOriginal;
	std::lock_guard<std::mutex> lk(sMu);
	if (newVisit)
		sLocal.safeValid = false;
	if (m == nullptr)
		return;
	const bool grounded = !m->checkStatusType(MARIO_STATUS_FLAG_JUMPING) && !m->checkStatusType(MARIO_STATUS_FLAG_SWIMMING)
	                      && !m->checkFlag(MARIO_FLAG_IN_ANY_WATER) && m->isTouchGround4cm() && m->mHolder == nullptr
	                      && !m->onYoshi();
	if (!grounded)
		return;
	sLocal.safeValid    = true;
	sLocal.safeStage    = (u8)stage;
	sLocal.safeScenario = (u8)scenario;
	sLocal.safePos[0]   = m->mPosition.x;
	sLocal.safePos[1]   = m->mPosition.y;
	sLocal.safePos[2]   = m->mPosition.z;
	sLocal.safeYaw      = m->mFaceAngle.y * (360.0f / 65536.0f);
}

void handleRequest(int stage, int scenario)
{
	Request r;
	{
		std::lock_guard<std::mutex> lk(sMu);
		r = sReq;
		if (r.kind == REQ_NONE)
			return;
	}
	// Keep it until Mario is in normal play (or paused: the director takes the change when
	// the pause menu closes), not in a talk, cutscene, shine demo or another transition.
	const u8 act = currentActivity();
	if (act != ACT_PLAY && act != ACT_PAUSED) {
		static double sToldAt;
		if ((act == ACT_TALK || act == ACT_CUTSCENE) && port_mp_time_ms() - sToldAt > 10000.0) {
			sToldAt = port_mp_time_ms();
			toast(act == ACT_TALK ? "The warp starts when the talk ends" : "The warp starts when the cutscene ends");
		}
		return;
	}
	bool done = true;
	char msg[160], where[96];
	if ((r.kind == REQ_PLAZA && port_mp_arena_plaza_refused())
	    || (r.kind == REQ_WARP_TO && port_mp_arena_warp_refused())) {
		r.kind = REQ_NONE; // Hide and Seek / Freeze Tag (port_mp_arena.cpp told the player why)
	} else if (r.kind == REQ_PLAZA) {
		plazaArea(&r.stage, &r.scenario);
		r.kind = REQ_GOTO;
	} else if (r.kind == REQ_GOTO && !levelSelectAllowed()) {
		toast("The host switched the level select off");
		r.kind = REQ_NONE;
	}
	if (r.kind == REQ_GOTO) {
		if (startStage(r.stage, r.scenario)) {
			stageLabel(r.stage, r.scenario, where, sizeof(where));
			std::snprintf(msg, sizeof(msg), "Going to %s", where);
			toast(msg);
		} else {
			done = false;
		}
	} else if (r.kind == REQ_WARP_TO) {
		PortMpPlayerInfo p;
		if (!port_mp_get_player(r.slot, &p) || p.serial != r.serial) {
			toast("Warp: that player left");
		} else if (!warpableStage(p.stage)) {
			std::snprintf(msg, sizeof(msg), "Warp: %s is not in a stage right now", p.name);
			toast(msg);
		} else if (p.stage != stage || p.scenario != scenario) {
			done = startStage(p.stage, p.scenario);
			if (done) {
				stageLabel(p.stage, p.scenario, where, sizeof(where));
				std::snprintf(msg, sizeof(msg), "Warping to %s (%s)", p.name, where);
				toast(msg);
			}
		} else {
			std::snprintf(msg, sizeof(msg), "Warping to %s", p.name);
			toast(msg);
		}
		if (done && port_mp_get_player(r.slot, &p) && p.serial == r.serial && warpableStage(p.stage)) {
			std::lock_guard<std::mutex> lk(sMu);
			sSpawn           = Spawn();
			sSpawn.active    = true;
			sSpawn.slot      = r.slot;
			sSpawn.serial    = p.serial;
			sSpawn.stage     = p.stage;
			sSpawn.scenario  = p.scenario;
			sSpawn.startedAt = port_mp_time_ms();
		}
	}
	if (done) {
		std::lock_guard<std::mutex> lk(sMu);
		sReq.kind = REQ_NONE;
	}
}

void spawnFrame(int stage, int scenario, int framesInVisit)
{
	Spawn s;
	{
		std::lock_guard<std::mutex> lk(sMu);
		s = sSpawn;
	}
	if (!s.active)
		return;
	const double now = port_mp_time_ms();
	if (now - s.startedAt > 90000.0) { // load + the stage's intro camera / talks
		toast("Warp: timed out, staying at the stage's start");
		std::lock_guard<std::mutex> lk(sMu);
		sSpawn.active = false;
		return;
	}
	if (stage != s.stage || scenario != s.scenario)
		return; // still loading
	// Wait until Mario has control: after the stage's intro camera / banners and his own
	// entry (the warp-in from the plaza keeps him hidden until it ends; teleporting during
	// it would leave him invisible), standing on the ground.
	TMarDirector* d = gpMarDirector;
	TMario* m       = gpMarioOriginal;
	if (m == nullptr || framesInVisit < 30 || currentActivity() != ACT_PLAY || d == nullptr)
		return;
	if (!m->checkUnk114(TMario::UNK114_FLAG_VISIBLE) || m->checkFlag(MARIO_FLAG_UNK4)
	    || m->checkStatusType(MARIO_STATUS_FLAG_JUMPING)
	    || !(m->isTouchGround4cm() || m->checkStatusType(MARIO_STATUS_FLAG_SWIMMING)))
		return;
	if (!s.sent) {
		MsgSpawnReq q;
		std::memset(&q, 0, sizeof(q));
		q.type     = MSG_SPAWN_REQ;
		q.stage    = (u8)stage;
		q.scenario = (u8)scenario;
		q.nonce    = (u32)(now * 7.0) ^ 0x5A17u;
		port_mp_send(PORT_MP_CH_WARP, &q, sizeof(q), 1, s.slot);
		std::lock_guard<std::mutex> lk(sMu);
		sSpawn.sent   = true;
		sSpawn.sentAt = now;
		sSpawn.nonce  = q.nonce;
		std::fprintf(stderr, "[mp-presence] asking slot %d for a spawn position\n", s.slot);
		return;
	}
	if (s.answered) {
		const MsgSpawnPos& a = s.answer;
		if (a.valid && a.stage == stage && a.scenario == scenario) {
			JGeometry::TVec3<f32> pos(a.pos[0], a.pos[1], a.pos[2]);
			gpMarioOriginal->warpRequest(pos, a.yawDeg);
			std::fprintf(stderr, "[mp-presence] spawned near slot %d at %.0f %.0f %.0f\n", s.slot, a.pos[0], a.pos[1],
			             a.pos[2]);
		} else {
			toast("Warp: no spawn position from that player, staying at the stage's start");
		}
		std::lock_guard<std::mutex> lk(sMu);
		sSpawn.active = false;
		return;
	}
	if (now - s.sentAt > 5000.0) {
		toast("Warp: no answer from that player, staying at the stage's start");
		std::lock_guard<std::mutex> lk(sMu);
		sSpawn.active = false;
	}
}

// Off-screen markers: players in our stage+scenario, projected with the game camera.
void markersFrame()
{
	int n = 0, seen = 0;
	Marker out[PORT_MP_MAX_PLAYERS];
	CPolarSubCamera* cam = gpCamera;
	if (sShowMarkers.load() && cam != nullptr && cam->mFovy > 1.0f && cam->mAspect > 0.1f) {
		const JGeometry::TVec3<f32> eye = cam->mPosition, at = cam->mTarget, up = cam->mUp;
		float f[3] = { at.x - eye.x, at.y - eye.y, at.z - eye.z };
		float fl   = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
		if (fl > 1e-3f) {
			for (float& v : f)
				v /= fl;
			float r[3] = { f[1] * up.z - f[2] * up.y, f[2] * up.x - f[0] * up.z, f[0] * up.y - f[1] * up.x };
			float rl   = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
			if (rl > 1e-3f) {
				for (float& v : r)
					v /= rl;
				const float u[3] = { r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0] };
				const float th   = std::tan(cam->mFovy * 0.5f * 3.14159265f / 180.0f);
				const int me     = port_mp_local_slot();
				for (int i = 0; i < PORT_MP_MAX_PLAYERS && n < PORT_MP_MAX_PLAYERS; i++) {
					if (i == me || !port_mp_same_stage(i) || port_mp_hide_hide_tag(i) || port_mp_prop_hide_tag(i))
						continue; // (hiders stay hidden from seekers / hunters)
					PortMpSnapshotRx rx;
					PortMpPlayerInfo p;
					if (port_mp_get_snapshots(i, &rx, 1) < 1 || !port_mp_get_player(i, &p))
						continue;
					if ((rx.snap.flags & PORT_MP_SNAP_HIDDEN) || port_mp_time_ms() - rx.rxMs > 3000.0)
						continue;
					seen++;
					const float d[3] = { rx.snap.pos[0] - eye.x, rx.snap.pos[1] + 80.0f - eye.y, rx.snap.pos[2] - eye.z };
					const float xv = d[0] * r[0] + d[1] * r[1] + d[2] * r[2];
					const float yv = d[0] * u[0] + d[1] * u[1] + d[2] * u[2];
					const float zv = d[0] * f[0] + d[1] * f[1] + d[2] * f[2];
					float dx, dy;
					if (zv > 10.0f) {
						dx = xv / (zv * th * cam->mAspect);
						dy = yv / (zv * th);
						if (std::fabs(dx) <= 1.0f && std::fabs(dy) <= 1.0f)
							continue; // on screen: the name tag shows where they are
					} else {
						dx = xv, dy = yv; // behind the camera: point to that side
						if (std::fabs(dx) + std::fabs(dy) < 1e-3f)
							dy = -1.0f;
					}
					const float dl = std::sqrt(dx * dx + dy * dy);
					Marker& mk     = out[n++];
					mk.dirX        = dx / dl;
					mk.dirY        = -dy / dl; // screen y grows downwards
					// on the border of an inset box around the centre
					const float k = 0.44f / (std::fabs(mk.dirX) > std::fabs(mk.dirY) ? std::fabs(mk.dirX) : std::fabs(mk.dirY));
					mk.x          = 0.5f + mk.dirX * k;
					mk.y          = 0.5f + mk.dirY * k;
					mk.distM      = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) / 100.0f;
					std::snprintf(mk.name, sizeof(mk.name), "%s", p.name);
					mk.rgb = port_mp_tag_rgb(i);
				}
			}
		}
	}
	// SMS_PORT_MP_PRESENCE_LOG=1 (debug): the markers every 5 s.
	static const bool sLog = std::getenv("SMS_PORT_MP_PRESENCE_LOG") != nullptr;
	static double sLogAt;
	if (sLog && port_mp_time_ms() - sLogAt > 5000.0) {
		sLogAt = port_mp_time_ms();
		std::fprintf(stderr, "[mp-presence] players in this stage: %d, off-screen markers: %d\n", seen, n);
		for (int i = 0; i < n; i++)
			std::fprintf(stderr, "[mp-presence]   %s at %.2f,%.2f (%.0f m)\n", out[i].name, out[i].x, out[i].y, out[i].distM);
	}
	std::lock_guard<std::mutex> lk(sMu);
	std::memcpy(sMarkers, out, sizeof(Marker) * n);
	sMarkerCount = n;
	sMarkersAt   = port_mp_time_ms();
}

// SMS_PORT_MP_PRESENCE_TEST="<name>:<action>@<frame>;..." (debug, unattended tests with
// mp_test.py, which gives each instance its name): action = goto=<stage>,<scenario> |
// warp=<player name> | plaza | lsoff / lson (host: level select off/on for the guests) |
// panel (opens the lobby panel, for screenshots).
// Each entry runs once, at that presented frame.
void testKnob()
{
	static const char* sTest = std::getenv("SMS_PORT_MP_PRESENCE_TEST");
	static unsigned sDone; // bit per entry
	if (sTest == nullptr || *sTest == '\0')
		return;
	const char* me = port_mp_get_name();
	int idx        = 0;
	for (const char* p = sTest; *p != '\0' && idx < 32; idx++) {
		const char* end = std::strchr(p, ';');
		const size_t len = end ? (size_t)(end - p) : std::strlen(p);
		char e[128];
		std::snprintf(e, sizeof(e), "%.*s", (int)(len < 127 ? len : 127), p);
		p = end ? end + 1 : p + len;
		char* colon = std::strchr(e, ':');
		char* at    = std::strchr(e, '@');
		if (colon == nullptr || at == nullptr || (sDone & (1u << idx)))
			continue;
		*colon = '\0';
		*at    = '\0';
		if (std::strcmp(e, me) != 0)
			continue;
		char* eq            = std::strchr(at + 1, '=');
		const unsigned when = (unsigned)std::strtoul(at + 1, nullptr, 10);
		if (port_frame_count() < when)
			continue;
		sDone |= 1u << idx;
		const char* action = colon + 1;
		const char* arg    = eq ? eq + 1 : "";
		std::fprintf(stderr, "[mp-presence] test: %s %s\n", action, arg);
		if (std::strcmp(action, "plaza") == 0) {
			queueRequest(REQ_PLAZA, 0, 0, -1);
		} else if (std::strcmp(action, "goto") == 0) {
			int s = 1, c = 0;
			std::sscanf(arg, "%d,%d", &s, &c);
			queueRequest(REQ_GOTO, s, c, -1);
		} else if (std::strcmp(action, "panel") == 0) {
			port_mp_menu_open(1);
		} else if ((std::strcmp(action, "lsoff") == 0 || std::strcmp(action, "lson") == 0) && port_mp_is_host()) {
			PortMpSessionSettings s;
			port_mp_get_settings(&s);
			s.reserved[kResLevelSelect] = action[3] == 'f' ? 1 : 0;
			port_mp_host_set_settings(&s);
		} else if (std::strcmp(action, "warp") == 0) {
			for (int i = 0; i < PORT_MP_MAX_PLAYERS; i++) {
				PortMpPlayerInfo pi;
				if (port_mp_get_player(i, &pi) && !pi.isLocal && std::strcmp(pi.name, arg) == 0)
					queueRequest(REQ_WARP_TO, 0, 0, i);
			}
		}
	}
}

void gameFrame()
{
	static TMarDirector* sDirector;
	static int sStage = -1, sScenario = -1, sFrames;
	TApplication* app  = SMSGetApplication();
	const int stage    = app->mCurrArea.getStage();
	const int scenario = app->mCurrArea.getScenario();
	const bool newVisit = gpMarDirector != sDirector || stage != sStage || scenario != sScenario;
	if (newVisit) {
		sDirector = gpMarDirector;
		sStage    = stage;
		sScenario = scenario;
		sFrames   = 0;
	} else {
		sFrames++;
	}
	trackSafePosition(stage, scenario, newVisit);
	{
		std::lock_guard<std::mutex> lk(sMu);
		sLocal.at       = port_mp_time_ms();
		sLocal.activity = currentActivity();
	}
	testKnob();
	handleRequest(stage, scenario);
	spawnFrame(stage, scenario, sFrames);
	markersFrame();
}

// ---- main thread ---------------------------------------------------------------------------
void sendPresence(bool reliable)
{
	TApplication* app = SMSGetApplication();
	MsgPresence m;
	std::memset(&m, 0, sizeof(m));
	m.type     = MSG_PRESENCE;
	m.appState = app->mAppState;
	m.stage    = app->mCurrArea.getStage();
	m.scenario = app->mCurrArea.getScenario();
	{
		std::lock_guard<std::mutex> lk(sMu);
		m.activity = localActivityLocked();
	}
	port_mp_send(PORT_MP_CH_WARP, &m, sizeof(m), reliable ? 1 : 0, -1);
}

void netPoll()
{
	u8 buf[PORT_MP_APP_MAX];
	int from, n;
	while ((n = port_mp_recv(PORT_MP_CH_WARP, buf, sizeof(buf), &from)) != 0) {
		if (n < 1 || from < 0 || from >= PORT_MP_MAX_PLAYERS)
			continue;
		PortMpPlayerInfo p;
		if (!port_mp_get_player(from, &p))
			continue;
		if (buf[0] == MSG_PRESENCE && n == (int)sizeof(MsgPresence)) {
			MsgPresence m;
			std::memcpy(&m, buf, sizeof(m));
			std::lock_guard<std::mutex> lk(sMu);
			RemotePresence& r = sRemote[from];
			const bool changed = !r.have || r.serial != p.serial || std::memcmp(&r.msg, &m, sizeof(m)) != 0;
			r.have             = true;
			r.serial           = p.serial;
			r.msg              = m;
			r.at               = port_mp_time_ms();
			if (changed)
				std::fprintf(stderr, "[mp-presence] slot %d (%s): app %d stage %d/%d activity %d\n", from, p.name,
				             m.appState, m.stage, m.scenario, m.activity);
		} else if (buf[0] == MSG_SPAWN_REQ && n == (int)sizeof(MsgSpawnReq)) {
			MsgSpawnReq q;
			std::memcpy(&q, buf, sizeof(q));
			MsgSpawnPos a;
			std::memset(&a, 0, sizeof(a));
			a.type     = MSG_SPAWN_POS;
			a.stage    = q.stage;
			a.scenario = q.scenario;
			a.nonce    = q.nonce;
			TApplication* app = SMSGetApplication();
			{
				std::lock_guard<std::mutex> lk(sMu);
				if (sLocal.safeValid && sLocal.safeStage == q.stage && sLocal.safeScenario == q.scenario
				    && app->mCurrArea.getStage() == q.stage && app->mCurrArea.getScenario() == q.scenario) {
					a.valid = 1;
					std::memcpy(a.pos, sLocal.safePos, sizeof(a.pos));
					a.yawDeg = sLocal.safeYaw;
				}
			}
			port_mp_send(PORT_MP_CH_WARP, &a, sizeof(a), 1, from);
			std::fprintf(stderr, "[mp-presence] spawn position for slot %d (%s): %s\n", from, p.name,
			             a.valid ? "sent" : "none (not in that stage / not on the ground yet)");
		} else if (buf[0] == MSG_SPAWN_POS && n == (int)sizeof(MsgSpawnPos)) {
			MsgSpawnPos a;
			std::memcpy(&a, buf, sizeof(a));
			std::lock_guard<std::mutex> lk(sMu);
			if (sSpawn.active && sSpawn.sent && from == sSpawn.slot && a.nonce == sSpawn.nonce) {
				sSpawn.answered = true;
				sSpawn.answer   = a;
			}
		}
	}
}

// Presence: reliable on every change, unreliable every 3 s (and reliable again whenever
// someone new joins, so they get it at once).
void presenceSend()
{
	static MsgPresence sLast;
	static double sLastSent, sLastChange;
	static u32 sSerials[PORT_MP_MAX_PLAYERS];
	TApplication* app = SMSGetApplication();
	MsgPresence m;
	std::memset(&m, 0, sizeof(m));
	m.type     = MSG_PRESENCE;
	m.appState = app->mAppState;
	m.stage    = app->mCurrArea.getStage();
	m.scenario = app->mCurrArea.getScenario();
	{
		std::lock_guard<std::mutex> lk(sMu);
		m.activity = localActivityLocked();
	}
	bool newPeer = false;
	for (int i = 0; i < PORT_MP_MAX_PLAYERS; i++) {
		PortMpPlayerInfo p;
		const u32 s = port_mp_get_player(i, &p) && !p.isLocal ? p.serial : 0;
		if (s != 0 && s != sSerials[i])
			newPeer = true;
		sSerials[i] = s;
	}
	const double now = port_mp_time_ms();
	if (std::memcmp(&m, &sLast, sizeof(m)) != 0) {
		sLast       = m;
		sLastChange = now;
	}
	// changes: reliable, at most every 250 ms (pause toggling, loading flicker)
	if ((sLastChange > sLastSent && now - sLastSent > 250.0) || newPeer) {
		sendPresence(true);
		sLastSent = now;
	} else if (now - sLastSent > 3000.0) {
		sendPresence(false);
		sLastSent = now;
	}
}

void contentBox(float winW, float winH, float& x, float& y, float& w, float& h)
{
	const float aspect = port_get_display_aspect();
	x = 0.0f, y = 0.0f, w = winW, h = winH;
	if (aspect <= 0.0f || winW <= 0.0f || winH <= 0.0f)
		return;
	if (winW / winH > aspect) {
		w = winH * aspect;
		x = (winW - w) * 0.5f;
	} else {
		h = winW / aspect;
		y = (winH - h) * 0.5f;
	}
}

void drawMarkers()
{
	Marker mk[PORT_MP_MAX_PLAYERS];
	int n;
	{
		std::lock_guard<std::mutex> lk(sMu);
		n = port_mp_time_ms() - sMarkersAt > 500.0 ? 0 : sMarkerCount;
		std::memcpy(mk, sMarkers, sizeof(Marker) * n);
	}
	if (n == 0 || !sShowMarkers.load() || SMSGetApplication()->mAppState != TApplication::APP_STATE_GAMEPLAY)
		return;
	const ImVec2 disp = ImGui::GetIO().DisplaySize;
	float bx, by, bw, bh;
	contentBox(disp.x, disp.y, bx, by, bw, bh);
	ImDrawList* dl   = ImGui::GetBackgroundDrawList();
	ImFont* font     = ImGui::GetFont();
	const float size = bh * 0.022f > ImGui::GetFontSize() ? bh * 0.022f : ImGui::GetFontSize();
	const float a    = bh * 0.03f; // arrow size
	for (int i = 0; i < n; i++) {
		const Marker& m = mk[i];
		const ImVec2 c(bx + m.x * bw, by + m.y * bh);
		const ImVec2 dir(m.dirX, m.dirY), side(-m.dirY, m.dirX);
		const ImVec2 tip(c.x + dir.x * a, c.y + dir.y * a);
		const ImVec2 b1(c.x - dir.x * a * 0.6f + side.x * a * 0.7f, c.y - dir.y * a * 0.6f + side.y * a * 0.7f);
		const ImVec2 b2(c.x - dir.x * a * 0.6f - side.x * a * 0.7f, c.y - dir.y * a * 0.6f - side.y * a * 0.7f);
		const ImU32 tag = IM_COL32((m.rgb >> 16) & 0xFF, (m.rgb >> 8) & 0xFF, m.rgb & 0xFF, 255);
		dl->AddTriangleFilled(tip, b1, b2, (tag & 0x00FFFFFFu) | 0xE6000000u);
		dl->AddTriangle(tip, b1, b2, IM_COL32(0, 0, 0, 200), 2.0f);
		char t[64];
		std::snprintf(t, sizeof(t), "%s %.0fm", m.name, m.distM);
		const bool game = mpui_font_ready(); // the game's own font, else ImGui's
		const float px  = size * 1.25f;
		const ImVec2 sz = game ? mpui_text_size(t, px) : font->CalcTextSizeA(size, 1e9f, 0.0f, t);
		// text on the inner side of the arrow, kept inside the picture
		const float back = a * 1.1f + 0.5f * (std::fabs(dir.x) * sz.x + std::fabs(dir.y) * sz.y) + 4.0f; // clear of the arrow
		float tx = c.x - dir.x * back - sz.x * 0.5f, ty = c.y - dir.y * back - sz.y * 0.5f;
		tx       = tx < bx + 2.0f ? bx + 2.0f : (tx + sz.x > bx + bw - 2.0f ? bx + bw - 2.0f - sz.x : tx);
		ty       = ty < by + 2.0f ? by + 2.0f : (ty + sz.y > by + bh - 2.0f ? by + bh - 2.0f - sz.y : ty);
		dl->AddRectFilled(ImVec2(tx - 4, ty - 2), ImVec2(tx + sz.x + 4, ty + sz.y + 2), IM_COL32(0, 0, 0, 140), 4.0f);
		if (game)
			mpui_text(dl, ImVec2(tx, ty), px, tag, t);
		else
			dl->AddText(font, size, ImVec2(tx, ty), tag, t);
	}
}

void overlay()
{
	netPoll();
	presenceSend();
	if (ImGui::GetCurrentContext() != nullptr)
		drawMarkers();
}

// ---- lobby panel section ---------------------------------------------------------------------
void lobbySection(int isHost)
{
	PortMpSessionSettings s;
	port_mp_get_settings(&s);
	TApplication* app   = SMSGetApplication();
	const bool inStage  = app->mAppState == TApplication::APP_STATE_GAMEPLAY && app->mCurrArea.getStage() != 15;
	const bool canWarp  = inStage;
	if (ImGui::BeginTable("presence", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
		ImGui::TableSetupColumn("Player");
		ImGui::TableSetupColumn("Where");
		ImGui::TableSetupColumn("");
		ImGui::TableHeadersRow();
		for (int i = 0; i < PORT_MP_MAX_PLAYERS; i++) {
			PortMpPlayerInfo p;
			if (!port_mp_get_player(i, &p))
				continue;
			char where[128];
			describe(i, p, where, sizeof(where));
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::Text("%s%s", p.name, p.isLocal ? " (you)" : "");
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(where);
			ImGui::TableNextColumn();
			if (!p.isLocal) {
				ImGui::PushID(i);
				const bool ok = canWarp && warpableStage(p.stage);
				if (!ok)
					ImGui::BeginDisabled();
				if (ImGui::SmallButton("Warp to"))
					queueRequest(REQ_WARP_TO, 0, 0, i);
				if (!ok)
					ImGui::EndDisabled();
				ImGui::PopID();
			}
		}
		ImGui::EndTable();
	}
	if (!canWarp)
		ImGui::TextDisabled("(warps and the level select work while you are in a stage)");
	if (!canWarp)
		ImGui::BeginDisabled();
	if (ImGui::Button("Return to Delfino Plaza"))
		queueRequest(REQ_PLAZA, 0, 0, -1);
	if (!canWarp)
		ImGui::EndDisabled();
	if (app->mAppState == TApplication::APP_STATE_GAMEPLAY && gpMarDirector != nullptr
	    && gpMarDirector->mState == TMarDirector::STATE_PAUSE_MENU)
		ImGui::TextDisabled("(a warp starts when the pause menu is closed)");
	bool show = sShowMarkers.load();
	if (ImGui::Checkbox("Arrows to off-screen players in your stage", &show))
		sShowMarkers.store(show);

	const bool allowedForAll = s.reserved[kResLevelSelect] == 0;
	if (isHost) {
		bool allow = allowedForAll;
		if (ImGui::Checkbox("Level select for everyone", &allow)) {
			s.reserved[kResLevelSelect] = allow ? 0 : 1;
			port_mp_host_set_settings(&s);
		}
	}
	if (!isHost && !allowedForAll) {
		ImGui::TextDisabled("Level select: switched off by the host");
		return;
	}
	if (!ImGui::TreeNode("Level select"))
		return;
	if (!canWarp)
		ImGui::BeginDisabled();
	auto episodes = [](const StageDef& d) {
		if (d.episodes <= 1) {
			if (ImGui::Selectable(d.name))
				queueRequest(REQ_GOTO, d.stage, 0, -1);
			return;
		}
		if (ImGui::TreeNode(d.name)) {
			for (int e = 0; e < d.episodes; e++) {
				char lbl[32];
				std::snprintf(lbl, sizeof(lbl), "Episode %d", e + 1);
				ImGui::PushID(e);
				if (ImGui::Selectable(lbl))
					queueRequest(REQ_GOTO, d.stage, e, -1);
				ImGui::PopID();
			}
			ImGui::TreePop();
		}
	};
	if (ImGui::Selectable("Delfino Plaza"))
		queueRequest(REQ_PLAZA, 0, 0, -1);
	for (const StageDef& d : kLevels)
		episodes(d);
	if (ImGui::Selectable("Corona Mountain"))
		queueRequest(REQ_GOTO, 52, 0, -1);
	if (ImGui::TreeNode("Bosses")) {
		for (const StageDef& d : kBosses)
			episodes(d);
		ImGui::TreePop();
	}
	if (ImGui::TreeNode("Secret courses")) {
		for (int i = 0; i < kExCount; i++)
			if (ImGui::Selectable(kExNames[i]))
				queueRequest(REQ_GOTO, kExFirst + i, 0, -1);
		ImGui::TreePop();
	}
	if (!canWarp)
		ImGui::EndDisabled();
	ImGui::TreePop();
}

struct Register {
	Register()
	{
		port_mp_register_game_frame(gameFrame);
		port_mp_register_overlay(overlay);
		port_mp_register_lobby_section("Where is everyone / level select", lobbySection);
	}
} sRegister;

} // namespace

// Discord integration (port_discord.cpp): "Bianco Hills, episode 3" style stage label.
extern "C" void port_mp_stage_label(int stage, int scenario, char* out, size_t cap)
{
	stageLabel(stage, scenario, out, cap);
}

#endif // SMS_PORT_AURORA
