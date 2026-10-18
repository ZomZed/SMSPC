// Multiplayer game mode "Hide and Seek" (w15; mode PORT_MP_MODE_HIDE, run by port_mp_modes.cpp).
//
// Starting it reloads everyone into the locked plaza (as None), or the match's level / the open plaza
// (the "Map" setting, port_mp_arena.cpp). Once the host is back there, it picks the seeker (the host's choice in the mode settings, or at random) and runs the
// round, sending the state to everyone on PORT_MP_CH_HIDE (reliable on every change, and once a
// second for late joiners, who watch):
//  - Hide (host setting, default 30 s, at least 10): the seeker sees a black screen and his pads
//    are idle (port_mp_request_input); the hiders run off.
//  - Seek (default 3 min): a seeker tags a hider by touching or stomping him: the seeker's client
//    sees its Mario overlap the hider (his latest snapshot) and asks the host, which decides. A
//    tagged hider becomes a seeker (host option, default on) or is out ("found").
//  - Seekers do not see the hiders' name tags (port_mp_puppet.cpp asks port_mp_hide_hide_tag).
//    The top-right list shows the phase, the time left and each player's role.
//  - The round ends when no hider is left (seekers win) or the time runs out (the hiders still
//    hidden win); the winners show for 6 s, then everyone goes back to the lobby (and AutoStart
//    begins the next cycle if on: port_mp_modes_round_over).
#ifdef SMS_PORT_AURORA

#include "port_mp.h"

#include <Player/Mario.hpp>
#include <System/Application.hpp>
#include <System/MarDirector.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" void port_mp_toast(const char* text);                                  // port_mp_menu.cpp
extern "C" void port_mp_hide_settings(int* hideS, int* seekS, int* convert, int* pick); // port_mp_modes.cpp
extern "C" void port_mp_modes_round_over(void);                                   // port_mp_modes.cpp
extern "C" int port_mp_home_stage(void); // port_mp_arena.cpp: the plaza, or a single-level match's level

namespace {

const uint8_t kChannel = PORT_MP_CH_HIDE;
enum Phase : u8 { HP_OFF = 0, HP_HIDE = 1, HP_SEEK = 2, HP_OVER = 3 };
enum Role : u8 { R_NONE = 0, R_HIDER = 1, R_SEEKER = 2, R_FOUND = 3 };
enum : u8 { MSG_STATE = 1, MSG_TAG = 2, MSG_DIED = 3 };

#pragma pack(push, 1)
struct State {
	u8 type; // MSG_STATE
	u8 phase;
	u8 round;  // +1 per round (clients toast on a new one)
	u8 winner; // HP_OVER: 1 hiders, 2 seekers
	u32 leftMs;
	u8 role[PORT_MP_MAX_PLAYERS];
	u8 died[PORT_MP_MAX_PLAYERS]; // 1 = lost his hider role by dying (toasts)
};
struct Tag {
	u8 type; // MSG_TAG
	u8 slot;
};
#pragma pack(pop)

State sState = { MSG_STATE, HP_OFF, 0, 0, 0, {}, {} };
State sSeen; // what this client last acted on (toasts)
double sDeadline;
double sLastSend;
bool sDirty;
double sModeSeenAt; // host: when the running mode became Hide and Seek
bool sBlind;        // our pads are held idle
double sTagCooldown;

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
	int hideS, seekS, convert, pick;
	port_mp_hide_settings(&hideS, &seekS, &convert, &pick);
	int slots[PORT_MP_MAX_PLAYERS], n = 0;
	for (int i = 0; i < PORT_MP_MAX_PLAYERS; i++)
		if (used(i))
			slots[n++] = i;
	std::memset(sState.role, R_NONE, sizeof(sState.role));
	std::memset(sState.died, 0, sizeof(sState.died));
	if (n == 0)
		return;
	int seeker = pick >= 0 && pick < PORT_MP_MAX_PLAYERS && used(pick) ? pick : slots[std::rand() % n];
	for (int i = 0; i < n; i++)
		sState.role[slots[i]] = slots[i] == seeker ? R_SEEKER : R_HIDER;
	sState.round++;
	sState.winner = 0;
	setPhase(HP_HIDE, hideS);
	char nm[32];
	std::fprintf(stderr, "[mp-hide] round %d: seeker %s (slot %d), %d hiders, hide %d s\n", sState.round,
	             nameOf(seeker, nm, sizeof(nm)), seeker, n - 1, hideS);
}

void hostTag(int seeker, int target)
{
	if (sState.phase != HP_SEEK || seeker < 0 || target < 0 || seeker >= PORT_MP_MAX_PLAYERS
	    || target >= PORT_MP_MAX_PLAYERS || sState.role[seeker] != R_SEEKER || sState.role[target] != R_HIDER)
		return;
	int hideS, seekS, convert, pick;
	port_mp_hide_settings(&hideS, &seekS, &convert, &pick);
	sState.role[target] = convert ? R_SEEKER : R_FOUND;
	sDirty              = true;
	char a[32], b[32];
	std::fprintf(stderr, "[mp-hide] %s tagged %s\n", nameOf(seeker, a, sizeof(a)), nameOf(target, b, sizeof(b)));
}

void hostFrame()
{
	PortMpSessionSettings s;
	port_mp_get_settings(&s);
	if (s.gameMode != PORT_MP_MODE_HIDE) {
		if (sState.phase != HP_OFF) {
			std::memset(sState.role, R_NONE, sizeof(sState.role));
			setPhase(HP_OFF, 0);
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
	case HP_OFF: {
		// the round begins once the host is back in the reloaded plaza or level (everyone reloads alike)
		TMarDirector* d = gpMarDirector;
		if (now - sModeSeenAt > 3000.0 && d != nullptr && d->mState == TMarDirector::STATE_UNK4
		    && SMSGetApplication()->mCurrArea.getStage() == port_mp_home_stage())
			hostStartRound();
		break;
	}
	case HP_HIDE:
		if (count(R_HIDER) == 0) { // every hider died before the seek began
			sState.winner = 2;
			setPhase(HP_OVER, 6);
		} else if (now >= sDeadline) {
			int hideS, seekS, convert, pick;
			port_mp_hide_settings(&hideS, &seekS, &convert, &pick);
			setPhase(HP_SEEK, seekS);
		}
		break;
	case HP_SEEK:
		if (count(R_HIDER) == 0 || count(R_SEEKER) == 0) {
			sState.winner = count(R_HIDER) == 0 ? 2 : 1;
			setPhase(HP_OVER, 6);
		} else if (now >= sDeadline) {
			sState.winner = 1;
			setPhase(HP_OVER, 6);
		}
		break;
	case HP_OVER:
		if (now >= sDeadline) {
			std::memset(sState.role, R_NONE, sizeof(sState.role));
			setPhase(HP_OFF, 0);
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
	char t[160], nm[32];
	if (sState.round != sSeen.round && sState.phase == HP_HIDE && me >= 0) {
		int seeker = -1;
		for (int i = 0; i < PORT_MP_MAX_PLAYERS; i++)
			if (sState.role[i] == R_SEEKER)
				seeker = i;
		if (sState.role[me] == R_SEEKER)
			std::snprintf(t, sizeof(t), "Hide and Seek: you are the seeker. Wait %d s...", secondsLeft());
		else
			std::snprintf(t, sizeof(t), "Hide and Seek: %s seeks. Hide! (%d s)", nameOf(seeker, nm, sizeof(nm)),
			              secondsLeft());
		port_mp_toast(t);
	}
	if (sState.phase == HP_SEEK && sSeen.phase != HP_SEEK)
		port_mp_toast(me >= 0 && sState.role[me] == R_SEEKER ? "Seek! Touch or stomp the hiders." : "The seeker is coming!");
	if ((sState.phase == HP_HIDE || sState.phase == HP_SEEK) && sSeen.phase == sState.phase)
		for (int i = 0; i < PORT_MP_MAX_PLAYERS; i++)
			if (sSeen.role[i] == R_HIDER && sState.role[i] != R_HIDER && sState.role[i] != R_NONE) {
				if (sState.died[i])
					std::snprintf(t, sizeof(t), i == me ? "You died: %s" : "%s died and %s.",
					              i == me ? (sState.role[i] == R_SEEKER ? "now you seek." : "you are out.")
					                      : nameOf(i, nm, sizeof(nm)),
					              sState.role[i] == R_SEEKER ? "seeks now" : "is out");
				else
					std::snprintf(t, sizeof(t), "%s was found%s.", nameOf(i, nm, sizeof(nm)),
					              sState.role[i] == R_SEEKER ? " and seeks now" : "");
				port_mp_toast(t);
			}
	if (sState.phase == HP_OVER && sSeen.phase != HP_OVER) {
		if (sState.winner == 2) {
			std::snprintf(t, sizeof(t), "Everyone was found: the seekers win!");
		} else {
			int n = std::snprintf(t, sizeof(t), "Time is up: ");
			bool any = false;
			for (int i = 0; i < PORT_MP_MAX_PLAYERS && n < (int)sizeof(t) - 40; i++)
				if (sSeen.role[i] == R_HIDER || sState.role[i] == R_HIDER) {
					if (sState.role[i] != R_HIDER)
						continue;
					n += std::snprintf(t + n, sizeof(t) - n, "%s%s", any ? ", " : "", nameOf(i, nm, sizeof(nm)));
					any = true;
				}
			std::snprintf(t + n, sizeof(t) - n, "%s", any ? " win!" : "the hiders win!");
		}
		port_mp_toast(t);
		std::fprintf(stderr, "[mp-hide] %s\n", t);
	}
	sSeen = sState;
}

// A hider who dies (a lost life, a game over) is caught as if tagged: he seeks, or is out.
void hostDied(int slot)
{
	if ((sState.phase != HP_HIDE && sState.phase != HP_SEEK) || slot < 0 || slot >= PORT_MP_MAX_PLAYERS
	    || sState.role[slot] != R_HIDER)
		return;
	int hideS, seekS, convert, pick;
	port_mp_hide_settings(&hideS, &seekS, &convert, &pick);
	sState.role[slot] = convert ? R_SEEKER : R_FOUND;
	sState.died[slot] = 1;
	sDirty            = true;
	char a[32];
	std::fprintf(stderr, "[mp-hide] %s died: %s\n", nameOf(slot, a, sizeof(a)), convert ? "seeks now" : "out");
}

// Our Mario died (the director's miss state): tell the host.
void watchDeath()
{
	static int sLastState = -1;
	const int st = gpMarDirector != nullptr ? gpMarDirector->mState : -1;
	const bool died = st == TMarDirector::STATE_UNK7 && sLastState != TMarDirector::STATE_UNK7;
	sLastState = st;
	const int me = port_mp_local_slot();
	if (!died || me < 0 || (sState.phase != HP_HIDE && sState.phase != HP_SEEK) || sState.role[me] != R_HIDER)
		return;
	if (port_mp_is_host()) {
		hostDied(me);
	} else {
		const u8 m = MSG_DIED;
		port_mp_send(kChannel, &m, 1, 1, -1);
	}
}

// The seeker's client: its Mario overlapping a hider (his latest snapshot) is a tag.
void seekerTags()
{
	const int me = port_mp_local_slot();
	if (sState.phase != HP_SEEK || me < 0 || sState.role[me] != R_SEEKER || gpMarioOriginal == nullptr)
		return;
	const double now = port_mp_time_ms();
	if (now < sTagCooldown)
		return;
	// SMS_PORT_MP_HIDE_TEST=1 (debug, tests): the seeker jumps onto a hider once, to test a tag.
	static const bool sTest = std::getenv("SMS_PORT_MP_HIDE_TEST") != nullptr;
	static u8 sTestRound = 0xFF;
	if (sTest && sTestRound != sState.round)
		for (int i = 0; i < PORT_MP_MAX_PLAYERS; i++) {
			PortMpSnapshotRx rx;
			if (i != me && sState.role[i] == R_HIDER && port_mp_get_snapshots(i, &rx, 1) > 0) {
				gpMarioOriginal->mPosition.set(rx.snap.pos[0], rx.snap.pos[1] + 40.0f, rx.snap.pos[2]);
				sTestRound = sState.round;
				std::fprintf(stderr, "[mp-hide] test: seeker jumps onto slot %d\n", i);
				break;
			}
		}
	const JGeometry::TVec3<f32>& m = gpMarioOriginal->mPosition;
	for (int i = 0; i < PORT_MP_MAX_PLAYERS; i++) {
		if (i == me || sState.role[i] != R_HIDER)
			continue;
		PortMpSnapshotRx rx;
		if (port_mp_get_snapshots(i, &rx, 1) < 1 || now - rx.rxMs > 1000.0 || !port_mp_same_stage(i))
			continue;
		const float dx = rx.snap.pos[0] - m.x, dz = rx.snap.pos[2] - m.z, dy = rx.snap.pos[1] - m.y;
		if (dx * dx + dz * dz < 100.0f * 100.0f && dy > -160.0f && dy < 160.0f) {
			sTagCooldown = now + 500.0;
			if (port_mp_is_host()) {
				hostTag(me, i);
			} else {
				Tag t = { MSG_TAG, (u8)i };
				port_mp_send(kChannel, &t, (int)sizeof(t), 1, -1);
			}
			return;
		}
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
		} else if (buf[0] == MSG_TAG && n == (int)sizeof(Tag) && host) {
			hostTag(from, ((const Tag*)buf)->slot);
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
	seekerTags();
	// the seeker's pads stay idle while the others hide
	const int me     = port_mp_local_slot();
	const bool blind = sState.phase == HP_HIDE && me >= 0 && sState.role[me] == R_SEEKER;
	if (blind != sBlind) {
		sBlind = blind;
		port_mp_request_input(blind ? 1 : 0);
	}
}

struct Register {
	Register() { port_mp_register_game_frame(gameFrame); }
} sRegister;

} // namespace

extern "C" {

// The player list: 1 = draw the black screen (we are the seeker while the others hide).
int port_mp_hide_blind(void) { return sBlind ? 1 : 0; }

// port_mp_puppet.cpp: 1 = do not show this player's name tag here (a hider, to a seeker).
int port_mp_hide_hide_tag(int slot)
{
	const int me = port_mp_local_slot();
	return sState.phase != HP_OFF && me >= 0 && slot >= 0 && slot < PORT_MP_MAX_PLAYERS
	       && sState.role[me] == R_SEEKER && sState.role[slot] == R_HIDER;
}

// The player list: the line above the players (1 = written).
int port_mp_hide_headline(char* out, int cap)
{
	const int left = secondsLeft();
	switch (sState.phase) {
	case HP_HIDE: std::snprintf(out, cap, "Hide and Seek: hide!  %d:%02d", left / 60, left % 60); return 1;
	case HP_SEEK:
		std::snprintf(out, cap, "Hide and Seek: seek  %d:%02d  (%d hiding)", left / 60, left % 60, count(R_HIDER));
		return 1;
	case HP_OVER: std::snprintf(out, cap, "Hide and Seek: %s win", sState.winner == 2 ? "seekers" : "hiders"); return 1;
	default: return 0;
	}
}

// port_mp_arena.cpp: 1 = we are a hider in a running round (the exit lock holds hiders).
int port_mp_hide_local_hider(void)
{
	const int me = port_mp_local_slot();
	return (sState.phase == HP_HIDE || sState.phase == HP_SEEK) && me >= 0 && me < PORT_MP_MAX_PLAYERS
	       && sState.role[me] == R_HIDER;
}

// The player list: a player's role in the round (null: none).
const char* port_mp_hide_role(int slot)
{
	if (sState.phase == HP_OFF || slot < 0 || slot >= PORT_MP_MAX_PLAYERS)
		return nullptr;
	switch (sState.role[slot]) {
	case R_HIDER: return "Hider";
	case R_SEEKER: return "Seeker";
	case R_FOUND: return "Found";
	default: return nullptr;
	}
}

} // extern "C"

#endif // SMS_PORT_AURORA
