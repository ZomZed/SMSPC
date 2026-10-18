// Multiplayer game modes and the in-game player list (w15).
//
// Game modes (PortMpSessionSettings.gameMode = the mode running now, reserved[8] = how many
// times the host pressed "Start"; both reach late joiners with the settings):
//  - None (default): everyone in Delfino Plaza (the end-game one, dolpic10), no way out: the warp
//    floors and pipes (TMario's BG warp check), the painted gates (TMario::warpIn), the stage-change
//    objects (MapObjTown.cpp) and the sun's way to Noki Bay (sunmgr.cpp) do nothing while it runs
//    (hooks under SMS_PORT_AURORA, port_mp_plaza_exit_blocked), and w11's level select is off. A
//    Hide and Seek / Freeze Tag / Prop Hunt match in the plaza closes them the same way.
//  - Free: every entrance open (the free-mode save already opens them all).
// Starting a mode (host, lobby panel) bumps reserved[8]; every client then reloads the plaza
// (everyone respawns at its start), so a restart, a switch, and going back to None all work
// the same way. A late joiner takes the running mode without the reload.
//
// Player list: the session's players, top right of the game picture, in the game's own font
// (gpSystemFont through J2DPrint), each in his cap colour. A view object in the director's
// "GX Post" perform list (CUE_DRAW, MarDirectorSetupObjects.cpp); drawn only in a session.
// With true widescreen it follows the HUD position setting like the game's HUD (w11).
#ifdef SMS_PORT_AURORA

#include "port_mp.h"

#include <GC2D/GCConsole2.hpp>
#include <JSystem/J2D/J2DOrthoGraph.hpp>
#include <JSystem/J2D/J2DPrint.hpp>
#include <JSystem/JDrama/JDRViewObj.hpp>
#include <JSystem/JUtility/JUTResFont.hpp>
#include <System/Application.hpp>
#include <System/FlagManager.hpp>
#include <System/MarDirector.hpp>
#include <System/StageUtil.hpp>
#include <Player/Mario.hpp>
#include <Player/MarioAccess.hpp>
#include <Player/MarioPositionObj.hpp>
#include <JSystem/JDrama/JDRNameRefGen.hpp>
#include <dolphin/gx.h>
#include <dolphin/mtx.h>
#include <imgui.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

extern "C" void port_mp_toast(const char* text);              // port_mp_menu.cpp
extern "C" int port_mp_hide_headline(char* out, int cap);     // port_mp_hideseek.cpp
extern "C" float PORTWideScale(void);                          // port_widescreen.cpp (w11)
extern "C" float PORTWideHudShift(int side, float frameWidth); // port_widescreen.cpp (w11)
extern JUTResFont* gpSystemFont;
extern "C" uint32_t port_mp_tag_rgb(int slot); // port_mp_palette.cpp
extern "C" const char* port_mp_lobby_name(void); // port_mp_game.cpp
extern "C" int port_key_held(int scancode);       // port_qol.cpp
extern "C" int port_mp_menu_wants_input(void);    // port_mp_menu.cpp
// port_mp_arena.cpp: where Hide and Seek / Freeze Tag is played
extern "C" int port_mp_arena_level_count(void);
extern "C" const char* port_mp_arena_level_name(int i);
extern "C" int port_mp_arena_code_for_level(int i);
extern "C" int port_mp_arena_all_levels(const PortMpSessionSettings* s);
extern "C" void port_mp_home_area(const PortMpSessionSettings* s, int* stage, int* scenario);
extern "C" const char* port_mp_arena_label(const PortMpSessionSettings* s);

namespace {

const int kResStartSeq = 8; // PortMpSessionSettings.reserved[8]: "Start" presses
const int kResArena    = 9; // reserved[9]: where Hide and Seek / Freeze Tag runs (port_mp_arena.cpp)
const int kResLock     = 10; // reserved[10]: the exit lock, seconds (all levels)
const int kDefaultLockS = 30;

int sMode = -1;      // running mode as last seen
int sSeq  = -1;      // start sequence as last seen
bool sReloadPending; // a mode start asks for a plaza reload

bool directorIdle()
{
	TMarDirector* d = gpMarDirector;
	return d != nullptr && d->mState == TMarDirector::STATE_UNK4
	       && (d->mFlags
	           & (TMarDirector::DIRECTOR_FLAG_SHINE_GET_PENDING | TMarDirector::DIRECTOR_FLAG_STAGE_TRANSITION_PENDING
	              | TMarDirector::DIRECTOR_FLAG_ACTOR_DEMO_STAGE_TRANSITION_PENDING
	              | TMarDirector::DIRECTOR_FLAG_GATE_DEMO_STAGE_TRANSITION_PENDING
	              | TMarDirector::DIRECTOR_FLAG_GAME_OVER_PENDING))
	              == 0;
}

void ctlFrame(); // below
void hostBeginMode(int mode); // below
void envModeFrame(PortMpSessionSettings& s); // below

void newsFrame(); // below (the D.E.B.S. ticker's lobby news)

// A session's plaza loads (a game mode's reload, joining from the file select) come from the plaza or
// the file select, for which TMarDirector::setMario uses the scene's default spot: the boat landing
// off the pier, in the sea. Instead each player starts on dry ground at one of the plaza's own
// "back from a level" spots (TMarioPositionObj "マリオセット位置"), picked by player slot, so the
// lobby is spread around the plaza.
void placeInPlaza()
{
	static unsigned sDoneSerial = ~0u;
	TMarDirector* d = gpMarDirector;
	TMario* m       = gpMarioOriginal;
	if (!port_mp_active() || d == nullptr || m == nullptr || d->mState != TMarDirector::STATE_UNK4
	    || sDoneSerial == gPortMpStageSerial)
		return;
	sDoneSerial = gPortMpStageSerial;
	TApplication* app = SMSGetApplication();
	const int prev    = app->mPrevArea.getStage();
	if (app->mCurrArea.getStage() != 1 || (prev != 15 && SMS_getShineStage((u8)prev) != 1))
		return; // not a plaza visit the game starts in the sea
	// "マリオセット位置" in Shift-JIS, as the scene names it (this file is UTF-8)
	const TMarioPositionObj* spots = (const TMarioPositionObj*)JDrama::TNameRefGen::search(
	    "\x83\x7d\x83\x8a\x83\x49\x83\x5a\x83\x62\x83\x67\x88\xca\x92\x75");
	if (spots == nullptr || spots->unkD0 == 0)
		return;
	// the ground-level spots first (Bianco, Noki, Ricco, Gelato, Pinna); the high ones (the Shine Gate,
	// the statue, Corona) only for a bigger lobby
	static const u8 kOrder[8] = { 0, 6, 1, 2, 3, 4, 5, 7 };
	const int slot = port_mp_local_slot();
	u8 k           = kOrder[(slot >= 0 ? slot : 0) % 8];
	if (k >= spots->unkD0)
		k = 0;
	const JGeometry::TVec3<f32>& p = spots->getUnk10(k);
	m->mPosition.set(p.x, p.y, p.z);
	m->mVel.set(0.0f, 0.0f, 0.0f);
	m->mForwardVel = 0.0f;
	m->mFaceAngle.y = m->mModelFaceAngle = (s16)(spots->getUnk70(k).y * (65536.0f / 360.0f));
	std::fprintf(stderr, "[mp] plaza start: spot %d of %d (%.0f %.0f %.0f)\n", k, (int)spots->unkD0, p.x, p.y, p.z);
}

void gameFrame()
{
	ctlFrame();
	newsFrame();
	placeInPlaza();
	PortMpSessionSettings s;
	port_mp_get_settings(&s);
	envModeFrame(s); // SMS_PORT_MP_MODE (below)
	const int seq = s.reserved[kResStartSeq];
	if (sSeq < 0) { // first look (joined, or created): take what runs, no reload
		sSeq  = seq;
		sMode = s.gameMode;
	}
	if (seq != sSeq) {
		sSeq           = seq;
		sMode          = s.gameMode;
		sReloadPending = true;
		char t[128];
		const char* arena = port_mp_arena_label(&s);
		std::snprintf(t, sizeof(t), "The host started %s%s%s%s.",
		              sMode == PORT_MP_MODE_FREE   ? "Free mode: every level is open"
		              : sMode == PORT_MP_MODE_HIDE ? "Hide and Seek"
		              : sMode == PORT_MP_MODE_FREEZE ? "Freeze Tag"
		              : sMode == PORT_MP_MODE_PROP ? "Prop Hunt"
		              : sMode == PORT_MP_MODE_MANHUNT ? "Manhunt"
		              : sMode == PORT_MP_MODE_SHINERUN ? "Shine Runners"
		              : sMode == PORT_MP_MODE_CNR ? "Cops and Robbers"
		                                           : "game mode None: plaza only",
		              arena != nullptr ? " (" : "", arena != nullptr ? arena : "", arena != nullptr ? ")" : "");
		port_mp_toast(t);
		std::fprintf(stderr, "[mp] game mode %d started (start %d): back to the plaza start\n", sMode, seq);
	}
	sMode = s.gameMode;
	if (sReloadPending && directorIdle() && SMSGetApplication()->mCurrArea.getStage() != 15) {
		sReloadPending = false;
		int stage, scen;
		port_mp_home_area(&s, &stage, &scen); // the plaza, or a single-level match's level
		SMSGetApplication()->setNextArea(TGameSequence((u8)stage, (u8)scen));
		gpMarDirector->onFlag(TMarDirector::DIRECTOR_FLAG_STAGE_TRANSITION_PENDING);
	}
}

// ---- mode control: Voting -> Waiting -> Starting, queue, AutoStart, per-mode settings ---------
// The host runs it and sends its state to everyone (PORT_MP_CH_MODECTL, reliable on every change
// and once a second for late joiners); players send their vote. Starting a mode still goes through
// the settings above (gameMode + the start counter), so every client reloads the plaza as before.
//  - Voting (host option, at least 10 s): the winning mode joins the queue.
//  - Waiting: its own timer (off, or at least 10 s). After a vote, no timer = skipped. Without
//    voting it always runs (its timer, or 10 s), so a mode never starts at once. Nothing queued =
//    it waits for as long as it takes and goes on once a mode is queued.
//  - Starting: the countdown (0 = right away), then the queue's front mode begins.
//  - A mode with a time limit ends back in the lobby; with AutoStart on, the next cycle begins.
const uint8_t kCtlChannel = PORT_MP_CH_MODECTL;
const int kMinTimerS      = 10; // voting and waiting timers are at least 10 s
const int kQueueMax       = 8;
enum Phase : u8 {
	PH_IDLE     = 0, // a mode runs without a time limit, or the lobby waits for the host
	PH_VOTING   = 1,
	PH_WAITING  = 2, // timer, or indefinitely while nothing is queued
	PH_STARTING = 3,
	PH_RUNNING  = 4, // a mode with a time limit
};
enum : u8 { MSG_CTL_STATE = 1, MSG_CTL_VOTE = 2 };

#pragma pack(push, 1)
struct CtlState {
	u8 type; // MSG_CTL_STATE
	u8 phase;
	u8 pendingMode; // Starting: the mode that begins
	u8 runningMode;
	u8 autoStart;
	u8 cycle; // a Voting/Waiting/Starting cycle is under way (Waiting goes on once a mode is queued)
	u8 queueLen;
	u8 queue[kQueueMax];
	u8 voteEnabled;
	u16 voteS;
	u16 waitS;  // 0 = no waiting timer
	u16 startS; // 0 = start right away
	u16 freeTimeLimitS; // Free mode setting: 0 = no time limit
	u32 leftMs;         // time left on the current phase's timer (0 = none)
	u8 votes[PORT_MP_MAX_PLAYERS]; // each slot's vote (a mode), 0xFF = none
	// Hide and Seek settings (port_mp_hideseek.cpp); 0 = the default
	u16 hideS;       // hide phase (default 30 s, at least 10)
	u16 seekS;       // seek phase (default 180 s)
	u8 hideNoConvert; // 1 = a tagged hider is out instead of seeking
	u8 hidePick;      // seeker: 0 = random, else slot + 1
	// Freeze Tag settings (port_mp_freezetag.cpp); 0 = the default
	u16 freezeReadyS;  // get-ready phase (default 10 s)
	u16 freezePlayS;   // round (default 180 s)
	u8 freezeTaggers;  // how many taggers (default 1)
	u8 freezePick;     // a tagger: 0 = random, else slot + 1
	// Where each of the two is played (port_mp_arena.cpp); 0 = the default
	u8 hideAllLevels;   // 0 = a single level, 1 = all levels
	u8 hideLevel;       // single level: port_mp_arena_level_name index (0 = Delfino Plaza)
	u8 hideLockS;       // all levels: exit lock seconds + 1 (0 = the default, 30 s)
	u8 freezeAllLevels;
	u8 freezeLevel;
	u8 freezeLockS;
	u8 freezeDeathOut; // 1 = a runner who dies is out (default: he joins the taggers)
	// Prop Hunt settings (port_mp_prophunt.cpp); 0 = the default
	u16 propHideS;     // hide phase (default 45 s)
	u16 propHuntS;     // hunt phase (default 240 s)
	u8 propNoConvert;  // 1 = a caught hider is out instead of hunting
	u8 propPick;       // the hunter: 0 = random, else slot + 1
	u8 propDamage;     // health a spray hit costs (of 8; default 2)
	u8 propWaterMul;   // how much faster hunters' tanks empty (default 4x)
	u8 propAllLevels;
	u8 propLevel;
	u8 propLockS;
	u8 propTauntS;     // hiders' Shadow Mario sound every this many s of the hunt (0 = 30 s, 255 = off)
	// Manhunt settings (port_mp_manhunt.cpp); 0 = the default
	u8 mhGoal;      // Shines the runner needs (default 3)
	u8 mhHeadS;     // head start (default 20 s)
	u8 mhTimeMin;   // time limit in minutes + 1 (0 = the default, 20; 1 = none)
	u8 mhDamage;    // health a hunter's hit costs (of 8; default 3)
	u8 mhPick;      // the runner: 0 = random, else slot + 1
	u8 mhSwitch;    // 1 = Switch: the hunter who kills the runner runs; most Shines wins
	// Shine Runners settings (port_mp_shinerun.cpp); 0 = the default
	u8 srGoal;      // Shines to win (default 5)
	u8 srTimeMin;   // time limit in minutes + 1 (0 = the default, 15; 1 = none)
	u8 srDeathLoss; // Shines lost by dying + 1 (0 = the default, 3)
	// Cops and Robbers settings (port_mp_cnr.cpp); 0 = the default
	u8 crCops;       // number of cops (0 = about a third)
	u16 crCoins;     // coin quota (default 100)
	u8 crShines;     // Shine quota + 1 (0 = the default, 3)
	u8 crTimeMin;    // time limit in minutes + 1 (0 = the default, 12; 1 = none)
	u8 crHeadS;      // head start (default 15 s)
	u8 crPick;       // a cop: 0 = random, else slot + 1
};
struct CtlVote {
	u8 type; // MSG_CTL_VOTE
	u8 mode; // 0xFF = take the vote back
};
#pragma pack(pop)

const char* const kModeShort[PORT_MP_MODE_COUNT] = { "None", "Free", "Hide and Seek", "Freeze Tag", "Prop Hunt", "Manhunt", "Shine Runners", "Cops and Robbers" };

CtlState sCtl = { MSG_CTL_STATE, PH_WAITING, PORT_MP_MODE_FREE, PORT_MP_MODE_NONE, 0, 0, 0, {}, 0, 20, 0, 10, 0, 0,
	              { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF } };
double sDeadline;   // local time the current phase's timer ends (0 = no timer)
double sLastCtlSend;
bool sCtlDirty = true;

const char* modeName(int m) { return m >= 0 && m < PORT_MP_MODE_COUNT ? kModeShort[m] : "?"; }
int clampTimer(int s) { return s < kMinTimerS ? kMinTimerS : (s > 3600 ? 3600 : s); }
int secondsLeft() { return sDeadline > 0.0 ? (int)((sDeadline - port_mp_time_ms() + 999.0) / 1000.0) : 0; }
void setTimer(int seconds) { sDeadline = seconds > 0 ? port_mp_time_ms() + seconds * 1000.0 : 0.0; }

// The mode with the most votes (-1: no votes). Ties go to the earlier mode.
int voteWinner()
{
	int count[PORT_MP_MODE_COUNT] = {};
	for (int i = 0; i < PORT_MP_MAX_PLAYERS; i++)
		if (sCtl.votes[i] < PORT_MP_MODE_COUNT && sCtl.votes[i] != PORT_MP_MODE_NONE)
			count[sCtl.votes[i]]++;
	int best = -1;
	for (int m = 0; m < PORT_MP_MODE_COUNT; m++)
		if (count[m] > 0 && (best < 0 || count[m] > count[best]))
			best = m;
	return best;
}

// The front of the queue (-1: empty). take = remove it.
int nextMode(bool take)
{
	if (sCtl.queueLen == 0)
		return -1;
	const int m = sCtl.queue[0];
	if (take) {
		std::memmove(&sCtl.queue[0], &sCtl.queue[1], sCtl.queueLen - 1);
		sCtl.queueLen--;
	}
	return m;
}

void enqueue(int mode, bool front)
{
	if (mode <= PORT_MP_MODE_NONE || mode >= PORT_MP_MODE_COUNT || sCtl.queueLen >= kQueueMax)
		return;
	if (front) {
		std::memmove(&sCtl.queue[1], &sCtl.queue[0], sCtl.queueLen);
		sCtl.queue[0] = (u8)mode;
	} else {
		sCtl.queue[sCtl.queueLen] = (u8)mode;
	}
	sCtl.queueLen++;
	sCtlDirty = true;
}

// Host: the mode begins for everyone now (the plaza reload of gameFrame above).
void hostBeginMode(int mode)
{
	PortMpSessionSettings s;
	port_mp_get_settings(&s);
	s.gameMode               = (uint8_t)mode;
	s.reserved[kResStartSeq] = (uint8_t)(s.reserved[kResStartSeq] + 1);
	// where Hide and Seek / Freeze Tag is played, with the mode (port_mp_arena.cpp)
	s.reserved[kResArena] = 0;
	s.reserved[kResLock]  = 0;
	if (mode == PORT_MP_MODE_HIDE || mode == PORT_MP_MODE_FREEZE || mode == PORT_MP_MODE_PROP) {
		const bool hide = mode == PORT_MP_MODE_HIDE, prop = mode == PORT_MP_MODE_PROP;
		const u8 all  = hide ? sCtl.hideAllLevels : prop ? sCtl.propAllLevels : sCtl.freezeAllLevels;
		const u8 lock = hide ? sCtl.hideLockS : prop ? sCtl.propLockS : sCtl.freezeLockS;
		const u8 lv   = hide ? sCtl.hideLevel : prop ? sCtl.propLevel : sCtl.freezeLevel;
		s.reserved[kResArena] = all ? 0xFF : (uint8_t)port_mp_arena_code_for_level(lv);
		s.reserved[kResLock]  = all ? (uint8_t)(lock ? lock - 1 : kDefaultLockS) : 0;
	}
	port_mp_host_set_settings(&s);
	sCtl.runningMode = (u8)mode;
	sCtl.cycle       = 0;
	if (mode == PORT_MP_MODE_FREE && sCtl.freeTimeLimitS > 0) {
		sCtl.phase = PH_RUNNING;
		setTimer(sCtl.freeTimeLimitS);
	} else {
		sCtl.phase = mode == PORT_MP_MODE_NONE ? PH_WAITING : PH_IDLE; // the lobby waits for a mode
		setTimer(0);
	}
	sCtlDirty = true;
	std::fprintf(stderr, "[mp] mode control: %s begins%s (t=%.0f s)\n", modeName(mode),
	             sCtl.phase == PH_RUNNING ? " (timed)" : "", port_mp_time_ms() / 1000.0);
}

// SMS_PORT_MP_MODE (gameFrame above).
void envModeFrame(PortMpSessionSettings& s)
{
	// SMS_PORT_MP_MODE=free|none|hide|freeze[:all|:<level>][:lock=<s>][:pick=<slot>]@<logic frame> (host, tests):
	// presses "Start" once. hide/freeze: ":all" = all levels, ":<n>" = single level n of the
	// arena list (0 = the plaza, the default), ":lock=<s>" = the exit lock.
	static const char* sEnvMode = std::getenv("SMS_PORT_MP_MODE");
	static int sEnvFrames;
	if (sEnvMode != nullptr && port_mp_is_host() && gpMarDirector != nullptr
	    && gpMarDirector->mState == TMarDirector::STATE_UNK4) {
		const char* at = std::strchr(sEnvMode, '@');
		if (++sEnvFrames == (at != nullptr ? std::atoi(at + 1) : 60)) {
			const int mode = std::strncmp(sEnvMode, "cops", 4) == 0     ? PORT_MP_MODE_CNR
			                 : std::strncmp(sEnvMode, "shine", 5) == 0  ? PORT_MP_MODE_SHINERUN
			                 : std::strncmp(sEnvMode, "manhunt", 7) == 0 ? PORT_MP_MODE_MANHUNT
			                 : std::strncmp(sEnvMode, "prop", 4) == 0   ? PORT_MP_MODE_PROP
			                 : std::strncmp(sEnvMode, "freeze", 6) == 0 ? PORT_MP_MODE_FREEZE // (before "free")
			                 : std::strncmp(sEnvMode, "free", 4) == 0   ? PORT_MP_MODE_FREE
			                 : std::strncmp(sEnvMode, "hide", 4) == 0   ? PORT_MP_MODE_HIDE
			                                                           : PORT_MP_MODE_NONE;
			const char* colon = std::strchr(sEnvMode, ':');
			const bool all    = colon != nullptr && std::strncmp(colon + 1, "all", 3) == 0;
			const int level   = colon != nullptr && !all && colon[1] >= '0' && colon[1] <= '9' ? std::atoi(colon + 1) : 0;
			const char* lock  = std::strstr(sEnvMode, "lock=");
			const u8 lockS    = lock != nullptr ? (u8)(std::atoi(lock + 5) + 1) : 0;
			const char* pick = std::strstr(sEnvMode, "pick="); // the seeker / a tagger: a slot
			if (pick != nullptr)
				sCtl.hidePick = sCtl.freezePick = sCtl.propPick = sCtl.mhPick = (u8)(std::atoi(pick + 5) + 1);
			if (const char* g = std::strstr(sEnvMode, "goal="))
				sCtl.mhGoal = (u8)std::atoi(g + 5);
			if (const char* h = std::strstr(sEnvMode, "head="))
				sCtl.mhHeadS = (u8)std::atoi(h + 5);
			if (mode == PORT_MP_MODE_MANHUNT)
				sCtl.mhSwitch = std::strstr(sEnvMode, "switch") != nullptr;
			if (mode == PORT_MP_MODE_SHINERUN && std::strstr(sEnvMode, "goal=") != nullptr)
				sCtl.srGoal = sCtl.mhGoal;
			if (const char* ta = std::strstr(sEnvMode, "taunt=")) // Prop Hunt: hider sound every <s> (0 = off)
				sCtl.propTauntS = (u8)(std::atoi(ta + 6) == 0 ? 255 : std::atoi(ta + 6));
			if (const char* hs = std::strstr(sEnvMode, "hidetime="))
				sCtl.propHideS = (u16)std::atoi(hs + 9);
			if (const char* dl = std::strstr(sEnvMode, "loss="))
				sCtl.srDeathLoss = (u8)(std::atoi(dl + 5) + 1);
			if (mode == PORT_MP_MODE_CNR) {
				if (pick != nullptr)
					sCtl.crPick = (u8)(std::atoi(pick + 5) + 1);
				if (const char* q = std::strstr(sEnvMode, "coins="))
					sCtl.crCoins = (u16)std::atoi(q + 6);
				if (const char* q = std::strstr(sEnvMode, "shines="))
					sCtl.crShines = (u8)(std::atoi(q + 7) + 1);
				if (const char* h = std::strstr(sEnvMode, "head="))
					sCtl.crHeadS = (u8)std::atoi(h + 5);
			}
			if (mode == PORT_MP_MODE_HIDE)
				sCtl.hideAllLevels = all, sCtl.hideLevel = (u8)level, sCtl.hideLockS = lockS;
			if (mode == PORT_MP_MODE_FREEZE)
				sCtl.freezeAllLevels = all, sCtl.freezeLevel = (u8)level, sCtl.freezeLockS = lockS;
			if (mode == PORT_MP_MODE_PROP)
				sCtl.propAllLevels = all, sCtl.propLevel = (u8)level, sCtl.propLockS = lockS;
			hostBeginMode(mode);
			std::fprintf(stderr, "[mp] SMS_PORT_MP_MODE: host starts mode %d (%s)\n", mode, sEnvMode);
			port_mp_get_settings(&s);
		}
	}
}

void hostEnterStarting()
{
	const int m = nextMode(true);
	if (m < 0) { // nothing queued after all: keep waiting
		sCtl.phase = PH_WAITING;
		setTimer(0);
		sCtlDirty = true;
		return;
	}
	sCtl.pendingMode = (u8)m;
	if (sCtl.startS == 0) {
		hostBeginMode(m);
		return;
	}
	sCtl.phase = PH_STARTING;
	setTimer(sCtl.startS);
	sCtlDirty = true;
}

// Waiting's timer: after a vote its own timer (none = skip), without voting its timer or 10 s.
void hostArmWaiting(bool afterVote)
{
	sCtl.phase = PH_WAITING;
	sCtlDirty  = true;
	if (sCtl.queueLen == 0) {
		setTimer(0); // nothing queued: wait for as long as it takes
		return;
	}
	if (afterVote) {
		if (sCtl.waitS == 0) {
			hostEnterStarting();
			return;
		}
		setTimer(clampTimer(sCtl.waitS));
	} else {
		setTimer(sCtl.waitS > 0 ? clampTimer(sCtl.waitS) : kMinTimerS);
	}
}

// A cycle begins (the host's Start, or AutoStart after a mode): Voting if on, else Waiting.
void hostBeginCycle()
{
	sCtl.cycle = 1;
	std::memset(sCtl.votes, 0xFF, sizeof(sCtl.votes));
	if (sCtl.voteEnabled) {
		sCtl.phase = PH_VOTING;
		setTimer(clampTimer(sCtl.voteS));
		sCtlDirty = true;
		port_mp_toast("Vote for the next game mode! (F2: lobby)");
		return;
	}
	hostArmWaiting(false);
}

void hostEndVoting()
{
	const int won = voteWinner();
	enqueue(won, false);
	std::memset(sCtl.votes, 0xFF, sizeof(sCtl.votes));
	char t[80];
	if (won >= 0)
		std::snprintf(t, sizeof(t), "Vote won by %s.", modeName(won));
	else
		std::snprintf(t, sizeof(t), "No votes.");
	port_mp_toast(t);
	hostArmWaiting(true);
}

void hostEndMode()
{
	port_mp_toast("Time is up: back to the lobby.");
	hostBeginMode(PORT_MP_MODE_NONE);
	if (sCtl.autoStart)
		hostBeginCycle();
	else {
		sCtl.phase = PH_WAITING; // the host starts the next one
		setTimer(0);
	}
	sCtlDirty = true;
}

} // namespace
extern "C" int port_mp_hide_headline(char* out, int cap); // port_mp_hideseek.cpp
extern "C" const char* port_mp_hide_role(int slot);
extern "C" int port_mp_freeze_headline(char* out, int cap); // port_mp_freezetag.cpp
extern "C" int port_mp_prop_headline(char* out, int cap);   // port_mp_prophunt.cpp
extern "C" int port_mp_manhunt_headline(char* out, int cap); // port_mp_manhunt.cpp
extern "C" const char* port_mp_manhunt_role(int slot);
extern "C" int port_mp_shinerun_headline(char* out, int cap); // port_mp_shinerun.cpp
extern "C" const char* port_mp_shinerun_role(int slot);
extern "C" int port_mp_cnr_headline(char* out, int cap); // port_mp_cnr.cpp
extern "C" const char* port_mp_cnr_role(int slot);
extern "C" const char* port_mp_prop_role(int slot);
extern "C" int port_mp_prop_blind(void);
extern "C" int port_mp_prop_hide_tag(int slot);
extern "C" int port_mp_hide_hide_tag(int slot);             // port_mp_hideseek.cpp
extern "C" const char* port_mp_freeze_role(int slot);
extern "C" int port_mp_hide_blind(void);
// port_mp_hideseek.cpp: the round is over: back to the lobby (and AutoStart).
extern "C" void port_mp_modes_round_over(void)
{
	if (!port_mp_is_host())
		return;
	hostBeginMode(PORT_MP_MODE_NONE);
	if (sCtl.autoStart)
		hostBeginCycle();
	else {
		sCtl.phase = PH_WAITING;
		setTimer(0);
	}
	sCtlDirty = true;
}
extern "C" void port_mp_freeze_settings(int* readyS, int* playS, int* taggers, int* pick)
{
	*readyS  = sCtl.freezeReadyS ? sCtl.freezeReadyS : 10;
	*playS   = sCtl.freezePlayS ? sCtl.freezePlayS : 180;
	*taggers = sCtl.freezeTaggers ? sCtl.freezeTaggers : 1;
	*pick    = sCtl.freezePick ? sCtl.freezePick - 1 : -1;
}
extern "C" void port_mp_prop_settings(int* hideS, int* huntS, int* convert, int* pick, int* damage, int* waterMul)
{
	*hideS    = sCtl.propHideS ? (sCtl.propHideS < 10 ? 10 : sCtl.propHideS) : 45;
	*huntS    = sCtl.propHuntS ? sCtl.propHuntS : 240;
	*convert  = sCtl.propNoConvert ? 0 : 1;
	*pick     = sCtl.propPick ? sCtl.propPick - 1 : -1;
	*damage   = sCtl.propDamage ? sCtl.propDamage : 2;
	*waterMul = sCtl.propWaterMul ? sCtl.propWaterMul : 4;
}
// Prop Hunt: seconds between the hiders' Shadow Mario sounds in the hunt (0 = off).
extern "C" int port_mp_prop_taunt_seconds(void)
{
	return sCtl.propTauntS == 255 ? 0 : sCtl.propTauntS ? sCtl.propTauntS : 30;
}
extern "C" void port_mp_manhunt_settings(int* goal, int* headS, int* timeS, int* damage, int* pick, int* swap)
{
	*swap   = sCtl.mhSwitch ? 1 : 0;
	*goal   = sCtl.mhGoal ? sCtl.mhGoal : 3;
	*headS  = sCtl.mhHeadS ? sCtl.mhHeadS : 20;
	*timeS  = sCtl.mhTimeMin ? (sCtl.mhTimeMin - 1) * 60 : 20 * 60;
	*damage = sCtl.mhDamage ? sCtl.mhDamage : 3;
	*pick   = sCtl.mhPick ? sCtl.mhPick - 1 : -1;
}
extern "C" void port_mp_shinerun_settings(int* goal, int* timeS, int* deathLoss)
{
	*goal      = sCtl.srGoal ? sCtl.srGoal : 5;
	*timeS     = sCtl.srTimeMin ? (sCtl.srTimeMin - 1) * 60 : 15 * 60;
	*deathLoss = sCtl.srDeathLoss ? sCtl.srDeathLoss - 1 : 3;
}
extern "C" void port_mp_cnr_settings(int* cops, int* coinQuota, int* shineQuota, int* timeS, int* headS, int* pick)
{
	*cops       = sCtl.crCops;
	*coinQuota  = sCtl.crCoins ? sCtl.crCoins : 100;
	*shineQuota = sCtl.crShines ? sCtl.crShines - 1 : 3;
	*timeS      = sCtl.crTimeMin ? (sCtl.crTimeMin - 1) * 60 : 12 * 60;
	*headS      = sCtl.crHeadS ? sCtl.crHeadS : 15;
	*pick       = sCtl.crPick ? sCtl.crPick - 1 : -1;
}
extern "C" int port_mp_freeze_death_out(void) { return sCtl.freezeDeathOut ? 1 : 0; }
extern "C" void port_mp_hide_settings(int* hideS, int* seekS, int* convert, int* pick)
{
	*hideS   = sCtl.hideS ? (sCtl.hideS < 10 ? 10 : sCtl.hideS) : 30;
	*seekS   = sCtl.seekS ? sCtl.seekS : 180;
	*convert = sCtl.hideNoConvert ? 0 : 1;
	*pick    = sCtl.hidePick ? sCtl.hidePick - 1 : -1;
}
namespace {

void hostCancel()
{
	sCtl.phase = sCtl.runningMode == PORT_MP_MODE_NONE ? PH_WAITING : PH_IDLE;
	sCtl.cycle = 0;
	setTimer(0);
	std::memset(sCtl.votes, 0xFF, sizeof(sCtl.votes));
	sCtlDirty = true;
}

void sendCtlState()
{
	const double left = sDeadline - port_mp_time_ms();
	sCtl.leftMs       = sDeadline > 0.0 ? (u32)(left > 1.0 ? left : 1.0) : 0;
	port_mp_send(kCtlChannel, &sCtl, (int)sizeof(sCtl), 1, -1);
	sLastCtlSend = port_mp_time_ms();
	sCtlDirty    = false;
}

void ctlFrame()
{
	if (!port_mp_active())
		return;
	const bool host = port_mp_is_host() != 0;
	// receive: the host's state (players), the players' votes (host)
	unsigned char buf[sizeof(CtlState) + 8];
	int from, n;
	while ((n = port_mp_recv(kCtlChannel, buf, (int)sizeof(buf), &from)) > 0) {
		if (buf[0] == MSG_CTL_STATE && n == (int)sizeof(CtlState) && !host) {
			std::memcpy(&sCtl, buf, sizeof(sCtl));
			sDeadline = sCtl.leftMs > 0 ? port_mp_time_ms() + sCtl.leftMs : 0.0;
		} else if (buf[0] == MSG_CTL_VOTE && n == (int)sizeof(CtlVote) && host && from >= 0
		           && from < PORT_MP_MAX_PLAYERS && sCtl.phase == PH_VOTING) {
			const u8 m       = ((const CtlVote*)buf)->mode;
			sCtl.votes[from] = m < PORT_MP_MODE_COUNT ? m : 0xFF;
			sCtlDirty        = true;
		}
	}
	if (!host)
		return;
	// SMS_PORT_MP_CTL="limit=15,auto=1,vote=12,wait=0,start=0,queue@300" (host, tests): settings,
	// then at that game frame queue Free and begin a cycle.
	static const char* sCtlEnv = std::getenv("SMS_PORT_MP_CTL");
	static int sCtlEnvFrames;
	if (sCtlEnv != nullptr) {
		if (sCtlEnvFrames == 0) {
			int v;
			if (const char* p = std::strstr(sCtlEnv, "limit="))
				sCtl.freeTimeLimitS = (u16)((v = std::atoi(p + 6)) > 0 ? clampTimer(v) : 0);
			if (const char* p = std::strstr(sCtlEnv, "auto="))
				sCtl.autoStart = std::atoi(p + 5) > 0;
			if (const char* p = std::strstr(sCtlEnv, "vote="))
				sCtl.voteEnabled = (v = std::atoi(p + 5)) > 0, sCtl.voteS = (u16)clampTimer(v);
			if (const char* p = std::strstr(sCtlEnv, "wait="))
				sCtl.waitS = (u16)((v = std::atoi(p + 5)) > 0 ? clampTimer(v) : 0);
			if (const char* p = std::strstr(sCtlEnv, "start="))
				sCtl.startS = (u16)std::atoi(p + 6);
		}
		const char* at = std::strstr(sCtlEnv, "queue@");
		if (++sCtlEnvFrames == (at != nullptr ? std::atoi(at + 6) : -1)) {
			std::fprintf(stderr, "[mp] SMS_PORT_MP_CTL: queue Free, begin\n");
			enqueue(PORT_MP_MODE_FREE, false);
			hostBeginCycle();
		}
	}
	// votes of players who left no longer count
	for (int i = 0; i < PORT_MP_MAX_PLAYERS; i++) {
		PortMpPlayerInfo p;
		if (sCtl.votes[i] != 0xFF && !(port_mp_get_player(i, &p) && p.used)) {
			sCtl.votes[i] = 0xFF;
			sCtlDirty     = true;
		}
	}
	// Waiting without a timer goes on once a mode is queued (during a cycle)
	if (sCtl.phase == PH_WAITING && sDeadline == 0.0 && sCtl.cycle && sCtl.queueLen > 0)
		hostArmWaiting(false);
	if (sDeadline > 0.0 && port_mp_time_ms() >= sDeadline) {
		switch (sCtl.phase) {
		case PH_VOTING: hostEndVoting(); break;
		case PH_WAITING: hostEnterStarting(); break;
		case PH_STARTING: hostBeginMode(sCtl.pendingMode); break;
		case PH_RUNNING: hostEndMode(); break;
		default: setTimer(0); break;
		}
	}
	if (sCtlDirty || port_mp_time_ms() - sLastCtlSend > 1000.0)
		sendCtlState();
}

// ---- lobby news on the plaza's D.E.B.S. ticker ---------------------------------------------
// Every client watches the shared mode state (sCtl, the session settings, the player list) and
// turns its changes into news lines; GCConsole2.cpp scrolls them on the plaza's news ticker in
// place of the game's own stories (port_mp_news_next). With nothing new, a lobby status line
// comes round every kNewsStatusMs.
const double kNewsStatusMs = 45000.0;
const int kNewsMax         = 8;
char sNews[kNewsMax][200];
int sNewsCount;
double sNewsLastShown;

void newsPost(const char* fmt, ...)
{
	if (sNewsCount >= kNewsMax) { // drop the oldest
		std::memmove(sNews[0], sNews[1], sizeof(sNews[0]) * (kNewsMax - 1));
		sNewsCount = kNewsMax - 1;
	}
	va_list ap;
	va_start(ap, fmt);
	std::vsnprintf(sNews[sNewsCount], sizeof(sNews[0]), fmt, ap);
	va_end(ap);
	std::fprintf(stderr, "[mp-news] %s\n", sNews[sNewsCount]);
	sNewsCount++;
}

// What happens next, in a few words (for the status line).
void newsNextLine(char* out, size_t cap)
{
	const int left = secondsLeft();
	switch (sCtl.phase) {
	case PH_VOTING: std::snprintf(out, cap, "Voting is open for %d more seconds: press F2 to vote!", left); return;
	case PH_WAITING:
		if (sDeadline > 0.0)
			std::snprintf(out, cap, "%s starts in about %d seconds.", modeName(nextMode(false)), left);
		else
			std::snprintf(out, cap, "Waiting for the host to pick the next game mode.");
		return;
	case PH_STARTING: std::snprintf(out, cap, "%s starts in %d seconds!", modeName(sCtl.pendingMode), left); return;
	case PH_RUNNING:
		std::snprintf(out, cap, "%s is on: %d:%02d left.", modeName(sCtl.runningMode), left / 60, left % 60);
		return;
	default: out[0] = '\0'; return;
	}
}

// Every frame on every client: compare with last frame, post what changed.
void newsFrame()
{
	static bool sWasActive;
	static CtlState sPrev;
	static u8 sPrevMode;
	static bool sPrevUsed[PORT_MP_MAX_PLAYERS];
	static char sPrevName[PORT_MP_MAX_PLAYERS][PORT_MP_NAME_MAX];
	if (!port_mp_active()) {
		sWasActive  = false;
		sNewsCount  = 0;
		return;
	}
	PortMpSessionSettings set;
	port_mp_get_settings(&set);
	if (!sWasActive) { // a new session: start from what is there, with a welcome
		sWasActive = true;
		sPrev      = sCtl;
		sPrevMode  = set.gameMode;
		for (int i = 0; i < PORT_MP_MAX_PLAYERS; i++) {
			PortMpPlayerInfo p;
			sPrevUsed[i] = port_mp_get_player(i, &p) && p.used;
			std::snprintf(sPrevName[i], sizeof(sPrevName[i]), "%s", sPrevUsed[i] ? p.name : "");
		}
		newsPost("Welcome to the lobby \"%s\"! The host picks the game modes (F2: the lobby panel).",
		         port_mp_lobby_name());
		sNewsLastShown = port_mp_time_ms();
		return;
	}
	// players
	for (int i = 0; i < PORT_MP_MAX_PLAYERS; i++) {
		PortMpPlayerInfo p;
		const bool used = port_mp_get_player(i, &p) && p.used;
		if (used && !sPrevUsed[i])
			newsPost("%s has joined the lobby!", p.name);
		else if (!used && sPrevUsed[i])
			newsPost("%s has left the lobby.", sPrevName[i]);
		sPrevUsed[i] = used;
		if (used)
			std::snprintf(sPrevName[i], sizeof(sPrevName[i]), "%s", p.name);
	}
	// the mode control's phases
	if (sCtl.phase != sPrev.phase) {
		switch (sCtl.phase) {
		case PH_VOTING:
			newsPost("Voting has begun! Press F2 and vote for the next game mode (%d seconds).", (int)sCtl.voteS);
			break;
		case PH_WAITING:
			if (sPrev.phase == PH_VOTING && sCtl.queueLen > 0)
				newsPost("The votes are in: %s is next!", modeName(sCtl.queue[0]));
			else if (sCtl.queueLen > 0)
				newsPost("Next up: %s. Get ready!", modeName(sCtl.queue[0]));
			break;
		case PH_STARTING:
			if (sPrev.phase == PH_VOTING) // no waiting phase: straight from the vote
				newsPost("The votes are in: %s wins!", modeName(sCtl.pendingMode));
			if (sCtl.startS > 0)
				newsPost("%s starts in %d seconds!", modeName(sCtl.pendingMode), (int)sCtl.startS);
			break;
		default: break;
		}
	}
	// the queue grew (outside a vote's result)
	if (sCtl.queueLen > sPrev.queueLen && sCtl.phase != PH_VOTING && !(sCtl.phase != sPrev.phase)) {
		char list[120] = "";
		for (int i = 0; i < sCtl.queueLen && i < 4; i++) {
			const size_t l = std::strlen(list);
			std::snprintf(list + l, sizeof(list) - l, "%s%s", i ? ", then " : "", modeName(sCtl.queue[i]));
		}
		newsPost("Game mode queue: %s.", list);
	}
	// the game mode itself
	if (set.gameMode != sPrevMode) {
		if (set.gameMode == PORT_MP_MODE_NONE)
			newsPost("The round is over. Everyone is back in the lobby!");
		else
			newsPost("%s has started! Good luck, everyone!", modeName(set.gameMode));
		sPrevMode = set.gameMode;
	}
	sPrev = sCtl;
}

// The line above the player list.
void modeHeadline(char* out, size_t cap)
{
	if (port_mp_hide_headline(out, (int)cap)) // a Hide and Seek round is under way
		return;
	if (port_mp_freeze_headline(out, (int)cap)) // a Freeze Tag round
		return;
	if (port_mp_prop_headline(out, (int)cap)) // a Prop Hunt round
		return;
	if (port_mp_manhunt_headline(out, (int)cap)) // a Manhunt round
		return;
	if (port_mp_shinerun_headline(out, (int)cap)) // a Shine Runners round
		return;
	if (port_mp_cnr_headline(out, (int)cap)) // a Cops and Robbers round
		return;
	const int left = secondsLeft();
	switch (sCtl.phase) {
	case PH_VOTING: std::snprintf(out, cap, "Voting: %d  (F2 to vote)", left); return;
	case PH_WAITING:
		if (sDeadline > 0.0)
			std::snprintf(out, cap, "Waiting: %d  (next: %s)", left, modeName(nextMode(false)));
		else
			std::snprintf(out, cap, "Waiting for a game mode");
		return;
	case PH_STARTING: std::snprintf(out, cap, "%s starts in %d", modeName(sCtl.pendingMode), left); return;
	case PH_RUNNING:
		std::snprintf(out, cap, "Game mode: %s  %d:%02d", modeName(sCtl.runningMode), left / 60, left % 60);
		return;
	default: {
		if (port_mp_hide_headline(out, (int)cap))
			return;
		PortMpSessionSettings s;
		port_mp_get_settings(&s);
		std::snprintf(out, cap, "Game mode: %s", modeName(s.gameMode));
		return;
	}
	}
}

// ---- player list ----------------------------------------------------------------------------
class TMpPlayerList : public JDrama::TViewObj {
public:
	TMpPlayerList()
	    : JDrama::TViewObj("MP player list")
	{
	}
	virtual void perform(u32 cue, JDrama::TGraphics* graphics);
};

alignas(16) unsigned char sListMem[sizeof(TMpPlayerList)];
TMpPlayerList* sList;

// One colour per player slot, so every name in the list differs (caps are often all red).
JUtility::TColor slotColour(int slot)
{
	const uint32_t rgb = port_mp_tag_rgb(slot); // the player's name-tag colour (Customize)
	return JUtility::TColor((u8)(rgb >> 16), (u8)(rgb >> 8), (u8)rgb, 255);
}

const char* stageName(int stage)
{
	switch (stage) {
	case 0: return "Airstrip";
	case 1: return "Delfino Plaza";
	case 2: return "Bianco Hills";
	case 3: return "Ricco Harbor";
	case 4: return "Gelato Beach";
	case 5: return "Pinna Park";
	case 6: return "Sirena Beach";
	case 7: return "Hotel Delfino";
	case 8: return "Pianta Village";
	case 9: return "Noki Bay";
	case 13: return "Pinna Park";
	case 15: return "File select";
	case 52: return "Corona Mountain";
	case 60: return "Bowser";
	default: return stage >= 20 && stage < 60 ? "Secret course" : nullptr;
	}
}

// ---- drawing helpers for the list (frame coordinates, 640 x 480) -----------------------------
const int kListKey = 43; // SDL_SCANCODE_TAB (held: the player list)

float textWidth(const char* text, int size)
{
	float w = 0.0f; // the glyph advances, scaled from the font's own size to ours
	for (const char* c = text; *c != '\0'; c++)
		w += (float)((JUTFont*)gpSystemFont)->getWidth((u8)*c);
	return w * (float)size / (float)(gpSystemFont->getWidth() > 0 ? gpSystemFont->getWidth() : size);
}

// Game-font text with a dark copy one pixel down-right (as J2DTextBox::drawSelf sets it up).
void printText(const char* text, float x, float y, int size, JUtility::TColor col)
{
	for (int pass = 0; pass < 2; pass++) {
		const JUtility::TColor c = pass == 0 ? JUtility::TColor(0, 0, 0, 200) : col;
		J2DPrint print(gpSystemFont, 0, 0, c, c);
		print.setFontSize(size, size);
		print.setSomeColors(JUtility::TColor(0, 0, 0, 0), JUtility::TColor(255, 255, 255, 255));
		print.initiate();
		Mtx id;
		MTXIdentity(id);
		GXLoadPosMtxImm(id, GX_PNMTX0);
		GXSetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE); // over everything
		GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_NOOP);
		print.print((int)x + (pass == 0 ? 1 : 0), (int)y + (pass == 0 ? 1 : 0), "%s", text);
	}
}

// A flat (blended) box.
void fillQuad(float x0, float y0, float x1, float y1, GXColor col)
{
	GXClearVtxDesc();
	GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
	GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
	GXSetNumChans(1);
	GXSetChanCtrl(GX_COLOR0A0, GX_FALSE, GX_SRC_REG, GX_SRC_REG, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
	GXSetChanMatColor(GX_COLOR0A0, col);
	GXSetNumTexGens(0);
	GXSetNumTevStages(1);
	GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
	GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
	GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_NOOP);
	GXSetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
	GXSetCullMode(GX_CULL_NONE);
	Mtx id;
	MTXIdentity(id);
	GXLoadPosMtxImm(id, GX_PNMTX0);
	GXSetCurrentMtx(GX_PNMTX0);
	GXBegin(GX_QUADS, GX_VTXFMT0, 4);
	GXPosition3f32(x0, y0, 0.0f);
	GXPosition3f32(x1, y0, 0.0f);
	GXPosition3f32(x1, y1, 0.0f);
	GXPosition3f32(x0, y1, 0.0f);
	GXEnd();
}

void TMpPlayerList::perform(u32 cue, JDrama::TGraphics* graphics)
{
	if (!(cue & CUE_DRAW) || !port_mp_active() || gpSystemFont == nullptr)
		return;
	if (gpMarDirector == nullptr || gpMarDirector->mState != TMarDirector::STATE_UNK4)
		return; // not over pause menus, talks, demos
	PortMpPlayerInfo players[PORT_MP_MAX_PLAYERS];
	int slots[PORT_MP_MAX_PLAYERS];
	int n = 0;
	for (int s = 0; s < PORT_MP_MAX_PLAYERS; s++)
		if (port_mp_get_player(s, &players[n]) && players[n].used)
			slots[n++] = s;
	if (n == 0)
		return;

	J2DOrthoGraph graph(graphics->getViewport());
	graph.setup2D();
	// Widescreen: J2D squeezes the 640-wide frame into the middle 4:3 of the picture; draw on
	// the whole picture instead (frame x from 320 - 320 s to 320 + 320 s), at the HUD's right
	// edge (w11's HUD position setting).
	const float scale = PORTWideScale();
	{ // (also at 4:3: the frame's own 0..640 x 0..480 over the game viewport)
		const JUTRect& vp = graphics->getViewport();
		GXSetViewport((f32)vp.x1, (f32)vp.y1, (f32)(vp.x2 - vp.x1), (f32)(vp.y2 - vp.y1), 0.0f, 1.0f);
		GXSetScissor(vp.x1, vp.y1, vp.x2 - vp.x1, vp.y2 - vp.y1);
		Mtx44 proj;
		C_MTXOrtho(proj, 0.0f, 480.0f, 320.0f - 320.0f * scale, 320.0f + 320.0f * scale, -1.0f, 1.0f);
		GXSetProjection(proj, GX_ORTHOGRAPHIC);
	}
	if (port_mp_hide_blind() || port_mp_prop_blind()) { // the seeker / hunter sees nothing while the others hide
		GXClearVtxDesc();
		GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
		GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
		GXSetNumChans(1);
		GXSetChanCtrl(GX_COLOR0A0, GX_FALSE, GX_SRC_REG, GX_SRC_REG, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
		GXSetChanMatColor(GX_COLOR0A0, (GXColor) { 0, 0, 0, 255 });
		GXSetNumTexGens(0);
		GXSetNumTevStages(1);
		GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
		GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
		GXSetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_NOOP);
		GXSetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
		GXSetCullMode(GX_CULL_NONE);
		Mtx id;
		MTXIdentity(id);
		GXLoadPosMtxImm(id, GX_PNMTX0);
		GXSetCurrentMtx(GX_PNMTX0);
		const float x0 = 320.0f - 320.0f * scale - 2.0f, x1 = 320.0f + 320.0f * scale + 2.0f;
		GXBegin(GX_QUADS, GX_VTXFMT0, 4);
		GXPosition3f32(x0, -2.0f, 0.0f);
		GXPosition3f32(x1, -2.0f, 0.0f);
		GXPosition3f32(x1, 482.0f, 0.0f);
		GXPosition3f32(x0, 482.0f, 0.0f);
		GXEnd();
	}
	// The session line (the vote / countdown / round, Hide and Seek's status) at the bottom
	// centre, above the D.E.B.S. ticker; the player list only while Tab is held.
	char headline[PORT_MP_NAME_MAX + 64];
	modeHeadline(headline, sizeof(headline));
	JUtility::TColor headCol(255, 255, 255, 255);
	if (sCtl.phase == PH_VOTING || sCtl.phase == PH_STARTING || (sCtl.phase == PH_WAITING && sDeadline > 0.0))
		headCol = JUtility::TColor(255, 230, 90, 255); // a countdown stands out
	if (headline[0] != '\0')
		printText(headline, 320.0f - textWidth(headline, 16) * 0.5f, 400.0f, 16, headCol); // baseline
	if (!port_key_held(kListKey) || port_mp_menu_wants_input())
		return;

	// ---- the list: name | where | role (Hide and Seek), in a dark box in the middle
	const int size = 15;
	char where[PORT_MP_MAX_PLAYERS][48];
	const char* role[PORT_MP_MAX_PLAYERS];
	float wName = textWidth("Player", size), wWhere = 0.0f, wRole = 0.0f;
	bool anyRole = false;
	for (int i = 0; i < n; i++) {
		const PortMpPlayerInfo& p = players[i];
		const char* st = p.stage != 0xFF ? stageName(p.stage) : nullptr;
		if (st != nullptr && p.stage != 1 && p.stage != 15)
			std::snprintf(where[i], sizeof(where[i]), "%s %d", st, p.scenario + 1); // the episode as the game counts it
		else
			std::snprintf(where[i], sizeof(where[i]), "%s", st != nullptr ? st : "Loading");
		role[i] = port_mp_hide_role(slots[i]); // Hide and Seek: Hider / Seeker / ...
		if (role[i] == nullptr)
			role[i] = port_mp_freeze_role(slots[i]); // Freeze Tag: Runner / Tagger / Frozen
		if (role[i] == nullptr)
			role[i] = port_mp_prop_role(slots[i]); // Prop Hunt: Hider / Hunter / Out
		if (role[i] == nullptr)
			role[i] = port_mp_manhunt_role(slots[i]); // Manhunt: Runner / Shadow Mario
		if (role[i] == nullptr)
			role[i] = port_mp_shinerun_role(slots[i]); // Shine Runners: Shines held
		if (role[i] == nullptr)
			role[i] = port_mp_cnr_role(slots[i]); // Cops and Robbers: Cop / Robber / In prison
		if (port_mp_hide_hide_tag(slots[i]) || port_mp_prop_hide_tag(slots[i]))
			std::snprintf(where[i], sizeof(where[i]), "?"); // where a hider is stays hidden from seekers
		anyRole |= role[i] != nullptr;
		const float a = textWidth(p.name, size), b = textWidth(where[i], size);
		wName  = a > wName ? a : wName;
		wWhere = b > wWhere ? b : wWhere;
		if (role[i] != nullptr) {
			const float c = textWidth(role[i], size);
			wRole = c > wRole ? c : wRole;
		}
	}
	const float gap = 24.0f, pad = 14.0f, row = size + 6.0f;
	char title[64];
	std::snprintf(title, sizeof(title), "Players %d", n);
	PortMpSessionSettings set;
	port_mp_get_settings(&set);
	char mode[64];
	const int shown = sCtl.phase == PH_RUNNING ? sCtl.runningMode : set.gameMode;
	std::snprintf(mode, sizeof(mode), "%s", shown == PORT_MP_MODE_NONE ? "Lobby" : modeName(shown));
	float wTable = wName + gap + wWhere + (anyRole ? gap + wRole : 0.0f);
	const float wHead = textWidth(title, size + 2) + gap + textWidth(mode, size + 2);
	wTable = wHead > wTable ? wHead : wTable;
	const float x0 = 320.0f - wTable * 0.5f - pad, x1 = 320.0f + wTable * 0.5f + pad;
	// (J2DPrint's y is the text's baseline)
	const float y0 = 110.0f, titleBase = y0 + pad + size + 2, line = titleBase + 6.0f;
	const float y1 = line + 4.0f + row * n + pad - 4.0f;
	fillQuad(x0, y0, x1, y1, (GXColor) { 0, 0, 40, 170 });
	fillQuad(x0 + 6.0f, line, x1 - 6.0f, line + 1.0f, (GXColor) { 255, 255, 255, 120 });
	printText(title, x0 + pad, titleBase, size + 2, JUtility::TColor(255, 230, 90, 255));
	printText(mode, x1 - pad - textWidth(mode, size + 2), titleBase, size + 2, JUtility::TColor(255, 255, 255, 255));
	float y = line + 4.0f + size;
	for (int i = 0; i < n; i++) {
		float x = x0 + pad;
		printText(players[i].name, x, y, size, slotColour(slots[i]));
		x += wName + gap;
		printText(where[i], x, y, size, JUtility::TColor(220, 230, 255, 255));
		x += wWhere + gap;
		if (role[i] != nullptr) {
			const bool seeker = role[i][0] == 'S' || role[i][0] == 's';
			printText(role[i], x, y, size, seeker ? JUtility::TColor(255, 120, 90, 255) : JUtility::TColor(120, 230, 140, 255));
		}
		y += row;
	}
}

struct Register {
	Register() { port_mp_register_game_frame(gameFrame); }
} sRegister;

} // namespace

extern "C" {

// Game mode None shows a locked plaza: the session's save (every Shine, from free mode) is set
// aside and the plaza loads from a copy without the Shines, the painted M portals and the opened
// pipes, so the M portals and warp pipes are gone, the Corona Mountain gate is shut and the
// Pinna Park cannon is still being repaired. Starting Free puts the full save back (with
// anything collected since). Called before every stage load in a session (port_mp_game.cpp,
// port_mp_redirect_title); the scene setup reads these flags.
namespace {
// The whole TFlagManager (its arrays are private): copied at a stage-load boundary, where the
// stage flags are reset anyway.
struct SavedFlags {
	alignas(8) unsigned char bytes[sizeof(TFlagManager)];
};
SavedFlags sFullSave;
bool sLockedView; // the flags are a modified copy (sViewKind); sFullSave holds the real ones
int sViewKind;    // 1 = the locked plaza (None, a match in the plaza), 2 = Manhunt (no Shines taken)

void saveFlags(SavedFlags& s, const TFlagManager* f) { std::memcpy(s.bytes, f, sizeof(s.bytes)); }
void loadFlags(TFlagManager* f, const SavedFlags& s) { std::memcpy((void*)f, s.bytes, sizeof(s.bytes)); }
} // namespace

// The plaza scenario for the running mode (stageArc.bin: scenario 2 = dolpic10, the end-game
// plaza). Every mode uses it; where the plaza must hold everyone (None, a match in the plaza) its
// entrances and warps do nothing instead (port_mp_plaza_exit_blocked).
int port_mp_plaza_scenario(const PortMpSessionSettings* s) { return s->startStage ? s->startScenario : 2; }

// None and a match in the plaza keep everyone in the plaza; all levels needs every entrance, and a
// match in one level does not show the plaza at all.
static bool lockedPlazaView(const PortMpSessionSettings& s)
{
	if (s.gameMode == PORT_MP_MODE_NONE)
		return true;
	if (s.gameMode != PORT_MP_MODE_HIDE && s.gameMode != PORT_MP_MODE_FREEZE && s.gameMode != PORT_MP_MODE_PROP)
		return false;
	int stage, scen;
	port_mp_home_area(&s, &stage, &scen);
	return !port_mp_arena_all_levels(&s) && stage == 1;
}

void port_mp_mode_flags_for_load(int stage)
{
	TFlagManager* f = TFlagManager::getInstance();
	if (f == nullptr)
		return;
	PortMpSessionSettings s;
	port_mp_get_settings(&s);
	const int want = !port_mp_active() ? 0
	                 : s.gameMode == PORT_MP_MODE_MANHUNT || s.gameMode == PORT_MP_MODE_SHINERUN
	                         || s.gameMode == PORT_MP_MODE_CNR
	                     ? 2
	                                                        : 0;
	if (sLockedView && sViewKind != want) { // Free (or the session ended, or another view): the real save again
		loadFlags(f, sFullSave);
		sLockedView = false;
		std::fprintf(stderr, "[mp] game mode: full save back (shines %d)\n", (int)f->getFlag(MSF_SHINE_COUNT));
	}
	if (want == 0 || sLockedView)
		return; // the real save, or already the wanted copy (and the full one is set aside)
	saveFlags(sFullSave, f);
	sLockedView = true;
	sViewKind   = want;
	for (u32 flag = MSF_SHINE_BIANCO_1; flag <= MSF_SHINE_CORONA; flag++)
		f->setBool(false, flag);
	if (want == 2) {
		// Manhunt, Shine Runners: every Shine Sprite collectible again (the episode selects start over); the
		// portals, pipes and everything else of the finished save stay open
		f->setFlag(MSF_SHINE_COUNT, 0);
		std::fprintf(stderr, "[mp] Manhunt: the save without its Shine Sprites (stage %d)\n", stage);
		return;
	}
}

// The plaza holds everyone now (None, or a match in the plaza): our Mario is in the plaza.
static bool plazaLocked()
{
	if (!port_mp_active() || SMSGetApplication()->mCurrArea.getStage() != 1 || gpMarDirector == nullptr)
		return false;
	PortMpSessionSettings s;
	port_mp_get_settings(&s);
	return lockedPlazaView(s);
}

// Game hooks where Mario starts a trip out (the painted gates' warpIn, warp floors and pipes):
// 1 = it does not start, he stays where he is. (Refusing in TMarDirector::setNextStage, after a
// gate or event has begun its transition, crashed some of them: that one only gets a 0.)
int port_mp_stage_change_blocked(int stage)
{
	(void)stage;
	return 0;
}
extern "C" int port_mp_plaza_exit_blocked(void)
{
	if (!plazaLocked())
		return 0;
	static double sToastAt;
	if (port_mp_time_ms() - sToastAt > 8000.0) {
		sToastAt = port_mp_time_ms();
		port_mp_toast("The plaza's exits are closed in this game mode.");
	}
	return 1;
}

// Hide and Seek / Freeze Tag: where the match is played (port_mp_arena.cpp). `runner` names the
// players the exit lock holds.
static void arenaSettings(u8& allLevels, u8& level, u8& lockS, const char* runner)
{
	static const char* const kMaps[] = { "Single level", "All levels" };
	int map = allLevels ? 1 : 0;
	ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
	if (ImGui::Combo("Map", &map, kMaps, 2)) {
		allLevels = (u8)map;
		sCtlDirty = true;
	}
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Single level: the whole match is played in one level, with no way out.\n"
		                  "All levels: everyone starts in Delfino Plaza and may go into any level\n"
		                  "(no episode select: each level loads one plain episode, no Shines).");
	if (!allLevels) {
		int lv = level < port_mp_arena_level_count() ? level : 0;
		ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
		if (ImGui::BeginCombo("Level", port_mp_arena_level_name(lv))) {
			for (int i = 0; i < port_mp_arena_level_count(); i++)
				if (ImGui::Selectable(port_mp_arena_level_name(i), i == lv)) {
					level     = (u8)i;
					sCtlDirty = true;
				}
			ImGui::EndCombo();
		}
		return;
	}
	int lock = lockS ? lockS - 1 : kDefaultLockS;
	ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
	if (ImGui::SliderInt("Exit lock", &lock, 0, 180, lock ? "%d s" : "off")) {
		lockS     = (u8)(lock + 1);
		sCtlDirty = true;
	}
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Each time a %s enters a level, he cannot leave it (Exit Area, Go to plaza)\n"
		                  "for this long, so nobody slips out the moment a seeker comes in.",
		                  runner);
}

// Lobby panel section (port_mp_menu.cpp, F2): the cycle, the queue, voting and the selected
// mode's settings. Only the host edits; everyone votes during Voting.
void port_mp_modes_panel(int host)
{
	char head[96];
	modeHeadline(head, sizeof(head));
	ImGui::TextUnformatted(head);

	ImGui::Text("Queue:");
	if (sCtl.queueLen == 0) {
		ImGui::SameLine();
		ImGui::TextDisabled("(empty)");
	}
	for (int i = 0; i < sCtl.queueLen; i++) {
		ImGui::PushID(1000 + i);
		ImGui::SameLine();
		ImGui::Text("%d. %s", i + 1, modeName(sCtl.queue[i]));
		if (host) {
			ImGui::SameLine();
			if (ImGui::SmallButton("x")) {
				std::memmove(&sCtl.queue[i], &sCtl.queue[i + 1], sCtl.queueLen - i - 1);
				sCtl.queueLen--;
				sCtlDirty = true;
			}
		}
		ImGui::PopID();
	}

	const int me = port_mp_local_slot();
	if (sCtl.phase == PH_VOTING) {
		ImGui::TextColored(ImVec4(1.0f, 0.9f, 0.35f, 1.0f), "Vote for the next mode (%d s):", secondsLeft());
		for (int m = 1; m < PORT_MP_MODE_COUNT; m++) { // not None: that is the lobby itself
			int votes = 0;
			for (int i = 0; i < PORT_MP_MAX_PLAYERS; i++)
				votes += sCtl.votes[i] == m;
			const bool mine = me >= 0 && me < PORT_MP_MAX_PLAYERS && sCtl.votes[me] == m;
			char label[48];
			std::snprintf(label, sizeof(label), "%s%s (%d)##vote%d", mine ? "> " : "", modeName(m), votes, m);
			ImGui::SameLine();
			if (ImGui::Button(label) && me >= 0) {
				CtlVote v = { MSG_CTL_VOTE, (u8)(mine ? 0xFF : m) };
				if (host) {
					sCtl.votes[me] = v.mode;
					sCtlDirty      = true;
				} else {
					port_mp_send(kCtlChannel, &v, (int)sizeof(v), 1, -1);
				}
			}
		}
	} else if (sCtl.voteEnabled) {
		ImGui::TextDisabled("Voting opens when the next game mode is chosen.");
	}
	if (!host) {
		ImGui::TextDisabled("Only the host starts modes and changes their settings.");
		return;
	}

	ImGui::Separator();
	static int sPick = PORT_MP_MODE_FREE;
	ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
	ImGui::Combo("##modepick", &sPick, kModeShort, PORT_MP_MODE_COUNT);
	ImGui::SameLine();
	const bool busy = sCtl.phase == PH_VOTING || sCtl.phase == PH_STARTING
	                  || (sCtl.phase == PH_WAITING && (sDeadline > 0.0 || sCtl.cycle));
	if (busy) {
		if (ImGui::Button("Cancel"))
			hostCancel();
	} else if (ImGui::Button("Start")) {
		if (sPick == PORT_MP_MODE_NONE) {
			hostBeginMode(PORT_MP_MODE_NONE); // straight back to the lobby
			sCtl.phase = PH_WAITING;
		} else {
			enqueue(sPick, true);
			hostBeginCycle();
		}
	}
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Voting (if on), then Waiting, then Starting; then everyone goes back to the start of "
		                  "Delfino Plaza and the mode begins.");
	ImGui::SameLine();
	if (ImGui::Button("Add to queue"))
		enqueue(sPick, false);
	if (!busy && sCtl.queueLen > 0) {
		ImGui::SameLine();
		if (ImGui::Button("Start queue"))
			hostBeginCycle();
	}

	ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
	bool voting = sCtl.voteEnabled != 0;
	if (ImGui::Checkbox("Voting", &voting)) {
		sCtl.voteEnabled = voting;
		sCtlDirty        = true;
	}
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Players vote at the start of each cycle; the winning mode joins the queue.");
	if (voting) {
		ImGui::SameLine();
		int t = sCtl.voteS;
		ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
		if (ImGui::SliderInt("Voting time (s)", &t, kMinTimerS, 120)) {
			sCtl.voteS = (u16)clampTimer(t);
			sCtlDirty  = true;
		}
	}
	bool waitOn = sCtl.waitS > 0;
	if (ImGui::Checkbox("Waiting timer", &waitOn)) {
		sCtl.waitS = waitOn ? 30 : 0;
		sCtlDirty  = true;
	}
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Off: after a vote, Waiting is skipped; without voting it lasts 10 s.\n"
		                  "Nothing queued: Waiting lasts until a mode is queued.");
	if (waitOn) {
		ImGui::SameLine();
		int t = sCtl.waitS;
		ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
		if (ImGui::SliderInt("Waiting time (s)", &t, kMinTimerS, 300)) {
			sCtl.waitS = (u16)clampTimer(t);
			sCtlDirty  = true;
		}
	}
	int startT = sCtl.startS;
	ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
	if (ImGui::SliderInt("Starting countdown (s)", &startT, 0, 60)) {
		sCtl.startS = (u16)startT;
		sCtlDirty   = true;
	}
	bool autoStart = sCtl.autoStart != 0;
	if (ImGui::Checkbox("AutoStart", &autoStart)) {
		sCtl.autoStart = autoStart;
		sCtlDirty      = true;
	}
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("When a timed mode ends, the next cycle (Voting / Waiting / Starting) begins by itself.");

	// the selected mode's own settings
	ImGui::Separator();
	ImGui::Text("%s mode settings", modeName(sPick));
	if (sPick == PORT_MP_MODE_FREE) {
		bool limited = sCtl.freeTimeLimitS > 0;
		if (ImGui::Checkbox("Time limit", &limited)) {
			sCtl.freeTimeLimitS = limited ? 60 : 0;
			sCtlDirty           = true;
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Off: Free mode runs until the host starts something else.\n"
			                  "On: when the time is up, everyone goes back to the lobby.");
		if (limited) {
			ImGui::SameLine();
			int secs = sCtl.freeTimeLimitS;
			ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
			if (ImGui::SliderInt("##freelimit", &secs, kMinTimerS, 3600, "%d s")) {
				sCtl.freeTimeLimitS = (u16)clampTimer(secs);
				sCtlDirty           = true;
			}
			ImGui::SameLine();
			ImGui::Text("(%d:%02d)", sCtl.freeTimeLimitS / 60, sCtl.freeTimeLimitS % 60);
		}
	} else if (sPick == PORT_MP_MODE_HIDE) {
		int hide = sCtl.hideS ? sCtl.hideS : 30, seek = sCtl.seekS ? sCtl.seekS : 180;
		ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
		if (ImGui::SliderInt("Hide time", &hide, 10, 300, "%d s")) {
			sCtl.hideS = (u16)(hide < 10 ? 10 : hide);
			sCtlDirty  = true;
		}
		ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
		if (ImGui::SliderInt("Seek time", &seek, 30, 1800, "%d s")) {
			sCtl.seekS = (u16)seek;
			sCtlDirty  = true;
		}
		bool convert = !sCtl.hideNoConvert;
		if (ImGui::Checkbox("Tagged players become seekers", &convert)) {
			sCtl.hideNoConvert = !convert;
			sCtlDirty          = true;
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Off: a tagged hider is out and watches.\nA hider who dies counts as tagged.");
		char cur[48] = "Random";
		PortMpPlayerInfo pi;
		if (sCtl.hidePick && port_mp_get_player(sCtl.hidePick - 1, &pi) && pi.used)
			std::snprintf(cur, sizeof(cur), "%s", pi.name);
		ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
		if (ImGui::BeginCombo("Seeker", cur)) {
			if (ImGui::Selectable("Random", sCtl.hidePick == 0)) {
				sCtl.hidePick = 0;
				sCtlDirty     = true;
			}
			for (int i = 0; i < PORT_MP_MAX_PLAYERS; i++)
				if (port_mp_get_player(i, &pi) && pi.used) {
					ImGui::PushID(2000 + i);
					if (ImGui::Selectable(pi.name, sCtl.hidePick == i + 1)) {
						sCtl.hidePick = (u8)(i + 1);
						sCtlDirty     = true;
					}
					ImGui::PopID();
				}
			ImGui::EndCombo();
		}
		arenaSettings(sCtl.hideAllLevels, sCtl.hideLevel, sCtl.hideLockS, "hider");
	} else if (sPick == PORT_MP_MODE_FREEZE) {
		int ready = sCtl.freezeReadyS ? sCtl.freezeReadyS : 10, play = sCtl.freezePlayS ? sCtl.freezePlayS : 180;
		int taggers = sCtl.freezeTaggers ? sCtl.freezeTaggers : 1;
		ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
		if (ImGui::SliderInt("Head start", &ready, 3, 60, "%d s")) {
			sCtl.freezeReadyS = (u16)ready;
			sCtlDirty         = true;
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("The taggers cannot move while the runners scatter.");
		ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
		if (ImGui::SliderInt("Round time", &play, 30, 1800, "%d s")) {
			sCtl.freezePlayS = (u16)play;
			sCtlDirty        = true;
		}
		ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
		if (ImGui::SliderInt("Taggers", &taggers, 1, PORT_MP_MAX_PLAYERS - 1)) {
			sCtl.freezeTaggers = (u8)taggers;
			sCtlDirty          = true;
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("How many players are it. At least one player always runs.");
		char cur[48] = "Random";
		PortMpPlayerInfo pi;
		if (sCtl.freezePick && port_mp_get_player(sCtl.freezePick - 1, &pi) && pi.used)
			std::snprintf(cur, sizeof(cur), "%s", pi.name);
		ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
		if (ImGui::BeginCombo("Tagger", cur)) {
			if (ImGui::Selectable("Random", sCtl.freezePick == 0)) {
				sCtl.freezePick = 0;
				sCtlDirty       = true;
			}
			for (int i = 0; i < PORT_MP_MAX_PLAYERS; i++)
				if (port_mp_get_player(i, &pi) && pi.used) {
					ImGui::PushID(3000 + i);
					if (ImGui::Selectable(pi.name, sCtl.freezePick == i + 1)) {
						sCtl.freezePick = (u8)(i + 1);
						sCtlDirty       = true;
					}
					ImGui::PopID();
				}
			ImGui::EndCombo();
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("One tagger you choose (the others, if more, are picked at random).");
		bool deathTags = !sCtl.freezeDeathOut;
		if (ImGui::Checkbox("Runners who die become taggers", &deathTags)) {
			sCtl.freezeDeathOut = !deathTags;
			sCtlDirty           = true;
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Off: a runner who dies (a lost life) is out and watches.");
		arenaSettings(sCtl.freezeAllLevels, sCtl.freezeLevel, sCtl.freezeLockS, "runner");
		ImGui::TextDisabled("Tagged runners freeze in place; other runners touch them to thaw them.");
	} else if (sPick == PORT_MP_MODE_PROP) {
		int hide = sCtl.propHideS ? sCtl.propHideS : 45, hunt = sCtl.propHuntS ? sCtl.propHuntS : 240;
		int dmg = sCtl.propDamage ? sCtl.propDamage : 2, water = sCtl.propWaterMul ? sCtl.propWaterMul : 4;
		ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
		if (ImGui::SliderInt("Hide time##prop", &hide, 10, 300, "%d s")) {
			sCtl.propHideS = (u16)hide;
			sCtlDirty      = true;
		}
		ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
		if (ImGui::SliderInt("Hunt time", &hunt, 30, 1800, "%d s")) {
			sCtl.propHuntS = (u16)hunt;
			sCtlDirty      = true;
		}
		ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
		if (ImGui::SliderInt("Spray damage", &dmg, 1, 8, "%d of 8")) {
			sCtl.propDamage = (u8)dmg;
			sCtlDirty       = true;
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Health a hider loses per spray hit (Mario has 8). At 0 he is caught.");
		ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
		if (ImGui::SliderInt("Hunter water use", &water, 1, 10, "%dx")) {
			sCtl.propWaterMul = (u8)water;
			sCtlDirty         = true;
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("How much faster the hunters' FLUDD tanks empty.");
		bool convert = !sCtl.propNoConvert;
		if (ImGui::Checkbox("Caught hiders become hunters", &convert)) {
			sCtl.propNoConvert = !convert;
			sCtlDirty          = true;
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Off: a caught hider (out of health, or dead) is out and watches.");
		int taunt = sCtl.propTauntS == 255 ? 0 : sCtl.propTauntS ? sCtl.propTauntS : 30;
		ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
		if (ImGui::SliderInt("Hider sound every", &taunt, 0, 120, taunt == 0 ? "off" : "%d s")) {
			sCtl.propTauntS = (u8)(taunt == 0 ? 255 : taunt < 5 ? 5 : taunt);
			sCtlDirty       = true;
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("During the hunt, every hider plays a Shadow Mario sound where he is, this often\n"
			                  "(everyone hears it, from that spot). 0 = off.");
		char cur[48] = "Random";
		PortMpPlayerInfo pi;
		if (sCtl.propPick && port_mp_get_player(sCtl.propPick - 1, &pi) && pi.used)
			std::snprintf(cur, sizeof(cur), "%s", pi.name);
		ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
		if (ImGui::BeginCombo("Hunter", cur)) {
			if (ImGui::Selectable("Random", sCtl.propPick == 0)) {
				sCtl.propPick = 0;
				sCtlDirty     = true;
			}
			for (int i = 0; i < PORT_MP_MAX_PLAYERS; i++)
				if (port_mp_get_player(i, &pi) && pi.used) {
					ImGui::PushID(4000 + i);
					if (ImGui::Selectable(pi.name, sCtl.propPick == i + 1)) {
						sCtl.propPick = (u8)(i + 1);
						sCtlDirty     = true;
					}
					ImGui::PopID();
				}
			ImGui::EndCombo();
		}
		arenaSettings(sCtl.propAllLevels, sCtl.propLevel, sCtl.propLockS, "hider");
		ImGui::TextDisabled("Hiders: Shadow Mario, no FLUDD; R next to an object turns them into it.");
	} else if (sPick == PORT_MP_MODE_MANHUNT) {
		int goal = sCtl.mhGoal ? sCtl.mhGoal : 3, head = sCtl.mhHeadS ? sCtl.mhHeadS : 20;
		int mins = sCtl.mhTimeMin ? sCtl.mhTimeMin - 1 : 20, dmg = sCtl.mhDamage ? sCtl.mhDamage : 3;
		ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
		if (ImGui::SliderInt("Shines to win", &goal, 1, 30)) {
			sCtl.mhGoal = (u8)goal;
			sCtlDirty   = true;
		}
		ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
		if (ImGui::SliderInt("Head start##mh", &head, 0, 120, "%d s")) {
			sCtl.mhHeadS = (u8)(head > 0 ? head : 1);
			sCtlDirty    = true;
		}
		ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
		if (ImGui::SliderInt("Time limit##mh", &mins, 0, 120, mins ? "%d min" : "none")) {
			sCtl.mhTimeMin = (u8)(mins + 1);
			sCtlDirty      = true;
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("When the time runs out, the Shadow Marios win.");
		ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
		if (ImGui::SliderInt("Hit damage", &dmg, 1, 8, "%d of 8")) {
			sCtl.mhDamage = (u8)dmg;
			sCtlDirty     = true;
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Health the runner loses when a Shadow Mario touches or stomps him (Mario has 8).");
		char cur[48] = "Random";
		PortMpPlayerInfo pi;
		if (sCtl.mhPick && port_mp_get_player(sCtl.mhPick - 1, &pi) && pi.used)
			std::snprintf(cur, sizeof(cur), "%s", pi.name);
		ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
		if (ImGui::BeginCombo("Runner", cur)) {
			if (ImGui::Selectable("Random", sCtl.mhPick == 0)) {
				sCtl.mhPick = 0;
				sCtlDirty   = true;
			}
			for (int i = 0; i < PORT_MP_MAX_PLAYERS; i++)
				if (port_mp_get_player(i, &pi) && pi.used) {
					ImGui::PushID(5000 + i);
					if (ImGui::Selectable(pi.name, sCtl.mhPick == i + 1)) {
						sCtl.mhPick = (u8)(i + 1);
						sCtlDirty   = true;
					}
					ImGui::PopID();
				}
			ImGui::EndCombo();
		}
		bool sw = sCtl.mhSwitch != 0;
		if (ImGui::Checkbox("Switch", &sw)) {
			sCtl.mhSwitch = sw;
			sCtlDirty     = true;
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("A Shadow Mario who kills the runner becomes the runner (the runner becomes a hunter).\n"
			                  "Everyone counts the Shines he took while running: the first to the goal wins,\n"
			                  "else whoever has the most when the time is up.");
		ImGui::TextDisabled("Everyone else hunts as Shadow Mario. Every level is open; Shines start over.");
	} else if (sPick == PORT_MP_MODE_SHINERUN) {
		int goal = sCtl.srGoal ? sCtl.srGoal : 5, mins = sCtl.srTimeMin ? sCtl.srTimeMin - 1 : 15;
		int loss = sCtl.srDeathLoss ? sCtl.srDeathLoss - 1 : 3;
		ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
		if (ImGui::SliderInt("Shines to win##sr", &goal, 1, 50)) {
			sCtl.srGoal = (u8)goal;
			sCtlDirty   = true;
		}
		ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
		if (ImGui::SliderInt("Time limit##sr", &mins, 0, 120, mins ? "%d min" : "none")) {
			sCtl.srTimeMin = (u8)(mins + 1);
			sCtlDirty      = true;
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("When the time runs out, the player with the most Shine Sprites wins.");
		ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
		if (ImGui::SliderInt("Death loss", &loss, 0, 20, "%d Shines")) {
			sCtl.srDeathLoss = (u8)(loss + 1);
			sCtlDirty        = true;
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Shine Sprites a player loses by dying.");
		ImGui::TextDisabled("Hit a player (stomp, ground pound, FLUDD) to knock a Shine out of him.");
	} else if (sPick == PORT_MP_MODE_CNR) {
		int cops = sCtl.crCops, coins = sCtl.crCoins ? sCtl.crCoins : 100, shines = sCtl.crShines ? sCtl.crShines - 1 : 3;
		int mins = sCtl.crTimeMin ? sCtl.crTimeMin - 1 : 12, head = sCtl.crHeadS ? sCtl.crHeadS : 15;
		ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
		if (ImGui::SliderInt("Cops", &cops, 0, PORT_MP_MAX_PLAYERS - 1, cops ? "%d" : "auto (a third)")) {
			sCtl.crCops = (u8)cops;
			sCtlDirty   = true;
		}
		ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
		if (ImGui::SliderInt("Coin quota", &coins, 10, 999)) {
			sCtl.crCoins = (u16)coins;
			sCtlDirty    = true;
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("The robbers' team total. Yellow coins count 1, red 2, blue 5.");
		ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
		if (ImGui::SliderInt("Shine quota", &shines, 0, 20)) {
			sCtl.crShines = (u8)(shines + 1);
			sCtlDirty     = true;
		}
		ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
		if (ImGui::SliderInt("Time limit##cr", &mins, 0, 60, mins ? "%d min" : "none")) {
			sCtl.crTimeMin = (u8)(mins + 1);
			sCtlDirty      = true;
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("When the time runs out without the quota met, the cops win.");
		ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
		if (ImGui::SliderInt("Head start##cr", &head, 1, 120, "%d s")) {
			sCtl.crHeadS = (u8)head;
			sCtlDirty    = true;
		}
		char cur[48] = "Random";
		PortMpPlayerInfo pi;
		if (sCtl.crPick && port_mp_get_player(sCtl.crPick - 1, &pi) && pi.used)
			std::snprintf(cur, sizeof(cur), "%s", pi.name);
		ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
		if (ImGui::BeginCombo("A cop", cur)) {
			if (ImGui::Selectable("Random", sCtl.crPick == 0)) {
				sCtl.crPick = 0;
				sCtlDirty   = true;
			}
			for (int i = 0; i < PORT_MP_MAX_PLAYERS; i++)
				if (port_mp_get_player(i, &pi) && pi.used) {
					ImGui::PushID(6000 + i);
					if (ImGui::Selectable(pi.name, sCtl.crPick == i + 1)) {
						sCtl.crPick = (u8)(i + 1);
						sCtlDirty   = true;
					}
					ImGui::PopID();
				}
			ImGui::EndCombo();
		}
		ImGui::TextDisabled("Cops knock robbers over (stomp, ground pound, FLUDD) and touch them: off to the");
		ImGui::TextDisabled("prison by the plaza's police station until another robber frees them.");
	} else {
		ImGui::TextDisabled("(no settings: the lobby with every entrance closed)");
	}
}

// MarDirectorSetupObjects.cpp: the player list for the "GX Post" perform list.
void* port_mp_hud_viewobj(void)
{
	if (sList == nullptr)
		sList = new (sListMem) TMpPlayerList();
	return sList;
}

} // extern "C"

// GCConsole2.cpp: in a session the plaza's news ticker shows lobby news.
extern "C" int port_mp_news_on(void) { return port_mp_active() ? 1 : 0; }

// GCConsole2.cpp, when the ticker is free: the next news line (1), or the lobby status when
// nothing new has come for a while, else 0.
extern "C" int port_mp_news_next(char* out, int cap)
{
	if (sNewsCount > 0) {
		std::snprintf(out, cap, "...LOBBY NEWS...  %s", sNews[0]);
		std::memmove(sNews[0], sNews[1], sizeof(sNews[0]) * (kNewsMax - 1));
		sNewsCount--;
		sNewsLastShown = port_mp_time_ms();
		return 1;
	}
	if (port_mp_time_ms() - sNewsLastShown < kNewsStatusMs)
		return 0;
	sNewsLastShown = port_mp_time_ms();
	int players = 0;
	for (int i = 0; i < PORT_MP_MAX_PLAYERS; i++) {
		PortMpPlayerInfo p;
		players += port_mp_get_player(i, &p) && p.used ? 1 : 0;
	}
	PortMpSessionSettings set;
	port_mp_get_settings(&set);
	char next[120];
	newsNextLine(next, sizeof(next));
	std::snprintf(out, cap, "...LOBBY NEWS...  \"%s\": %d player%s. Game mode: %s. %s", port_mp_lobby_name(), players,
	              players == 1 ? "" : "s", modeName(set.gameMode), next);
	return 1;
}


#endif // SMS_PORT_AURORA
