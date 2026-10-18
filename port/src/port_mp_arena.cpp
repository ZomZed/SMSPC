// Where Hide and Seek and Freeze Tag are played (the "Map" mode setting, port_mp_modes.cpp).
//
// The running choice travels with the mode in PortMpSessionSettings.reserved[9] (the arena) and
// reserved[10] (the exit lock), so every client routes stage changes the same way:
//  - Single level (reserved[9] = 0: Delfino Plaza, the default, as before; else stage + 1): the mode
//    reloads everyone into that level and the whole match stays there. Every stage change that would
//    leave it is sent back (port_mp_game.cpp, port_mp_arena_route), the pause menu has no "Exit Area"
//    (PauseMenu2.cpp) and the lobby's "Go to plaza" is off. Areas of the same level (the hotel and
//    casino of Sirena Beach, the park of Pinna Park, the bay's undersea area) stay reachable; secret
//    courses do not.
//  - All levels (reserved[9] = 0xFF): everyone starts in the plaza with every entrance open (the
//    free-mode plaza) and may go into any level. There is no episode select: a level entered from the
//    plaza loads one fixed episode of it (kGeneric), the same for everyone, so players in the same
//    level see each other.
//  - Either way the levels have no objectives: Shine Sprites and red coins are removed as they show up.
//  - Exit lock (reserved[10] seconds, all levels only): each time a hider (Hide and Seek) or a runner
//    (Freeze Tag, frozen ones too) enters a level, he cannot leave it for that long: no "Exit Area", no
//    "Go to plaza", and any other trip to the plaza puts him back in the level. So a hider cannot slip
//    out the moment a seeker comes in. (Dying is no way out either: a hider or runner who dies is caught,
//    port_mp_hideseek.cpp / port_mp_freezetag.cpp.) Seekers and taggers come and go freely. "Warp to
//    player" is off in both modes.
#ifdef SMS_PORT_AURORA

#include "port_mp.h"

#include <MoveBG/Item.hpp>
#include <Player/Mario.hpp>
#include <MoveBG/ItemManager.hpp>
#include <Strategic/ActorTypes.hpp>
#include <System/Application.hpp>
#include <System/MarDirector.hpp>
#include <System/StageUtil.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" int port_mp_pause_exit_refused(void);   // below
extern "C" void port_mp_toast(const char* text);   // port_mp_menu.cpp
extern "C" int port_mp_hide_local_hider(void);      // port_mp_hideseek.cpp
extern "C" int port_mp_freeze_local_runner(void);   // port_mp_freezetag.cpp
extern "C" int port_mp_prop_local_hider(void);      // port_mp_prophunt.cpp
extern "C" int port_mp_cnr_route(int* nextState, unsigned char* stage, unsigned char* scenario); // port_mp_cnr.cpp
extern "C" int port_mp_manhunt_route(int from, int* nextState, unsigned char* stage, unsigned char* scenario); // port_mp_manhunt.cpp
extern "C" int port_mp_plaza_scenario(const PortMpSessionSettings* s); // port_mp_modes.cpp

namespace {

const int kResArena = 9;  // PortMpSessionSettings.reserved[9]
const int kResLock  = 10; // reserved[10]

struct Level {
	u8 stage;
	u8 scenario; // the episode loaded for it (index = TApplication scenario)
	const char* name;
};
// The levels a single-level match can use, and the episode each level loads in either arena: one that
// shows the level in its normal state, without a boss, a race, a chase or a timer (Shines and red
// coins are removed anyway). The plaza's own scenario comes from port_mp_plaza_scenario.
const Level kLevels[] = {
	{ 1, 0, "Delfino Plaza" },
	{ 0, 1, "Delfino Airstrip" },
	{ 2, 2, "Bianco Hills" },        // episode 3, The Hillside Cave Secret
	{ 3, 2, "Ricco Harbor" },        // episode 3, The Caged Shine Sprite
	{ 4, 0, "Gelato Beach" },        // episode 1, Dune Bud Sand Castle Secret
	{ 5, 1, "Pinna Park" },          // episode 2, The Beach Cannon's Secret
	{ 13, 1, "Pinna Park (the park)" },
	{ 6, 1, "Sirena Beach" },        // episode 2, The Hotel Lobby's Secret
	{ 8, 4, "Pianta Village" },      // episode 5, Secret of the Village Underside
	{ 9, 5, "Noki Bay" },            // episode 6, The Shell's Secret
};
const int kLevelCount = (int)(sizeof(kLevels) / sizeof(kLevels[0]));

const Level* findLevel(int stage)
{
	for (int i = 0; i < kLevelCount; i++)
		if (kLevels[i].stage == stage)
			return &kLevels[i];
	return nullptr;
}

bool tagMode(const PortMpSessionSettings& s)
{
	return s.gameMode == PORT_MP_MODE_HIDE || s.gameMode == PORT_MP_MODE_FREEZE || s.gameMode == PORT_MP_MODE_PROP;
}

bool isExStage(int stage) { return stage > 0x14 && stage < 0x35; }

int curStage() { return SMSGetApplication()->mCurrArea.getStage(); }

// ---- exit lock ---------------------------------------------------------------------------------
double sLockUntil;      // local time the lock ends (0 = none)
unsigned sLockSerial;   // the stage visit the lock was started for
bool sLockArmed;        // sLockSerial is valid

bool localRunner() { return port_mp_hide_local_hider() || port_mp_freeze_local_runner() || port_mp_prop_local_hider(); }

int lockSecondsLeft()
{
	if (sLockUntil <= 0.0 || !localRunner())
		return 0;
	const double left = sLockUntil - port_mp_time_ms();
	return left > 0.0 ? (int)((left + 999.0) / 1000.0) : 0;
}

void lockFrame(const PortMpSessionSettings& s)
{
	if (!tagMode(s) || s.reserved[kResArena] != 0xFF || s.reserved[kResLock] == 0) {
		sLockUntil = 0.0;
		sLockArmed = false;
		return;
	}
	TMarDirector* d = gpMarDirector;
	if (d == nullptr || d->mState != TMarDirector::STATE_UNK4)
		return; // the lock starts when Mario gets control in the new stage
	if (sLockArmed && sLockSerial == gPortMpStageSerial)
		return;
	sLockArmed  = true;
	sLockSerial = gPortMpStageSerial;
	const int stage = curStage();
	if (stage == 1 || stage == 15 || !localRunner()) {
		sLockUntil = 0.0;
		return;
	}
	sLockUntil = port_mp_time_ms() + s.reserved[kResLock] * 1000.0;
	char t[96];
	std::snprintf(t, sizeof(t), "You cannot leave this level for %d s.", (int)s.reserved[kResLock]);
	port_mp_toast(t);
	std::fprintf(stderr, "[mp-arena] exit lock: %d s in stage %d\n", (int)s.reserved[kResLock], stage);
}

// ---- no objectives -------------------------------------------------------------------------------
void stripObjectives()
{
	TMarDirector* d = gpMarDirector;
	if (d == nullptr || d->mState != TMarDirector::STATE_UNK4 || gpItemManager == nullptr)
		return;
	for (int i = 0; i < gpItemManager->getObjNum(); i++) {
		THitActor* a = gpItemManager->getObj(i);
		if (a == nullptr || !(a->isActorType(ACTOR_TYPE_SHINE) || a->isActorType(ACTOR_TYPE_COIN_RED)))
			continue;
		TItem* it = (TItem*)a;
		if (!it->checkLiveFlag(LIVE_FLAG_DEAD)) {
			it->kill();
			std::fprintf(stderr, "[mp-arena] removed a %s (stage %d)\n",
			             a->isActorType(ACTOR_TYPE_SHINE) ? "Shine Sprite" : "red coin", curStage());
		}
	}
}

// SMS_PORT_MP_ARENA_TEST="enter=<stage>@<n>;exit@<n>;force@<n>;die@<n>" (tests): at the n-th frame of normal
// play in a session, enter a level the way a painting does (no scenario: the episode select path),
// choose "Exit Area" (refused while the exit lock holds), or leave for the plaza anyway (the
// stage-change check must hold a locked runner), or lose all health (a death).
void testFrame()
{
	static const char* sTest = std::getenv("SMS_PORT_MP_ARENA_TEST");
	static int sFrames;
	TMarDirector* d = gpMarDirector;
	if (sTest == nullptr || d == nullptr || d->mState != TMarDirector::STATE_UNK4)
		return;
	sFrames++;
	for (const char* p = sTest; p != nullptr && *p != '\0';) {
		const char* at = std::strchr(p, '@');
		if (at == nullptr)
			break;
		if (std::atoi(at + 1) == sFrames) {
			if (std::strncmp(p, "enter=", 6) == 0) {
				std::fprintf(stderr, "[mp-arena] test: enter stage %d\n", std::atoi(p + 6));
				d->setNextStage((u16)std::atoi(p + 6), nullptr);
			} else if (std::strncmp(p, "exit", 4) == 0) {
				const bool refused = port_mp_pause_exit_refused() != 0;
				std::fprintf(stderr, "[mp-arena] test: Exit Area %s\n", refused ? "refused" : "taken");
				if (!refused)
					d->setNextStage(1, nullptr);
			} else if (std::strncmp(p, "die", 3) == 0 && gpMarioOriginal != nullptr) {
				std::fprintf(stderr, "[mp-arena] test: Mario loses all health\n");
				gpMarioOriginal->decHP(gpMarioOriginal->getHealth());
			} else if (std::strncmp(p, "force", 5) == 0) {
				std::fprintf(stderr, "[mp-arena] test: leaving for the plaza anyway\n");
				d->setNextStage(1, nullptr);
			}
		}
		p = std::strchr(at, ';');
		if (p != nullptr)
			p++;
	}
}

void gameFrame()
{
	if (!port_mp_active())
		return;
	PortMpSessionSettings s;
	port_mp_get_settings(&s);
	lockFrame(s);
	if (tagMode(s))
		stripObjectives();
	testFrame();
}

struct Register {
	Register() { port_mp_register_game_frame(gameFrame); }
} sRegister;

} // namespace

extern "C" {

// The levels a single-level match can pick (the mode settings).
int port_mp_arena_level_count(void) { return kLevelCount; }
const char* port_mp_arena_level_name(int i) { return i >= 0 && i < kLevelCount ? kLevels[i].name : "?"; }
// reserved[9] for a single-level match in level i (0 = the plaza).
int port_mp_arena_code_for_level(int i)
{
	const int stage = i > 0 && i < kLevelCount ? kLevels[i].stage : 1;
	return stage == 1 ? 0 : stage + 1;
}

// The episode a level loads in Hide and Seek / Freeze Tag (-1: not one of the listed levels).
int port_mp_generic_scenario(int stage)
{
	const Level* l = findLevel(stage);
	return l != nullptr && stage != 1 ? l->scenario : -1;
}

// 1 = the running mode is played in all levels.
int port_mp_arena_all_levels(const PortMpSessionSettings* s) { return tagMode(*s) && s->reserved[kResArena] == 0xFF; }

// Where the running mode starts and where players come back to (a mode start, a late join, a game
// over): the plaza, or a single-level match's level.
void port_mp_home_area(const PortMpSessionSettings* s, int* stage, int* scenario)
{
	const int code = s->reserved[kResArena];
	if (tagMode(*s) && code != 0 && code != 0xFF) {
		*stage           = code - 1;
		const int scen   = port_mp_generic_scenario(*stage);
		*scenario        = scen >= 0 ? scen : 0;
		return;
	}
	*stage    = s->startStage ? s->startStage : 1;
	*scenario = port_mp_plaza_scenario(s);
}

int port_mp_home_stage(void)
{
	PortMpSessionSettings s;
	port_mp_get_settings(&s);
	int stage, scen;
	port_mp_home_area(&s, &stage, &scen);
	return stage;
}

// A short name of the running arena ("all levels", "Bianco Hills"; null: not a tag mode).
const char* port_mp_arena_label(const PortMpSessionSettings* s)
{
	if (!tagMode(*s))
		return nullptr;
	const int code = s->reserved[kResArena];
	if (code == 0xFF)
		return "all levels";
	const Level* l = findLevel(code == 0 ? 1 : code - 1);
	return l != nullptr ? l->name : "?";
}

// Seconds the local player still cannot leave his level (0 = free to go).
int port_mp_arena_exit_lock_left(void)
{
	PortMpSessionSettings s;
	port_mp_get_settings(&s);
	if (!port_mp_active() || !port_mp_arena_all_levels(&s))
		return 0;
	return lockSecondsLeft();
}

// PauseMenu2.cpp (load): 1 = no "Exit Area" in this stage (a single-level match).
int port_mp_pause_no_exit(void)
{
	PortMpSessionSettings s;
	port_mp_get_settings(&s);
	return port_mp_active() && tagMode(s) && s.reserved[kResArena] != 0xFF ? 1 : 0;
}

// PauseMenu2.cpp: "Exit Area" chosen. 1 = refused (the exit lock; tells the player why).
int port_mp_pause_exit_refused(void)
{
	const int left = port_mp_arena_exit_lock_left();
	if (left <= 0)
		return 0;
	char t[96];
	std::snprintf(t, sizeof(t), "You can leave this level in %d s.", left);
	port_mp_toast(t);
	return 1;
}

// port_mp_presence.cpp: "Go to plaza" (1 = refused, with a toast) and "Warp to player" (1 = refused).
int port_mp_arena_plaza_refused(void)
{
	PortMpSessionSettings s;
	port_mp_get_settings(&s);
	if (!port_mp_active() || !tagMode(s))
		return 0;
	if (s.reserved[kResArena] != 0xFF) {
		if (curStage() == 1)
			return 0;
		port_mp_toast("This match stays in one level: no going to the plaza.");
		return 1;
	}
	return port_mp_pause_exit_refused();
}
int port_mp_arena_warp_refused(void)
{
	PortMpSessionSettings s;
	port_mp_get_settings(&s);
	if (!port_mp_active() || !(tagMode(s) || s.gameMode == PORT_MP_MODE_MANHUNT || s.gameMode == PORT_MP_MODE_CNR))
		return 0;
	port_mp_toast(s.gameMode == PORT_MP_MODE_CNR       ? "No warping to players during Cops and Robbers."
	              : s.gameMode == PORT_MP_MODE_MANHUNT ? "No warping to players during Manhunt."
	              : s.gameMode == PORT_MP_MODE_HIDE   ? "No warping to players during Hide and Seek."
	              : s.gameMode == PORT_MP_MODE_PROP ? "No warping to players during Prop Hunt."
	                                                : "No warping to players during Freeze Tag.");
	return 1;
}

// port_mp_game.cpp (port_mp_redirect_title), for every mode but Free: where a stage change may go.
// None and single-level matches stay in their home area (sub-areas of a single level are fine); all
// levels skips the episode select and loads each level's fixed episode, and holds a locked runner in
// his level. `from` is the stage being left (-1: none). Returns 1 when it changed the destination.
int port_mp_arena_route(int from, int fromScenario, int* nextState, unsigned char* stage, unsigned char* scenario)
{
	PortMpSessionSettings s;
	port_mp_get_settings(&s);
	if (s.gameMode == PORT_MP_MODE_MANHUNT) // every level open; hunters follow the runner in
		return port_mp_manhunt_route(from, nextState, stage, scenario);
	if (s.gameMode == PORT_MP_MODE_SHINERUN) // every level open, as Free
		return 0;
	if (s.gameMode == PORT_MP_MODE_CNR) // every level open; prisoners stay in the plaza
		return port_mp_cnr_route(nextState, stage, scenario);
	const int ns       = *nextState;
	const bool title    = ns == TApplication::APP_STATE_TITLE; // the episode select of a level entrance
	const bool gameplay = ns == TApplication::APP_STATE_GAMEPLAY;
	if (!title && !(gameplay && *stage != 15))
		return 0;
	int home, homeScen;
	port_mp_home_area(&s, &home, &homeScen);
	if (port_mp_arena_all_levels(&s)) {
		// leaving a level for the plaza (Exit Area, a lost life: decideNextStage) while locked
		if (*stage == home && from >= 0 && from != home && port_mp_arena_exit_lock_left() > 0) {
			std::fprintf(stderr, "[mp-arena] exit lock: leaving %d for the plaza refused\n", from);
			*nextState = TApplication::APP_STATE_GAMEPLAY;
			*stage     = (u8)from;
			*scenario  = (u8)fromScenario;
			port_mp_pause_exit_refused();
			return 1;
		}
		if (title || (gameplay && from == home && *stage != home)) {
			const int g = port_mp_generic_scenario(*stage);
			if (g < 0)
				return 0; // not a listed level (a secret course, Corona Mountain): as it is
			std::fprintf(stderr, "[mp-arena] all levels: entrance to %d/%d (state %d) loads episode %d\n", *stage,
			             *scenario, ns, g + 1);
			*nextState = TApplication::APP_STATE_GAMEPLAY;
			*scenario  = (u8)g;
			return 1;
		}
		return 0;
	}
	// None, or a single-level match
	if (gameplay) {
		if (*stage == home)
			return 0;
		if (home != 1 && tagMode(s) && !isExStage(*stage) && SMS_getShineStage(*stage) == SMS_getShineStage((u8)home))
			return 0; // another area of the same level
	}
	std::fprintf(stderr, "[mp-arena] stage change to %d/%d (state %d) sent back to %d/%d\n", *stage, *scenario, ns,
	             home, homeScen);
	*nextState = TApplication::APP_STATE_GAMEPLAY;
	*stage     = (u8)home;
	*scenario  = (u8)homeScen;
	port_mp_toast(tagMode(s) ? "This match stays in one level." : "You are in a lobby: the host has not started a game mode yet.");
	return 1;
}

} // extern "C"

#endif // SMS_PORT_AURORA
