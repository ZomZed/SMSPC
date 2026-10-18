// Multiplayer game mode "Manhunt" (mode PORT_MP_MODE_MANHUNT, run by port_mp_modes.cpp like the
// other round modes): one runner, a normal Mario with FLUDD, has to collect a number of Shine Sprites
// (host setting, default 3) while everyone else hunts him as Shadow Mario (no FLUDD; drawn by
// port_mp_prophunt.cpp's look code, the game's own /kagemario model).
//  - The whole world is open (the free-mode plaza and save) but every Shine Sprite is collectible
//    again: the save's Shine flags are set aside for the match (port_mp_modes.cpp), so the episode
//    selects offer what a fresh file would (episode 1, then each next one as its Shine is taken).
//  - Head start (default 20 s): the hunters' pads are idle. Then a hunter touching or stomping the
//    runner costs him health (default 3 of 8; the game's own damageExec: knock-down, invincibility
//    frames, coins heal). The runner dying any way (health, a pit, drowning) ends the round.
//  - A hunter entering the level the runner is in goes straight into the runner's episode
//    (port_mp_arena_route), so they meet. Nobody warps to players.
//  - The runner wins with the Shine count reached; the hunters win when he dies, leaves, or the time
//    limit (default 20 min, 0 = none) runs out.
//  - Switch (host option): a hunter who kills the runner (the runner dies within 10 s of that
//    hunter's hit) becomes the runner, and the old runner a hunter. Everyone keeps his own count of
//    the Shines he took while running (each Shine counts once in a round, and everyone's save marks
//    it taken); the first to reach the goal wins, else whoever has the most when the time is up.
//    A new runner gets FLUDD back and 10 s in which hits do not count; a runner who dies on his own
//    (a pit, drowning) stays the runner.
// State on PORT_MP_CH_MANHUNT from the host (reliable on every change, once a second).
#ifdef SMS_PORT_AURORA

#include "port_mp.h"

#include <Player/Mario.hpp>
#include <Player/MarioAccess.hpp>
#include <Strategic/HitActor.hpp>
#include <Player/Watergun.hpp>
#include <System/Application.hpp>
#include <System/FlagManager.hpp>
#include <System/MarDirector.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" void port_mp_toast(const char* text);                                                // port_mp_menu.cpp
extern "C" void port_mp_manhunt_settings(int* goal, int* headS, int* timeS, int* damage, int* pick,
                                         int* swap); // port_mp_modes.cpp
extern "C" void port_mp_modes_round_over(void);                                                 // port_mp_modes.cpp
extern "C" int port_mp_home_stage(void);                                                        // port_mp_arena.cpp
extern "C" int port_mp_puppet_pos(int slot, float pos[3]);                                      // port_mp_puppet.cpp
extern "C" void port_mp_manhunt_shine_got(int shineId);                                        // below

namespace {

const uint8_t kChannel = PORT_MP_CH_MANHUNT;
enum Phase : u8 { MP_OFF = 0, MP_HEAD = 1, MP_PLAY = 2, MP_OVER = 3 };
enum Role : u8 { R_NONE = 0, R_RUNNER = 1, R_HUNTER = 2 };
enum : u8 { MSG_STATE = 1, MSG_SHINE = 2, MSG_DIED = 3, MSG_HIT = 4 };

#pragma pack(push, 1)
struct State {
	u8 type; // MSG_STATE
	u8 phase;
	u8 round;
	u8 winner; // MP_OVER: 1 the runner, 2 the hunters, 3 Switch: winnerSlot (0xFF = a draw)
	u32 leftMs;
	u8 runner; // slot
	u8 shines; // collected this round
	u8 goal;
	u8 lastShine; // the latest one's event id (toasts)
	u8 role[PORT_MP_MAX_PLAYERS];
	u8 got[32]; // Shine event ids taken this round (bits)
	u8 swap;       // Switch on for this round
	u8 swaps;      // +1 per switch (toasts)
	u8 prevRunner; // the runner before the latest switch
	u8 winnerSlot; // winner 1 / 3: who
	u8 scores[PORT_MP_MAX_PLAYERS]; // Shines each took while running
};
struct ShineMsg {
	u8 type; // MSG_SHINE (runner -> host)
	u8 id;
};
struct HitMsg {
	u8 type; // MSG_HIT (hunter -> runner)
	u8 victim;
	u8 damage;
	u8 pad;
	float pos[3]; // the hunter (knock-back direction)
};
#pragma pack(pop)

State sState = { MSG_STATE, MP_OFF, 0, 0, 0, 0xFF, 0, 0, 0, {}, {}, 0, 0, 0xFF, 0xFF, {} };
State sSeen;
double sDeadline;
double sLastSend;
bool sDirty;
double sModeSeenAt;
bool sHeld;
int sHitCooldown;
double sGraceUntil;   // runner: hits do not count before this (a new runner after a switch)
int sLastHitter = -1; // runner: the hunter who hit him last, and when
double sLastHitAt;
u8 sApplied[32];      // Shine bits already marked taken in our save

bool running()
{
	PortMpSessionSettings s;
	port_mp_get_settings(&s);
	return port_mp_active() && s.gameMode == PORT_MP_MODE_MANHUNT;
}

int secondsLeft() { return sDeadline > 0.0 ? (int)((sDeadline - port_mp_time_ms() + 999.0) / 1000.0) : 0; }

bool used(int slot)
{
	PortMpPlayerInfo p;
	return port_mp_get_player(slot, &p) && p.used;
}

const char* nameOf(int slot, char* buf, size_t cap)
{
	PortMpPlayerInfo p;
	std::snprintf(buf, cap, "%s", port_mp_get_player(slot, &p) ? p.name : "?");
	return buf;
}

void setPhase(u8 phase, int seconds)
{
	sState.phase = phase;
	sDeadline    = seconds > 0 ? port_mp_time_ms() + seconds * 1000.0 : 0.0;
	sDirty       = true;
}

int me() { return port_mp_local_slot(); }
u8 myRole() { return me() >= 0 && me() < PORT_MP_MAX_PLAYERS ? sState.role[me()] : R_NONE; }
bool roundOn() { return sState.phase == MP_HEAD || sState.phase == MP_PLAY; }

void endRound(u8 winner, const char* why)
{
	sState.winner = winner;
	setPhase(MP_OVER, 8);
	std::fprintf(stderr, "[mp-manhunt] round over: %s (%s)\n", winner == 1 ? "the runner wins" : "the hunters win", why);
}

// ---- host ------------------------------------------------------------------------------------
void hostStartRound()
{
	int goal, headS, timeS, damage, pick, swap;
	port_mp_manhunt_settings(&goal, &headS, &timeS, &damage, &pick, &swap);
	int slots[PORT_MP_MAX_PLAYERS], n = 0;
	for (int i = 0; i < PORT_MP_MAX_PLAYERS; i++)
		if (used(i))
			slots[n++] = i;
	std::memset(sState.role, R_NONE, sizeof(sState.role));
	std::memset(sState.got, 0, sizeof(sState.got));
	std::memset(sState.scores, 0, sizeof(sState.scores));
	sState.swap       = (u8)swap;
	sState.swaps      = 0;
	sState.prevRunner = 0xFF;
	sState.winnerSlot = 0xFF;
	if (n == 0)
		return;
	const int runner = pick >= 0 && pick < PORT_MP_MAX_PLAYERS && used(pick) ? pick : slots[std::rand() % n];
	for (int i = 0; i < n; i++)
		sState.role[slots[i]] = slots[i] == runner ? R_RUNNER : R_HUNTER;
	sState.runner    = (u8)runner;
	sState.shines    = 0;
	sState.goal      = (u8)goal;
	sState.lastShine = 0;
	sState.round++;
	sState.winner = 0;
	setPhase(MP_HEAD, headS);
	char nm[32];
	std::fprintf(stderr, "[mp-manhunt] round %d: runner %s (slot %d), %d hunters, %d Shines to get%s\n", sState.round,
	             nameOf(runner, nm, sizeof(nm)), runner, n - 1, goal, swap ? " (Switch)" : "");
}

void hostShine(int slot, int id)
{
	if (!roundOn() || slot != sState.runner || id < 0 || id >= 256)
		return;
	if (sState.got[id >> 3] & (1 << (id & 7)))
		return; // that one again
	sState.got[id >> 3] |= (u8)(1 << (id & 7));
	if (sState.scores[slot] < 255)
		sState.scores[slot]++;
	sState.shines    = sState.scores[slot];
	sState.lastShine = (u8)id;
	sDirty           = true;
	std::fprintf(stderr, "[mp-manhunt] runner (slot %d) got Shine %d (%d/%d)\n", slot, id, sState.shines, sState.goal);
	if (sState.scores[slot] >= sState.goal) {
		sState.winnerSlot = (u8)slot;
		endRound(1, "Shines collected");
	}
}

// Switch: the runner role goes to `to` (a hunter); the old runner hunts.
void hostSwitch(int to, const char* why)
{
	const int from = sState.runner;
	if (from < PORT_MP_MAX_PLAYERS && sState.role[from] == R_RUNNER)
		sState.role[from] = R_HUNTER;
	sState.role[to]   = R_RUNNER;
	sState.runner     = (u8)to;
	sState.prevRunner = (u8)from;
	sState.shines     = sState.scores[to];
	sState.swaps++;
	sDirty = true;
	char a[32], b[32];
	std::fprintf(stderr, "[mp-manhunt] switch (%s): %s runs now, %s hunts\n", why, nameOf(to, a, sizeof(a)),
	             from < PORT_MP_MAX_PLAYERS ? nameOf(from, b, sizeof(b)) : "-");
}

void hostDied(int slot, int killer)
{
	if (!roundOn() || slot != sState.runner)
		return;
	if (!sState.swap) {
		endRound(2, "the runner died");
		return;
	}
	if (killer >= 0 && killer < PORT_MP_MAX_PLAYERS && sState.role[killer] == R_HUNTER && used(killer))
		hostSwitch(killer, "the runner was caught");
	else
		std::fprintf(stderr, "[mp-manhunt] the runner died on his own: still the runner\n");
}

// Switch, time up: the most Shines wins (0xFF = a draw).
void hostTimeUp()
{
	int best = -1, bestN = -1;
	bool tie = false;
	for (int i = 0; i < PORT_MP_MAX_PLAYERS; i++) {
		if (sState.role[i] == R_NONE)
			continue;
		if (sState.scores[i] > bestN) {
			best  = i;
			bestN = sState.scores[i];
			tie   = false;
		} else if (sState.scores[i] == bestN) {
			tie = true;
		}
	}
	sState.winnerSlot = tie || best < 0 ? 0xFF : (u8)best;
	endRound(3, "time is up");
}

void hostFrame()
{
	if (!running()) {
		if (sState.phase != MP_OFF) {
			std::memset(sState.role, R_NONE, sizeof(sState.role));
			setPhase(MP_OFF, 0);
		}
		sModeSeenAt = 0.0;
		return;
	}
	const double now = port_mp_time_ms();
	if (sModeSeenAt == 0.0)
		sModeSeenAt = now;
	for (int i = 0; i < PORT_MP_MAX_PLAYERS; i++)
		if (sState.role[i] != R_NONE && !used(i)) {
			sState.role[i] = R_NONE;
			sDirty         = true;
		}
	int goal, headS, timeS, damage, pick, swap;
	port_mp_manhunt_settings(&goal, &headS, &timeS, &damage, &pick, &swap);
	switch (sState.phase) {
	case MP_OFF: {
		TMarDirector* d = gpMarDirector;
		if (now - sModeSeenAt > 3000.0 && d != nullptr && d->mState == TMarDirector::STATE_UNK4
		    && SMSGetApplication()->mCurrArea.getStage() == port_mp_home_stage())
			hostStartRound();
		break;
	}
	case MP_HEAD:
	case MP_PLAY: {
		bool hunters = false;
		for (int i = 0; i < PORT_MP_MAX_PLAYERS; i++)
			hunters |= sState.role[i] == R_HUNTER;
		const bool runnerGone = sState.runner >= PORT_MP_MAX_PLAYERS || sState.role[sState.runner] != R_RUNNER;
		if (runnerGone && sState.swap && hunters) { // Switch: a hunter takes over
			int pickSlot = -1;
			for (int i = 0; i < PORT_MP_MAX_PLAYERS && pickSlot < 0; i++)
				if (sState.role[i] == R_HUNTER)
					pickSlot = i;
			hostSwitch(pickSlot, "the runner left");
		} else if (runnerGone) {
			endRound(2, "the runner left");
		} else if (!hunters) {
			sState.winnerSlot = sState.runner;
			endRound(1, "no hunters left");
		} else if (sState.phase == MP_HEAD && now >= sDeadline) {
			setPhase(MP_PLAY, timeS);
		} else if (sState.phase == MP_PLAY && sDeadline > 0.0 && now >= sDeadline) {
			if (sState.swap)
				hostTimeUp();
			else
				endRound(2, "time is up");
		}
		break;
	}
	case MP_OVER:
		if (now >= sDeadline) {
			std::memset(sState.role, R_NONE, sizeof(sState.role));
			setPhase(MP_OFF, 0);
			sModeSeenAt = 0.0;
			port_mp_modes_round_over();
		}
		break;
	}
}

// ---- every client ----------------------------------------------------------------------------
void toastChanges()
{
	const int m = me();
	char t[160], nm[32];
	if (sState.round != sSeen.round && sState.phase == MP_HEAD && m >= 0) {
		if (sState.role[m] == R_RUNNER)
			std::snprintf(t, sizeof(t), "Manhunt: you run! Get %d Shine Sprites. The Shadow Marios start in %d s.",
			              sState.goal, secondsLeft());
		else
			std::snprintf(t, sizeof(t), "Manhunt: catch %s before he gets %d Shine Sprites! (go in %d s)",
			              nameOf(sState.runner, nm, sizeof(nm)), sState.goal, secondsLeft());
		port_mp_toast(t);
	}
	if (sState.phase == MP_PLAY && sSeen.phase != MP_PLAY)
		port_mp_toast(myRole() == R_RUNNER ? "The Shadow Marios are coming!" : "Go! Touch or stomp the runner.");
	if (sState.swaps != sSeen.swaps && sState.round == sSeen.round && roundOn()) {
		char b[32];
		if (sState.runner == m)
			std::snprintf(t, sizeof(t), "You caught %s: now YOU run! (%d/%d Shines)", nameOf(sState.prevRunner, nm, sizeof(nm)),
			              sState.scores[m], sState.goal);
		else if (sState.prevRunner == m)
			std::snprintf(t, sizeof(t), "%s caught you: now you hunt as Shadow Mario.", nameOf(sState.runner, nm, sizeof(nm)));
		else
			std::snprintf(t, sizeof(t), "%s caught %s and runs now!", nameOf(sState.runner, nm, sizeof(nm)),
			              nameOf(sState.prevRunner, b, sizeof(b)));
		port_mp_toast(t);
	}
	if (sState.shines > sSeen.shines && sState.round == sSeen.round && sState.runner == sSeen.runner) {
		std::snprintf(t, sizeof(t), "%s got a Shine Sprite! (%d/%d)", nameOf(sState.runner, nm, sizeof(nm)), sState.shines,
		              sState.goal);
		port_mp_toast(t);
	}
	if (sState.phase == MP_OVER && sSeen.phase != MP_OVER) {
		if (sState.winner == 1)
			std::snprintf(t, sizeof(t), "%s got %d Shine Sprites and wins!",
			              nameOf(sState.winnerSlot < PORT_MP_MAX_PLAYERS ? sState.winnerSlot : sState.runner, nm, sizeof(nm)),
			              sState.winnerSlot < PORT_MP_MAX_PLAYERS ? sState.scores[sState.winnerSlot] : sState.shines);
		else if (sState.winner == 3 && sState.winnerSlot < PORT_MP_MAX_PLAYERS)
			std::snprintf(t, sizeof(t), "Time is up: %s wins with %d Shine Sprites!", nameOf(sState.winnerSlot, nm, sizeof(nm)),
			              sState.scores[sState.winnerSlot]);
		else if (sState.winner == 3)
			std::snprintf(t, sizeof(t), "Time is up: a draw!");
		else
			std::snprintf(t, sizeof(t), "The Shadow Marios win! (%s had %d/%d)", nameOf(sState.runner, nm, sizeof(nm)),
			              sState.shines, sState.goal);
		port_mp_toast(t);
		std::fprintf(stderr, "[mp-manhunt] %s\n", t);
	}
	sSeen = sState;
}

bool localPlayable(TMario* m)
{
	return m != nullptr && gpMarDirector != nullptr && gpMarDirector->mState == TMarDirector::STATE_UNK4
	       && !m->checkFlag(MARIO_FLAG_GAME_OVER);
}

void watchDeath()
{
	static int sLastState = -1;
	const int st = gpMarDirector != nullptr ? gpMarDirector->mState : -1;
	const bool died = st == TMarDirector::STATE_UNK7 && sLastState != TMarDirector::STATE_UNK7;
	sLastState = st;
	if (!died || !roundOn() || myRole() != R_RUNNER)
		return;
	const int killer = sLastHitter >= 0 && port_mp_time_ms() - sLastHitAt < 10000.0 ? sLastHitter : -1;
	if (port_mp_is_host()) {
		hostDied(me(), killer);
	} else {
		const u8 msg[2] = { MSG_DIED, (u8)(killer >= 0 ? killer : 0xFF) };
		port_mp_send(kChannel, msg, 2, 1, -1);
	}
}

// A hunter's client: its Mario overlapping the runner as drawn is a hit.
void hunterHits(TMario* m)
{
	if (sHitCooldown > 0)
		sHitCooldown--;
	if (sState.phase != MP_PLAY || myRole() != R_HUNTER || sHitCooldown > 0)
		return;
	const int r = sState.runner;
	f32 pos[3];
	if (r >= PORT_MP_MAX_PLAYERS || !port_mp_same_stage(r) || !port_mp_puppet_pos(r, pos))
		return;
	// SMS_PORT_MP_MANHUNT_TEST=1 (tests): a hunter jumps onto the runner every 2.5 s
	static const bool sTest = std::getenv("SMS_PORT_MP_MANHUNT_TEST") != nullptr;
	static double sTestAt;
	if (sTest && port_mp_time_ms() - sTestAt > 2500.0) {
		sTestAt = port_mp_time_ms();
		m->mPosition.set(pos[0], pos[1] + 40.0f, pos[2]);
		std::fprintf(stderr, "[mp-manhunt] test: hunter jumps onto the runner\n");
	}
	const f32 dx = pos[0] - m->mPosition.x, dz = pos[2] - m->mPosition.z, dy = pos[1] - m->mPosition.y;
	if (dx * dx + dz * dz >= 90.0f * 90.0f || dy <= -160.0f || dy >= 160.0f)
		return;
	int goal, headS, timeS, damage, pick, swap;
	port_mp_manhunt_settings(&goal, &headS, &timeS, &damage, &pick, &swap);
	HitMsg msg = { MSG_HIT, (u8)r, (u8)damage, 0, { m->mPosition.x, m->mPosition.y, m->mPosition.z } };
	port_mp_send(kChannel, &msg, (int)sizeof(msg), 1, r);
	sHitCooldown = 45;
	std::fprintf(stderr, "[mp-manhunt] hit the runner (damage %d)\n", damage);
}

// Stand-in for damageExec's knock-back direction (never registered with the hit check).
class TManhuntProxy : public THitActor {
public:
	TManhuntProxy()
	    : THitActor("mp manhunt hit")
	{
	}
};
TManhuntProxy sProxy;

void applyHit(int from, const HitMsg& msg)
{
	TMario* m = gpMarioOriginal;
	if (sState.phase != MP_PLAY || myRole() != R_RUNNER || !localPlayable(m) || !port_mp_same_stage(from))
		return;
	if (port_mp_time_ms() < sGraceUntil) // a new runner's grace after a switch
		return;
	sLastHitter = from;
	sLastHitAt  = port_mp_time_ms();
	if (m->mStatus == MARIO_STATUS_NOMOTION) // an emote's demo status refuses the knock-down
		m->changePlayerStatus(MARIO_STATUS_WAIT, 0, true);
	sProxy.mPosition.set(msg.pos[0], msg.pos[1], msg.pos[2]);
	const int before = m->mHealth;
	m->damageExec(&sProxy, msg.damage ? msg.damage : 3, 1, 0, 16.0f, 10, 0.0f, 60);
	std::fprintf(stderr, "[mp-manhunt] hit by slot %d: health %d -> %d\n", from, before, (int)m->mHealth);
}

void gameFrame()
{
	const bool host = port_mp_is_host() != 0;
	unsigned char buf[sizeof(State) + 8];
	int from, n;
	while ((n = port_mp_recv(kChannel, buf, (int)sizeof(buf), &from)) > 0) {
		if (buf[0] == MSG_STATE && n == (int)sizeof(State) && !host) {
			std::memcpy(&sState, buf, sizeof(sState));
			sDeadline = sState.leftMs > 0 ? port_mp_time_ms() + sState.leftMs : 0.0;
		} else if (buf[0] == MSG_SHINE && n == (int)sizeof(ShineMsg) && host) {
			hostShine(from, ((const ShineMsg*)buf)->id);
		} else if (buf[0] == MSG_DIED && n == 2 && host) {
			hostDied(from, buf[1] != 0xFF ? buf[1] : -1);
		} else if (buf[0] == MSG_HIT && n == (int)sizeof(HitMsg) && ((const HitMsg*)buf)->victim == me()) {
			applyHit(from, *(const HitMsg*)buf);
		}
	}
	if (host) {
		hostFrame();
		if (sDirty || port_mp_time_ms() - sLastSend > 1000.0) {
			const double left = sDeadline - port_mp_time_ms();
			sState.leftMs     = sDeadline > 0.0 ? (u32)(left > 1.0 ? left : 1.0) : 0;
			port_mp_send(kChannel, &sState, (int)sizeof(sState), 1, -1);
			sLastSend = port_mp_time_ms();
			sDirty    = false;
		}
	}
	// becoming the runner (round start, or a switch): no stale killer; after a switch, a grace
	if (myRole() == R_RUNNER && (sSeen.role[me() >= 0 && me() < PORT_MP_MAX_PLAYERS ? me() : 0] != R_RUNNER
	                             || sState.round != sSeen.round)) {
		sLastHitter = -1;
		sGraceUntil = sState.swaps != 0 ? port_mp_time_ms() + 10000.0 : 0.0;
	}
	// every Shine counted in the round is marked taken in our save (nobody counts one twice)
	if (running() && roundOn())
		for (int b = 0; b < 32; b++)
			if (u8 fresh = (u8)(sState.got[b] & ~sApplied[b])) {
				for (int k = 0; k < 8; k++)
					if (fresh & (1 << k)) {
						TFlagManager* f = TFlagManager::getInstance();
						const s32 count = f->getFlag(MSF_SHINE_COUNT); // (the HUD counter stays ours)
						if (!f->getShineFlag((u8)(b * 8 + k))) {
							f->setShineFlag((u8)(b * 8 + k));
							f->setFlag(MSF_SHINE_COUNT, count);
						}
					}
				sApplied[b] |= fresh;
			}
	if (sState.phase == MP_OFF)
		std::memset(sApplied, 0, sizeof(sApplied));
	toastChanges();
	watchDeath();
	TMario* m = gpMarioOriginal;
	if (localPlayable(m)) {
		if (myRole() == R_HUNTER && roundOn() && m->checkFlag(MARIO_FLAG_HAS_FLUDD))
			m->offFlag(MARIO_FLAG_HAS_FLUDD); // Shadow Mario has none (the next stage load gives it back)
		if (myRole() == R_RUNNER && roundOn() && !m->checkFlag(MARIO_FLAG_HAS_FLUDD) && m->mWaterGun != nullptr
		    && sState.swaps != 0) {
			m->onFlag(MARIO_FLAG_HAS_FLUDD); // a hunter who became the runner gets FLUDD back
			m->mWaterGun->resetWaterToFull();
		}
		hunterHits(m);
		// SMS_PORT_MP_MANHUNT_TEST=1 (tests): the runner reports one Shine (twice: counted once)
		static const bool sTest = std::getenv("SMS_PORT_MP_MANHUNT_TEST") != nullptr;
		static int sTestTerm    = -1;
		const int term          = sState.round * 256 + sState.swaps;
		if (sTest && myRole() == R_RUNNER && sState.phase == MP_PLAY && sTestTerm != term) {
			sTestTerm = term;
			std::fprintf(stderr, "[mp-manhunt] test: the runner reports Shine %d twice\n", 5 + sState.swaps);
			port_mp_manhunt_shine_got(5 + sState.swaps);
			port_mp_manhunt_shine_got(5 + sState.swaps);
		}
	}
	// the hunters' pads stay idle during the runner's head start
	const bool held = sState.phase == MP_HEAD && myRole() == R_HUNTER;
	if (held != sHeld) {
		sHeld = held;
		port_mp_request_input(held ? 1 : 0);
	}
}

struct Register {
	Register() { port_mp_register_game_frame(gameFrame); }
} sRegister;

} // namespace

extern "C" {

int port_mp_manhunt_running(void) { return running() ? 1 : 0; }

// port_mp_prophunt.cpp's looks: 1 = this player is a Shadow Mario hunter now.
int port_mp_manhunt_shadow(int slot)
{
	return running() && roundOn() && slot >= 0 && slot < PORT_MP_MAX_PLAYERS && sState.role[slot] == R_HUNTER;
}

// port_mp_interact.cpp: 1 = no stomps or ground pounds on this player (the runner: hunters hurt
// him through Manhunt's own hits, which a knock-back's invincibility frames would swallow).
int port_mp_manhunt_protect(int slot) { return running() && roundOn() && slot == sState.runner; }

// port_mp_game.cpp (the shine-get demo): the runner's Shines count.
void port_mp_manhunt_shine_got(int shineId)
{
	if (!running() || !roundOn() || myRole() != R_RUNNER)
		return;
	if (port_mp_is_host()) {
		hostShine(me(), shineId);
	} else {
		ShineMsg msg = { MSG_SHINE, (u8)shineId };
		port_mp_send(kChannel, &msg, (int)sizeof(msg), 1, -1);
	}
}

// port_mp_arena.cpp (every stage change in Manhunt): a hunter entering the runner's level goes into
// the runner's episode (no episode select). Returns 1 when it changed the destination.
int port_mp_manhunt_route(int from, int* nextState, unsigned char* stage, unsigned char* scenario)
{
	if (!roundOn() || myRole() != R_HUNTER || sState.runner >= PORT_MP_MAX_PLAYERS)
		return 0;
	const bool title    = *nextState == TApplication::APP_STATE_TITLE;
	const bool gameplay = *nextState == TApplication::APP_STATE_GAMEPLAY;
	PortMpPlayerInfo p;
	if (!(title || gameplay) || !port_mp_get_player(sState.runner, &p) || p.stage != *stage || p.stage == 1
	    || p.stage == 0xFF || (gameplay && *scenario == p.scenario) || from == *stage)
		return 0;
	std::fprintf(stderr, "[mp-manhunt] hunter enters stage %d: the runner's episode %d\n", *stage, p.scenario + 1);
	*nextState = TApplication::APP_STATE_GAMEPLAY;
	*scenario  = p.scenario;
	return 1;
}

int port_mp_manhunt_headline(char* out, int cap)
{
	char nm[32];
	const int left = secondsLeft();
	switch (sState.phase) {
	case MP_HEAD:
		std::snprintf(out, cap, "Manhunt: head start %d  (%s: 0/%d Shines)", left, nameOf(sState.runner, nm, sizeof(nm)),
		              sState.goal);
		return 1;
	case MP_PLAY:
		if (sDeadline > 0.0)
			std::snprintf(out, cap, "Manhunt%s  %d:%02d  (%s: %d/%d Shines)", sState.swap ? " (Switch)" : "", left / 60,
			              left % 60, nameOf(sState.runner, nm, sizeof(nm)), sState.shines, sState.goal);
		else
			std::snprintf(out, cap, "Manhunt  (%s: %d/%d Shines)", nameOf(sState.runner, nm, sizeof(nm)), sState.shines,
			              sState.goal);
		return 1;
	case MP_OVER:
		if (sState.winnerSlot < PORT_MP_MAX_PLAYERS && sState.winner != 2)
			std::snprintf(out, cap, "Manhunt: %s wins", nameOf(sState.winnerSlot, nm, sizeof(nm)));
		else
			std::snprintf(out, cap, "Manhunt: %s", sState.winner == 3 ? "a draw" : "the Shadow Marios win");
		return 1;
	default: return 0;
	}
}

const char* port_mp_manhunt_role(int slot)
{
	if (sState.phase == MP_OFF || slot < 0 || slot >= PORT_MP_MAX_PLAYERS)
		return nullptr;
	if (sState.swap && sState.role[slot] != R_NONE) { // Switch: everyone's own Shines
		static char sBuf[PORT_MP_MAX_PLAYERS][32];
		std::snprintf(sBuf[slot], sizeof(sBuf[slot]), "%s %d/%d", sState.role[slot] == R_RUNNER ? "Runner" : "Shadow",
		              sState.scores[slot], sState.goal);
		return sBuf[slot];
	}
	switch (sState.role[slot]) {
	case R_RUNNER: return "Runner";
	case R_HUNTER: return "Shadow Mario";
	default: return nullptr;
	}
}

} // extern "C"

#endif // SMS_PORT_AURORA
