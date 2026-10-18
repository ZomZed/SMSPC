// Multiplayer game mode "Freeze Tag" (mode PORT_MP_MODE_FREEZE, run by port_mp_modes.cpp like Hide and
// Seek, port_mp_hideseek.cpp, whose structure this follows).
//
// Starting it reloads everyone into the locked plaza (as None), or the match's level / the open plaza
// (the "Map" setting, port_mp_arena.cpp). Once the host is back there it
// picks the taggers (the host's choice in the mode settings, or at random; how many is a setting) and
// runs the round, sending the state to everyone on PORT_MP_CH_FREEZE (reliable on every change, and
// once a second for late joiners, who watch):
//  - Get ready (host setting, default 10 s): the taggers' pads are idle (port_mp_request_input) while
//    the runners scatter.
//  - Play (default 3 min): a tagger freezes a runner by touching or stomping him (the tagger's client
//    sees its Mario overlap the runner's latest snapshot and asks the host, which decides). A frozen
//    runner's pads are idle where he stands, and his name tag turns ice blue for everyone. Another
//    runner thaws him by touching him; a thawed runner cannot be frozen again for 2 s.
//  - The round ends when every runner is frozen (the taggers win) or the time runs out (the runners
//    win); the result shows for 6 s, then everyone goes back to the lobby (and AutoStart begins the
//    next cycle if on: port_mp_modes_round_over).
// The top-right list shows the phase, the time left, how many run and how many are frozen, and each
// player's role (Tagger / Runner / Frozen).
#ifdef SMS_PORT_AURORA

#include "port_mp.h"

#include <Player/Mario.hpp>
#include <System/Application.hpp>
#include <System/MarDirector.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" void port_mp_toast(const char* text);                                              // port_mp_menu.cpp
extern "C" void port_mp_freeze_settings(int* readyS, int* playS, int* taggers, int* pick);    // port_mp_modes.cpp
extern "C" int port_mp_freeze_death_out(void); // port_mp_modes.cpp: 1 = a runner who dies is out
extern "C" void port_mp_modes_round_over(void);                                               // port_mp_modes.cpp
extern "C" int port_mp_home_stage(void); // port_mp_arena.cpp: the plaza, or a single-level match's level

namespace {

const uint8_t kChannel   = PORT_MP_CH_FREEZE;
const float kTouchRadius = 100.0f; // horizontal; and within 160 vertically (as Hide and Seek's tag)
const double kThawGraceMs = 2000.0;
enum Phase : u8 { FP_OFF = 0, FP_READY = 1, FP_PLAY = 2, FP_OVER = 3 };
enum Role : u8 { R_NONE = 0, R_RUNNER = 1, R_TAGGER = 2, R_FROZEN = 3, R_OUT = 4 }; // R_OUT: died (setting)
enum : u8 { MSG_STATE = 1, MSG_FREEZE = 2, MSG_THAW = 3, MSG_DIED = 4 };

#pragma pack(push, 1)
struct State {
	u8 type; // MSG_STATE
	u8 phase;
	u8 round;  // +1 per round (clients toast on a new one)
	u8 winner; // FP_OVER: 1 runners, 2 taggers
	u32 leftMs;
	u8 role[PORT_MP_MAX_PLAYERS];
	u8 thaws[PORT_MP_MAX_PLAYERS]; // how many times each runner was thawed this round (toasts)
	u8 died[PORT_MP_MAX_PLAYERS];  // 1 = lost his runner role by dying (toasts)
};
struct Touch {
	u8 type; // MSG_FREEZE or MSG_THAW
	u8 slot;
};
#pragma pack(pop)

State sState = { MSG_STATE, FP_OFF, 0, 0, 0, {}, {}, {} };
State sSeen; // what this client last acted on (toasts)
double sDeadline;
double sLastSend;
bool sDirty;
double sModeSeenAt;                       // host: when the running mode became Freeze Tag
double sGraceUntil[PORT_MP_MAX_PLAYERS];  // host: a thawed runner cannot be frozen before this
bool sHeld;                               // our pads are held idle (a tagger getting ready, or frozen)
double sTouchCooldown;

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

int count(u8 role)
{
	int n = 0;
	for (int i = 0; i < PORT_MP_MAX_PLAYERS; i++)
		n += sState.role[i] == role && used(i);
	return n;
}

void setPhase(u8 phase, int seconds)
{
	sState.phase = phase;
	sDeadline    = seconds > 0 ? port_mp_time_ms() + seconds * 1000.0 : 0.0;
	sDirty       = true;
}

// ---- host ------------------------------------------------------------------------------------
void hostStartRound()
{
	int readyS, playS, taggers, pick;
	port_mp_freeze_settings(&readyS, &playS, &taggers, &pick);
	int slots[PORT_MP_MAX_PLAYERS], n = 0;
	for (int i = 0; i < PORT_MP_MAX_PLAYERS; i++)
		if (used(i))
			slots[n++] = i;
	std::memset(sState.role, R_NONE, sizeof(sState.role));
	std::memset(sState.thaws, 0, sizeof(sState.thaws));
	std::memset(sState.died, 0, sizeof(sState.died));
	if (n == 0)
		return;
	for (int i = 0; i < n; i++)
		sState.role[slots[i]] = R_RUNNER;
	// at least one runner stays: a lone player is the tagger of nobody (the round ends at once)
	int want = taggers < 1 ? 1 : taggers;
	if (want > n - 1)
		want = n > 1 ? n - 1 : 1;
	int chosen = 0;
	if (pick >= 0 && pick < PORT_MP_MAX_PLAYERS && used(pick)) {
		sState.role[pick] = R_TAGGER;
		chosen            = 1;
	}
	for (int guard = 0; chosen < want && guard < 1000; guard++) {
		const int s = slots[std::rand() % n];
		if (sState.role[s] == R_RUNNER) {
			sState.role[s] = R_TAGGER;
			chosen++;
		}
	}
	for (int i = 0; i < PORT_MP_MAX_PLAYERS; i++)
		sGraceUntil[i] = 0.0;
	sState.round++;
	sState.winner = 0;
	setPhase(FP_READY, readyS);
	std::fprintf(stderr, "[mp-freeze] round %d: %d tagger(s), %d runner(s), get ready %d s, play %d s\n",
	             sState.round, chosen, n - chosen, readyS, playS);
}

void hostFreeze(int tagger, int target)
{
	if (sState.phase != FP_PLAY || tagger < 0 || target < 0 || tagger >= PORT_MP_MAX_PLAYERS
	    || target >= PORT_MP_MAX_PLAYERS || sState.role[tagger] != R_TAGGER || sState.role[target] != R_RUNNER
	    || port_mp_time_ms() < sGraceUntil[target])
		return;
	sState.role[target] = R_FROZEN;
	sDirty              = true;
	char a[32], b[32];
	std::fprintf(stderr, "[mp-freeze] %s froze %s\n", nameOf(tagger, a, sizeof(a)), nameOf(target, b, sizeof(b)));
}

void hostThaw(int runner, int target)
{
	if (sState.phase != FP_PLAY || runner < 0 || target < 0 || runner >= PORT_MP_MAX_PLAYERS
	    || target >= PORT_MP_MAX_PLAYERS || runner == target || sState.role[runner] != R_RUNNER
	    || sState.role[target] != R_FROZEN)
		return;
	sState.role[target] = R_RUNNER;
	if (sState.thaws[target] < 255)
		sState.thaws[target]++;
	sGraceUntil[target] = port_mp_time_ms() + kThawGraceMs;
	sDirty              = true;
	char a[32], b[32];
	std::fprintf(stderr, "[mp-freeze] %s thawed %s\n", nameOf(runner, a, sizeof(a)), nameOf(target, b, sizeof(b)));
}

// A runner (or a frozen one) who dies (a lost life, a game over) joins the taggers, or is out (the
// host's setting, port_mp_freeze_death_out).
void hostDied(int slot)
{
	if ((sState.phase != FP_READY && sState.phase != FP_PLAY) || slot < 0 || slot >= PORT_MP_MAX_PLAYERS
	    || (sState.role[slot] != R_RUNNER && sState.role[slot] != R_FROZEN))
		return;
	const bool out    = port_mp_freeze_death_out() != 0;
	sState.role[slot] = out ? R_OUT : R_TAGGER;
	sState.died[slot] = 1;
	sDirty            = true;
	char a[32];
	std::fprintf(stderr, "[mp-freeze] %s died: %s\n", nameOf(slot, a, sizeof(a)), out ? "out" : "tags now");
}

// Our Mario died (the director's miss state): tell the host.
void watchDeath()
{
	static int sLastState = -1;
	const int st = gpMarDirector != nullptr ? gpMarDirector->mState : -1;
	const bool died = st == TMarDirector::STATE_UNK7 && sLastState != TMarDirector::STATE_UNK7;
	sLastState = st;
	const int me = port_mp_local_slot();
	if (!died || me < 0 || (sState.phase != FP_READY && sState.phase != FP_PLAY)
	    || (sState.role[me] != R_RUNNER && sState.role[me] != R_FROZEN))
		return;
	if (port_mp_is_host()) {
		hostDied(me);
	} else {
		const u8 m = MSG_DIED;
		port_mp_send(kChannel, &m, 1, 1, -1);
	}
}

void hostFrame()
{
	PortMpSessionSettings s;
	port_mp_get_settings(&s);
	if (s.gameMode != PORT_MP_MODE_FREEZE) {
		if (sState.phase != FP_OFF) {
			std::memset(sState.role, R_NONE, sizeof(sState.role));
			setPhase(FP_OFF, 0);
		}
		sModeSeenAt = 0.0;
		return;
	}
	const double now = port_mp_time_ms();
	if (sModeSeenAt == 0.0)
		sModeSeenAt = now;
	// players who left are out of the round
	for (int i = 0; i < PORT_MP_MAX_PLAYERS; i++)
		if (sState.role[i] != R_NONE && !used(i)) {
			sState.role[i] = R_NONE;
			sDirty         = true;
		}
	switch (sState.phase) {
	case FP_OFF: {
		// the round begins once the host is back in the reloaded plaza or level (everyone reloads alike)
		TMarDirector* d = gpMarDirector;
		if (now - sModeSeenAt > 3000.0 && d != nullptr && d->mState == TMarDirector::STATE_UNK4
		    && SMSGetApplication()->mCurrArea.getStage() == port_mp_home_stage())
			hostStartRound();
		break;
	}
	case FP_READY:
		if (count(R_RUNNER) + count(R_FROZEN) == 0) { // every runner died before the play began
			sState.winner = 2;
			setPhase(FP_OVER, 6);
		} else if (now >= sDeadline) {
			int readyS, playS, taggers, pick;
			port_mp_freeze_settings(&readyS, &playS, &taggers, &pick);
			setPhase(FP_PLAY, playS);
		}
		break;
	case FP_PLAY:
		if (count(R_RUNNER) == 0 || count(R_TAGGER) == 0) {
			sState.winner = count(R_RUNNER) == 0 ? 2 : 1;
			setPhase(FP_OVER, 6);
		} else if (now >= sDeadline) {
			sState.winner = 1;
			setPhase(FP_OVER, 6);
		}
		break;
	case FP_OVER:
		if (now >= sDeadline) {
			std::memset(sState.role, R_NONE, sizeof(sState.role));
			setPhase(FP_OFF, 0);
			sModeSeenAt = 0.0;
			port_mp_modes_round_over();
		}
		break;
	}
}

// ---- every client ----------------------------------------------------------------------------
void toastChanges()
{
	const int me = port_mp_local_slot();
	char t[160], nm[32], nm2[32];
	if (sState.round != sSeen.round && sState.phase == FP_READY && me >= 0) {
		if (sState.role[me] == R_TAGGER)
			std::snprintf(t, sizeof(t), "Freeze Tag: you are it! Wait %d s...", secondsLeft());
		else
			std::snprintf(t, sizeof(t), "Freeze Tag: %d tagger(s). Run! (%d s)", count(R_TAGGER), secondsLeft());
		port_mp_toast(t);
	}
	if (sState.phase == FP_PLAY && sSeen.phase != FP_PLAY)
		port_mp_toast(me >= 0 && sState.role[me] == R_TAGGER ? "Go! Touch the runners to freeze them."
		                                                      : "The taggers are coming! Touch frozen friends to thaw them.");
	if ((sState.phase == FP_READY || sState.phase == FP_PLAY) && sSeen.phase == sState.phase)
		for (int i = 0; i < PORT_MP_MAX_PLAYERS; i++)
			if (sState.died[i] && !sSeen.died[i]) {
				const bool out = sState.role[i] == R_OUT;
				if (i == me)
					std::snprintf(t, sizeof(t), out ? "You died: you are out." : "You died: now you are it!");
				else
					std::snprintf(t, sizeof(t), out ? "%s died and is out." : "%s died and tags now!",
					              nameOf(i, nm, sizeof(nm)));
				port_mp_toast(t);
			}
	if (sState.phase == FP_PLAY && sSeen.phase == FP_PLAY)
		for (int i = 0; i < PORT_MP_MAX_PLAYERS; i++) {
			if (sSeen.role[i] == R_RUNNER && sState.role[i] == R_FROZEN) {
				std::snprintf(t, sizeof(t), i == me ? "You are frozen! Wait for a friend." : "%s is frozen!",
				              nameOf(i, nm, sizeof(nm)));
				port_mp_toast(t);
			} else if (sSeen.role[i] == R_FROZEN && sState.role[i] == R_RUNNER) {
				std::snprintf(t, sizeof(t), i == me ? "You were thawed: run!" : "%s was thawed.",
				              nameOf(i, nm2, sizeof(nm2)));
				port_mp_toast(t);
			}
		}
	if (sState.phase == FP_OVER && sSeen.phase != FP_OVER) {
		if (sState.winner == 2)
			std::snprintf(t, sizeof(t), "No runners left: the taggers win!");
		else
			std::snprintf(t, sizeof(t), "Time is up: the runners win! (%d still running)", count(R_RUNNER));
		port_mp_toast(t);
		std::fprintf(stderr, "[mp-freeze] %s\n", t);
	}
	sSeen = sState;
}

// Our Mario overlapping a player's latest snapshot (same stage, a fresh snapshot).
bool touching(int slot)
{
	if (gpMarioOriginal == nullptr)
		return false;
	PortMpSnapshotRx rx;
	if (port_mp_get_snapshots(slot, &rx, 1) < 1 || port_mp_time_ms() - rx.rxMs > 1000.0 || !port_mp_same_stage(slot))
		return false;
	const JGeometry::TVec3<f32>& m = gpMarioOriginal->mPosition;
	const float dx = rx.snap.pos[0] - m.x, dz = rx.snap.pos[2] - m.z, dy = rx.snap.pos[1] - m.y;
	return dx * dx + dz * dz < kTouchRadius * kTouchRadius && dy > -160.0f && dy < 160.0f;
}

void send(u8 type, int target)
{
	const int me = port_mp_local_slot();
	if (port_mp_is_host()) {
		if (type == MSG_FREEZE)
			hostFreeze(me, target);
		else
			hostThaw(me, target);
		return;
	}
	Touch t = { type, (u8)target };
	port_mp_send(kChannel, &t, (int)sizeof(t), 1, -1);
}

// A tagger's client freezes the runners it touches; a runner's client thaws the frozen it touches.
void touches()
{
	const int me = port_mp_local_slot();
	if (sState.phase != FP_PLAY || me < 0)
		return;
	const double now = port_mp_time_ms();
	if (now < sTouchCooldown)
		return;
	const u8 mine = sState.role[me];
	const u8 want = mine == R_TAGGER ? R_RUNNER : mine == R_RUNNER ? R_FROZEN : R_NONE;
	if (want == R_NONE)
		return;
	// SMS_PORT_MP_FREEZE_TEST=1 (debug, tests): a tagger jumps onto a runner once per round.
	static const bool sTest = std::getenv("SMS_PORT_MP_FREEZE_TEST") != nullptr;
	static u8 sTestRound = 0xFF;
	// (and a runner onto a frozen player: a freeze, then a thaw)
	if (sTest && sTestRound != sState.round && gpMarioOriginal != nullptr)
		for (int i = 0; i < PORT_MP_MAX_PLAYERS; i++) {
			PortMpSnapshotRx rx;
			if (i != me && sState.role[i] == want && port_mp_get_snapshots(i, &rx, 1) > 0) {
				gpMarioOriginal->mPosition.set(rx.snap.pos[0], rx.snap.pos[1] + 40.0f, rx.snap.pos[2]);
				sTestRound = sState.round;
				std::fprintf(stderr, "[mp-freeze] test: %s jumps onto slot %d\n", mine == R_TAGGER ? "tagger" : "runner", i);
				break;
			}
		}
	for (int i = 0; i < PORT_MP_MAX_PLAYERS; i++)
		if (i != me && sState.role[i] == want && touching(i)) {
			sTouchCooldown = now + 500.0;
			send(want == R_RUNNER ? MSG_FREEZE : MSG_THAW, i);
			return;
		}
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
		} else if (buf[0] == MSG_FREEZE && n == (int)sizeof(Touch) && host) {
			hostFreeze(from, ((const Touch*)buf)->slot);
		} else if (buf[0] == MSG_THAW && n == (int)sizeof(Touch) && host) {
			hostThaw(from, ((const Touch*)buf)->slot);
		} else if (buf[0] == MSG_DIED && n == 1 && host) {
			hostDied(from);
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
	toastChanges();
	watchDeath();
	touches();
	// a tagger's pads stay idle while the runners get ready; a frozen runner's while he is frozen
	const int me    = port_mp_local_slot();
	const bool held = me >= 0
	                  && ((sState.phase == FP_READY && sState.role[me] == R_TAGGER)
	                      || (sState.phase == FP_PLAY && sState.role[me] == R_FROZEN));
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

// The player list: the line above the players (1 = written).
int port_mp_freeze_headline(char* out, int cap)
{
	const int left = secondsLeft();
	switch (sState.phase) {
	case FP_READY: std::snprintf(out, cap, "Freeze Tag: get ready  %d:%02d", left / 60, left % 60); return 1;
	case FP_PLAY:
		std::snprintf(out, cap, "Freeze Tag  %d:%02d  (%d running, %d frozen)", left / 60, left % 60, count(R_RUNNER),
		              count(R_FROZEN));
		return 1;
	case FP_OVER: std::snprintf(out, cap, "Freeze Tag: %s win", sState.winner == 2 ? "taggers" : "runners"); return 1;
	default: return 0;
	}
}

// port_mp_arena.cpp: 1 = we run (or stand frozen) in a running round (the exit lock holds runners).
int port_mp_freeze_local_runner(void)
{
	const int me = port_mp_local_slot();
	return (sState.phase == FP_READY || sState.phase == FP_PLAY) && me >= 0 && me < PORT_MP_MAX_PLAYERS
	       && (sState.role[me] == R_RUNNER || sState.role[me] == R_FROZEN);
}

// The player list: a player's role in the round (null: none).
const char* port_mp_freeze_role(int slot)
{
	if (sState.phase == FP_OFF || slot < 0 || slot >= PORT_MP_MAX_PLAYERS)
		return nullptr;
	switch (sState.role[slot]) {
	case R_RUNNER: return "Runner";
	case R_TAGGER: return "Tagger";
	case R_FROZEN: return "Frozen";
	case R_OUT: return "Out";
	default: return nullptr;
	}
}

// port_mp_puppet.cpp: a frozen player's name tag is ice blue (1 = *rgb changed).
int port_mp_freeze_tag_rgb(int slot, u32* rgb)
{
	if (sState.phase != FP_PLAY || slot < 0 || slot >= PORT_MP_MAX_PLAYERS || sState.role[slot] != R_FROZEN)
		return 0;
	*rgb = 0x9FE8FFu;
	return 1;
}

} // extern "C"

#endif // SMS_PORT_AURORA
