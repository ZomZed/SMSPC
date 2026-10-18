// Multiplayer game mode "Cops and Robbers" (mode PORT_MP_MODE_CNR, run by port_mp_modes.cpp like the
// other round modes).
//  - Teams: cops (host setting, default about a third of the players, at least one) and robbers.
//  - Every level is open on a copy of the save without its Shine flags (as Manhunt): the robbers
//    steal coins (yellow 1, red 2, blue 5) and Shine Sprites (each once per round) for a team quota
//    (host settings, default 100 coins and 3 Shines). Cops' pickups do not count.
//  - A cop knocks a robber over (the interaction module's stomp, ground pound or FLUDD push) and
//    touches him while he is down (or within 1.5 s of getting up): the robber is arrested. The
//    robber's own client decides (it knows when its Mario is down) and tells the host.
//  - Arrested robbers are sent to the prison in Delfino Plaza, in front of the police station, and
//    held inside it (nor can they leave the plaza) until a free robber touches them (3 s of
//    protection after that). The prison is the police station's interior.
//  - A free robber who rings the police station's bell (jumping into it, as in the original game)
//    opens the prison: every prisoner is released, and everyone's police bell swings. Then the bell
//    needs 30 s before it can break anyone out again.
//  - Robbers win with both quotas met; cops win when every robber is in prison at once or the time
//    (default 12 min) runs out. Head start (default 15 s): the cops' pads are idle.
// State on PORT_MP_CH_CNR from the host (reliable on every change, once a second).
#ifdef SMS_PORT_AURORA

#include "port_mp.h"

#include <Map/Map.hpp>
#include <Map/MapData.hpp>
#include <MoveBG/Item.hpp>
#include <MoveBG/MapObjDolpic.hpp>
#include <Player/Mario.hpp>
#include <Player/MarioAccess.hpp>
#include <Player/MarioStatus.hpp>
#include <Strategic/ActorTypes.hpp>
#include <System/Application.hpp>
#include <System/FlagManager.hpp>
#include <System/MarDirector.hpp>

#include <dolphin/gx.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" void port_mp_toast(const char* text);                         // port_mp_menu.cpp
extern "C" void port_mp_cnr_settings(int* cops, int* coinQuota, int* shineQuota, int* timeS, int* headS,
                                     int* pick);                          // port_mp_modes.cpp
extern "C" void port_mp_modes_round_over(void);                          // port_mp_modes.cpp
extern "C" int port_mp_home_stage(void);                                 // port_mp_arena.cpp
extern "C" void port_mp_home_area(const PortMpSessionSettings* s, int* stage, int* scenario);
extern "C" int port_mp_puppet_pos(int slot, float pos[3]);               // port_mp_puppet.cpp
extern "C" void port_shot_request(unsigned tag);                         // port_movetest.cpp (tests)
extern "C" void port_mp_overlay_set_cage(const float* bottom, const float* top, const unsigned char* valid, int n,
                                         const float* label, int labelValid, uint32_t rgb, const char* text);

namespace {

const uint8_t kChannel = PORT_MP_CH_CNR;
enum Phase : u8 { CR_OFF = 0, CR_HEAD = 1, CR_PLAY = 2, CR_OVER = 3 };
enum Role : u8 { R_NONE = 0, R_COP = 1, R_ROBBER = 2, R_JAILED = 3 };
enum : u8 { MSG_STATE = 1, MSG_LOOT = 2, MSG_ARRESTED = 3, MSG_FREED = 4, MSG_BELL = 5 };
const double kBellCooldownMs = 30000.0;

// The prison: the police station's interior in Delfino Plaza (under its bell, BellDolpicPolice at
// 6500 3980 5100 in dolpic10's scene): a room with its floor at y 305 and a ceiling 250-300 above,
// x 6350..6650, z 4600..5600 in the plaza's collision, its door at the z 5600 end. Prisoners are
// put in it and held inside (a margin off the walls); robbers walk in to free them.
const float kPrisonX = 6500.0f, kPrisonZ = 5050.0f;  // inside, away from the door
const float kRoomX0 = 6380.0f, kRoomX1 = 6620.0f, kRoomZ0 = 4650.0f, kRoomZ1 = 5480.0f;
const float kDoorX = 6500.0f, kDoorZ = 5650.0f;       // the label over the door
const float kTouch   = 110.0f; // touching another player (horizontal; and within 170 vertically)

bool inRoom(f32 x, f32 z) { return x >= kRoomX0 && x <= kRoomX1 && z >= kRoomZ0 && z <= kRoomZ1; }

#pragma pack(push, 1)
struct State {
	u8 type; // MSG_STATE
	u8 phase;
	u8 round;
	u8 winner; // CR_OVER: 1 robbers, 2 cops
	u32 leftMs;
	u8 role[PORT_MP_MAX_PLAYERS];
	u16 coins;      // the robbers' loot
	u8 shines;
	u8 shineQuota;
	u16 coinQuota;
	u8 lastBy;      // who did the latest arrest / release (toasts)
	u8 jailbreaks;  // +1 per bell jailbreak (every client rings its bell and toasts)
	u8 got[32];     // Shines taken this round (bits)
	u8 bellWait;    // seconds before the bell can open the prison again (0 = ready)
};
struct LootMsg {
	u8 type; // MSG_LOOT
	u8 shine; // 1 = a Shine (id), 0 = coins (amount)
	u8 value;
	u8 pad;
};
struct WhoMsg {
	u8 type; // MSG_ARRESTED / MSG_FREED
	u8 by;   // the cop who arrested / the robber who freed
};
#pragma pack(pop)

State sState = { MSG_STATE, CR_OFF, 0, 0, 0, {}, 0, 0, 0, 0, 0xFF, 0, {}, 0 };
double sBellReadyAt;   // host: the next time the bell can open the prison
void* sPoliceBell;     // this stage's police bell (TBellDolpic), while the plaza is loaded
unsigned sBellSerial;
bool sRingingForJailbreak; // our own ring() call below (not a robber's hit)
State sSeen;
double sDeadline;
double sLastSend;
bool sDirty;
double sModeSeenAt;
bool sHeld;
double sFreedAt[PORT_MP_MAX_PLAYERS]; // host: protection after a release
u8 sApplied[32];

bool running()
{
	PortMpSessionSettings s;
	port_mp_get_settings(&s);
	return port_mp_active() && s.gameMode == PORT_MP_MODE_CNR;
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

int me() { return port_mp_local_slot(); }
u8 roleOf(int s) { return s >= 0 && s < PORT_MP_MAX_PLAYERS ? sState.role[s] : R_NONE; }
u8 myRole() { return roleOf(me()); }
bool roundOn() { return sState.phase == CR_HEAD || sState.phase == CR_PLAY; }

void endRound(u8 winner, const char* why)
{
	sState.winner = winner;
	setPhase(CR_OVER, 8);
	std::fprintf(stderr, "[mp-cnr] round over: %s win (%s)\n", winner == 1 ? "robbers" : "cops", why);
}

// ---- host ------------------------------------------------------------------------------------
void hostStartRound()
{
	int cops, coinQ, shineQ, timeS, headS, pick;
	port_mp_cnr_settings(&cops, &coinQ, &shineQ, &timeS, &headS, &pick);
	int slots[PORT_MP_MAX_PLAYERS], n = 0;
	for (int i = 0; i < PORT_MP_MAX_PLAYERS; i++)
		if (used(i))
			slots[n++] = i;
	std::memset(sState.role, R_NONE, sizeof(sState.role));
	std::memset(sState.got, 0, sizeof(sState.got));
	if (n == 0)
		return;
	int want = cops > 0 ? cops : (n + 2) / 3; // about a third
	if (want > n - 1)
		want = n > 1 ? n - 1 : 1; // at least one robber
	for (int i = 0; i < n; i++)
		sState.role[slots[i]] = R_ROBBER;
	int chosen = 0;
	if (pick >= 0 && pick < PORT_MP_MAX_PLAYERS && used(pick)) {
		sState.role[pick] = R_COP;
		chosen            = 1;
	}
	for (int guard = 0; chosen < want && guard < 1000; guard++) {
		const int s = slots[std::rand() % n];
		if (sState.role[s] == R_ROBBER) {
			sState.role[s] = R_COP;
			chosen++;
		}
	}
	sState.coins      = 0;
	sState.shines     = 0;
	sState.coinQuota  = (u16)coinQ;
	sState.shineQuota = (u8)shineQ;
	sState.lastBy     = 0xFF;
	sState.jailbreaks = 0;
	sBellReadyAt      = 0.0;
	sState.round++;
	sState.winner = 0;
	for (double& t : sFreedAt)
		t = 0.0;
	setPhase(CR_HEAD, headS);
	std::fprintf(stderr, "[mp-cnr] round %d: %d cop(s), %d robber(s), quota %d coins + %d Shines\n", sState.round, chosen,
	             n - chosen, coinQ, shineQ);
}

void hostCheckQuota()
{
	if (sState.phase == CR_PLAY && sState.coins >= sState.coinQuota && sState.shines >= sState.shineQuota)
		endRound(1, "the quota is met");
}

void hostLoot(int slot, const LootMsg& m)
{
	if (!roundOn() || roleOf(slot) != R_ROBBER)
		return;
	if (m.shine) {
		if (sState.got[m.value >> 3] & (1 << (m.value & 7)))
			return;
		sState.got[m.value >> 3] |= (u8)(1 << (m.value & 7));
		sState.shines++;
	} else {
		const int c = sState.coins + m.value;
		sState.coins = (u16)(c > 60000 ? 60000 : c);
	}
	sDirty = true;
	hostCheckQuota();
}

void hostArrest(int robber, int cop)
{
	if (sState.phase != CR_PLAY || roleOf(robber) != R_ROBBER || roleOf(cop) != R_COP
	    || port_mp_time_ms() < sFreedAt[robber])
		return;
	sState.role[robber] = R_JAILED;
	sState.lastBy       = (u8)cop;
	sDirty              = true;
	char a[32], b[32];
	std::fprintf(stderr, "[mp-cnr] %s arrested %s\n", nameOf(cop, a, sizeof(a)), nameOf(robber, b, sizeof(b)));
}

void hostFree(int jailed, int robber)
{
	if (sState.phase != CR_PLAY || roleOf(jailed) != R_JAILED || roleOf(robber) != R_ROBBER)
		return;
	sState.role[jailed] = R_ROBBER;
	sState.lastBy       = (u8)robber;
	sFreedAt[jailed]    = port_mp_time_ms() + 3000.0;
	sDirty              = true;
	char a[32], b[32];
	std::fprintf(stderr, "[mp-cnr] %s freed %s\n", nameOf(robber, a, sizeof(a)), nameOf(jailed, b, sizeof(b)));
}

// A free robber rang the police bell: everyone in prison walks out.
void hostBell(int robber)
{
	if (sState.phase != CR_PLAY || roleOf(robber) != R_ROBBER || count(R_JAILED) == 0)
		return;
	const double now = port_mp_time_ms();
	if (now < sBellReadyAt)
		return;
	sBellReadyAt = now + kBellCooldownMs;
	for (int i = 0; i < PORT_MP_MAX_PLAYERS; i++)
		if (sState.role[i] == R_JAILED) {
			sState.role[i] = R_ROBBER;
			sFreedAt[i]    = now + 3000.0;
		}
	sState.lastBy = (u8)robber;
	sState.jailbreaks++;
	sDirty = true;
	char a[32];
	std::fprintf(stderr, "[mp-cnr] %s rang the police bell: jailbreak\n", nameOf(robber, a, sizeof(a)));
}

void hostFrame()
{
	if (!running()) {
		if (sState.phase != CR_OFF) {
			std::memset(sState.role, R_NONE, sizeof(sState.role));
			setPhase(CR_OFF, 0);
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
	int cops, coinQ, shineQ, timeS, headS, pick;
	port_mp_cnr_settings(&cops, &coinQ, &shineQ, &timeS, &headS, &pick);
	switch (sState.phase) {
	case CR_OFF: {
		TMarDirector* d = gpMarDirector;
		if (now - sModeSeenAt > 3000.0 && d != nullptr && d->mState == TMarDirector::STATE_UNK4
		    && SMSGetApplication()->mCurrArea.getStage() == port_mp_home_stage())
			hostStartRound();
		break;
	}
	case CR_HEAD:
	case CR_PLAY:
		if (count(R_COP) == 0)
			endRound(1, "no cops left");
		else if (count(R_ROBBER) + count(R_JAILED) == 0)
			endRound(2, "no robbers left");
		else if (count(R_ROBBER) == 0)
			endRound(2, "every robber is in prison");
		else if (sState.phase == CR_HEAD && now >= sDeadline)
			setPhase(CR_PLAY, timeS);
		else if (sState.phase == CR_PLAY && sDeadline > 0.0 && now >= sDeadline)
			endRound(2, "time is up");
		break;
	case CR_OVER:
		if (now >= sDeadline) {
			std::memset(sState.role, R_NONE, sizeof(sState.role));
			setPhase(CR_OFF, 0);
			sModeSeenAt = 0.0;
			port_mp_modes_round_over();
		}
		break;
	}
}

// ---- every client ----------------------------------------------------------------------------
// Swings this plaza's police bell (a jailbreak somebody else started).
void ringPoliceBell()
{
	if (sPoliceBell == nullptr || sBellSerial != gPortMpStageSerial || SMSGetApplication()->mCurrArea.getStage() != 1)
		return;
	TBellDolpic* bell = (TBellDolpic*)sPoliceBell;
	JGeometry::TVec3<f32> from = bell->mPosition;
	from.x -= 200.0f;
	sRingingForJailbreak = true;
	bell->ring(from);
	sRingingForJailbreak = false;
}

void toastChanges()
{
	const int m = me();
	char t[160], nm[32], nm2[32];
	if (sState.round != sSeen.round && sState.phase == CR_HEAD && m >= 0) {
		if (sState.role[m] == R_COP)
			std::snprintf(t, sizeof(t), "Cops and Robbers: you are a COP. Knock robbers over and grab them. (go in %d s)",
			              secondsLeft());
		else
			std::snprintf(t, sizeof(t), "Cops and Robbers: you are a ROBBER. Steal %d coins and %d Shine Sprites! (cops go in %d s)",
			              sState.coinQuota, sState.shineQuota, secondsLeft());
		port_mp_toast(t);
	}
	if (sState.phase == CR_PLAY && sSeen.phase != CR_PLAY)
		port_mp_toast(myRole() == R_COP ? "Go! Knock the robbers over, then touch them." : "The cops are coming!");
	const bool jailbreak = sState.round == sSeen.round && sState.jailbreaks != sSeen.jailbreaks;
	if (jailbreak) {
		std::snprintf(t, sizeof(t), sState.lastBy == m ? "You rang the police bell: the prison is open!"
		                                               : "%s rang the police bell: JAILBREAK!",
		              nameOf(sState.lastBy, nm, sizeof(nm)));
		port_mp_toast(t);
		ringPoliceBell(); // everyone hears it
	}
	if (sState.round == sSeen.round && sSeen.phase == sState.phase && roundOn() && !jailbreak)
		for (int i = 0; i < PORT_MP_MAX_PLAYERS; i++) {
			if (sSeen.role[i] == R_ROBBER && sState.role[i] == R_JAILED) {
				std::snprintf(t, sizeof(t), i == m ? "%s arrested you! Wait in the prison for a robber to free you."
				                                   : "%s arrested %s!",
				              nameOf(sState.lastBy, nm, sizeof(nm)), nameOf(i, nm2, sizeof(nm2)));
				port_mp_toast(t);
			} else if (sSeen.role[i] == R_JAILED && sState.role[i] == R_ROBBER) {
				std::snprintf(t, sizeof(t), i == m ? "%s freed you: run!" : "%s broke %s out of prison!",
				              nameOf(sState.lastBy, nm, sizeof(nm)), nameOf(i, nm2, sizeof(nm2)));
				port_mp_toast(t);
			}
		}
	// loot milestones (every 25 coins, every Shine)
	if (sState.round == sSeen.round && roundOn()
	    && (sState.shines != sSeen.shines || sState.coins / 25 != sSeen.coins / 25)) {
		std::snprintf(t, sizeof(t), "Robbers' loot: %d/%d coins, %d/%d Shines", sState.coins, sState.coinQuota,
		              sState.shines, sState.shineQuota);
		port_mp_toast(t);
	}
	if (sState.phase == CR_OVER && sSeen.phase != CR_OVER) {
		std::snprintf(t, sizeof(t), sState.winner == 1 ? "The robbers made their quota: the robbers win!"
		                                               : "The cops win! (loot %d/%d coins, %d/%d Shines)",
		              sState.coins, sState.coinQuota, sState.shines, sState.shineQuota);
		port_mp_toast(t);
		std::fprintf(stderr, "[mp-cnr] %s\n", t);
	}
	sSeen = sState;
}

bool playable(TMario* m)
{
	return m != nullptr && gpMarDirector != nullptr && gpMarDirector->mState == TMarDirector::STATE_UNK4
	       && !m->checkFlag(MARIO_FLAG_GAME_OVER);
}

bool knockedDown(const TMario* m)
{
	switch (m->mStatus) {
	case MARIO_STATUS_SAFE_BACK_DOWN:
	case MARIO_STATUS_SHORT_BACK_DOWN:
	case MARIO_STATUS_BACK_DOWN:
	case MARIO_STATUS_JUMP_SHORT_BACK_DOWN:
	case MARIO_STATUS_JUMP_BACK_DOWN:
	case MARIO_STATUS_SAFE_FORE_DOWN:
	case MARIO_STATUS_SHORT_FORE_DOWN:
	case MARIO_STATUS_FORE_DOWN:
	case MARIO_STATUS_JUMP_SHORT_FORE_DOWN:
	case MARIO_STATUS_JUMP_FORE_DOWN:
	case MARIO_STATUS_LAND_SAFE_DOWN: return true;
	default: return false;
	}
}

// Another player (of that role) touching our Mario, as drawn here: the slot, or -1.
int touchedBy(const TMario* m, u8 role)
{
	for (int s = 0; s < PORT_MP_MAX_PLAYERS; s++) {
		if (s == me() || roleOf(s) != role || !port_mp_same_stage(s))
			continue;
		f32 p[3];
		if (!port_mp_puppet_pos(s, p))
			continue;
		const f32 dx = p[0] - m->mPosition.x, dz = p[2] - m->mPosition.z, dy = p[1] - m->mPosition.y;
		if (dx * dx + dz * dz < kTouch * kTouch && dy > -170.0f && dy < 170.0f)
			return s;
	}
	return -1;
}

void sendWho(u8 type, int by)
{
	WhoMsg w = { type, (u8)by };
	if (port_mp_is_host()) {
		if (type == MSG_ARRESTED)
			hostArrest(me(), by);
		else
			hostFree(me(), by);
	} else {
		port_mp_send(kChannel, &w, (int)sizeof(w), 1, -1);
	}
}

// A robber: knocked over and touched by a cop = arrested.
void robberFrame(TMario* m)
{
	static double sDownUntil, sAskedAt;
	if (myRole() != R_ROBBER || sState.phase != CR_PLAY)
		return;
	const double now = port_mp_time_ms();
	if (knockedDown(m))
		sDownUntil = now + 1500.0;
	if (now >= sDownUntil || now - sAskedAt < 1000.0)
		return;
	const int cop = touchedBy(m, R_COP);
	if (cop >= 0) {
		sAskedAt = now;
		std::fprintf(stderr, "[mp-cnr] knocked over and touched by a cop: arrested\n");
		sendWho(MSG_ARRESTED, cop);
	}
}

// A jailed robber: to the plaza, into the prison, held there; a free robber's touch releases him.
double sShotAt; // tests: when to take a picture of the prison

void jailedFrame(TMario* m)
{
	static double sAskedAt, sReturnAt;
	if (sShotAt > 0.0 && port_mp_time_ms() >= sShotAt) {
		sShotAt = 0.0;
		port_shot_request(810000);
	}
	if (myRole() != R_JAILED || !roundOn())
		return;
	const double now = port_mp_time_ms();
	const int stage  = SMSGetApplication()->mCurrArea.getStage();
	if (stage != 1) { // to the plaza (once per few seconds while the change is under way)
		if (now - sReturnAt > 4000.0 && gpMarDirector != nullptr
		    && !(gpMarDirector->mFlags & TMarDirector::DIRECTOR_FLAG_STAGE_TRANSITION_PENDING)) {
			sReturnAt = now;
			PortMpSessionSettings s;
			port_mp_get_settings(&s);
			int home, scen;
			port_mp_home_area(&s, &home, &scen);
			SMSGetApplication()->setNextArea(TGameSequence((u8)home, (u8)scen));
			gpMarDirector->onFlag(TMarDirector::DIRECTOR_FLAG_STAGE_TRANSITION_PENDING);
			std::fprintf(stderr, "[mp-cnr] arrested: off to the prison in the plaza\n");
		}
		return;
	}
	// held inside the prison room (elsewhere: put in it; at a wall: kept in)
	const f32 x = m->mPosition.x, z = m->mPosition.z;
	const bool near = x > kRoomX0 - 400.0f && x < kRoomX1 + 400.0f && z > kRoomZ0 - 400.0f && z < kRoomZ1 + 400.0f;
	if (!near || m->mPosition.y < 0.0f || m->mPosition.y > 900.0f) {
		f32 y = 305.0f;
		if (gpMap != nullptr) {
			const TBGCheckData* plane = nullptr;
			const f32 g = gpMap->checkGround(kPrisonX, 500.0f, kPrisonZ, &plane); // under the room's ceiling
			if (plane != nullptr && g > -100000.0f)
				y = g;
		}
		m->mPosition.set(kPrisonX, y + 5.0f, kPrisonZ);
		m->mVel.x = m->mVel.y = m->mVel.z = 0.0f;
		m->mForwardVel = 0.0f;
		std::fprintf(stderr, "[mp-cnr] put in the prison at %.0f %.0f %.0f\n", kPrisonX, y, kPrisonZ);
		static const bool sShot = std::getenv("SMS_PORT_MP_CNR_TEST") != nullptr;
		static bool sShotDone;
		if (sShot && !sShotDone) {
			sShotDone = true;
			sShotAt   = now + 1200.0; // (tests: a picture of the prison a moment later)
		}
	} else if (!inRoom(x, z)) {
		m->mPosition.x = x < kRoomX0 ? kRoomX0 : (x > kRoomX1 ? kRoomX1 : x);
		m->mPosition.z = z < kRoomZ0 ? kRoomZ0 : (z > kRoomZ1 ? kRoomZ1 : z);
		m->mForwardVel *= 0.3f;
	}
	if (now - sAskedAt < 1000.0)
		return;
	const int rescuer = touchedBy(m, R_ROBBER);
	if (rescuer >= 0) {
		sAskedAt = now;
		sendWho(MSG_FREED, rescuer);
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
		} else if (!host) {
			continue;
		} else if (buf[0] == MSG_LOOT && n == (int)sizeof(LootMsg)) {
			hostLoot(from, *(const LootMsg*)buf);
		} else if (buf[0] == MSG_ARRESTED && n == (int)sizeof(WhoMsg)) {
			hostArrest(from, ((const WhoMsg*)buf)->by);
		} else if (buf[0] == MSG_FREED && n == (int)sizeof(WhoMsg)) {
			hostFree(from, ((const WhoMsg*)buf)->by);
		} else if (buf[0] == MSG_BELL && n == 1) {
			hostBell(from);
		}
	}
	if (host) {
		hostFrame();
		if (sDirty || port_mp_time_ms() - sLastSend > 1000.0) {
			const double left = sDeadline - port_mp_time_ms();
			sState.leftMs     = sDeadline > 0.0 ? (u32)(left > 1.0 ? left : 1.0) : 0;
			const double bell = sBellReadyAt - port_mp_time_ms();
			sState.bellWait   = bell > 0.0 ? (u8)(bell / 1000.0 + 1.0) : 0;
			port_mp_send(kChannel, &sState, (int)sizeof(sState), 1, -1);
			sLastSend = port_mp_time_ms();
			sDirty    = false;
		}
	}
	// every Shine stolen this round is marked taken in our save (the HUD counter stays ours)
	if (running() && roundOn())
		for (int b = 0; b < 32; b++)
			if (u8 fresh = (u8)(sState.got[b] & ~sApplied[b])) {
				for (int k = 0; k < 8; k++)
					if (fresh & (1 << k)) {
						TFlagManager* f = TFlagManager::getInstance();
						const s32 c     = f->getFlag(MSF_SHINE_COUNT);
						if (!f->getShineFlag((u8)(b * 8 + k))) {
							f->setShineFlag((u8)(b * 8 + k));
							f->setFlag(MSF_SHINE_COUNT, c);
						}
					}
				sApplied[b] |= fresh;
			}
	if (sState.phase == CR_OFF)
		std::memset(sApplied, 0, sizeof(sApplied));
	toastChanges();
	TMario* m = gpMarioOriginal;
	if (playable(m)) {
		robberFrame(m);
		jailedFrame(m);
	}
	// SMS_PORT_MP_CNR_TEST=1 (tests): a cop jumps onto a robber every 3 s; a robber reports 10 coins
	// and a Shine (id 5 + slot) once play starts
	static const bool sTest = std::getenv("SMS_PORT_MP_CNR_TEST") != nullptr;
	if (sTest && sState.phase == CR_PLAY && playable(m)) {
		static double sAt;
		static u8 sLootRound = 0xFF;
		if (myRole() == R_COP && port_mp_time_ms() - sAt > 3000.0 && count(R_JAILED) == 0) // (one arrest, then a rescue)
			for (int s = 0; s < PORT_MP_MAX_PLAYERS; s++) {
				f32 p[3];
				if (roleOf(s) == R_ROBBER && port_mp_same_stage(s) && port_mp_puppet_pos(s, p)) {
					sAt = port_mp_time_ms();
					m->mPosition.set(p[0], p[1] + 300.0f, p[2]); // falls on his head: a stomp
					std::fprintf(stderr, "[mp-cnr] test: cop drops onto slot %d\n", s);
					break;
				}
			}
		static double sRescueAt;
		static const bool sBellTest = std::getenv("SMS_PORT_MP_CNR_BELL") != nullptr; // ring instead of walking in
		if (sBellTest && myRole() == R_ROBBER && count(R_JAILED) > 0 && port_mp_time_ms() - sRescueAt > 6000.0
		    && sPoliceBell != nullptr && sBellSerial == gPortMpStageSerial) {
			sRescueAt = port_mp_time_ms();
			TBellDolpic* bell = (TBellDolpic*)sPoliceBell;
			std::fprintf(stderr, "[mp-cnr] test: robber jumps into the police bell\n");
			bell->touchPlayer(m); // as when Mario hits it
		}
		if (!sBellTest && myRole() == R_ROBBER && port_mp_time_ms() - sRescueAt > 6000.0)
			for (int s = 0; s < PORT_MP_MAX_PLAYERS; s++) {
				f32 p[3];
				if (s != me() && roleOf(s) == R_JAILED && port_mp_same_stage(s) && port_mp_puppet_pos(s, p)) {
					sRescueAt = port_mp_time_ms();
					m->mPosition.set(p[0] + 40.0f, p[1] + 10.0f, p[2]);
					std::fprintf(stderr, "[mp-cnr] test: robber runs to the prisoner in slot %d\n", s);
					break;
				}
			}
		if (myRole() == R_ROBBER && sLootRound != sState.round) {
			sLootRound = sState.round;
			std::fprintf(stderr, "[mp-cnr] test: robber reports 10 coins and Shine %d\n", 5 + me());
			LootMsg a = { MSG_LOOT, 0, 10, 0 }, b = { MSG_LOOT, 1, (u8)(5 + me()), 0 };
			if (port_mp_is_host()) {
				hostLoot(me(), a);
				hostLoot(me(), b);
			} else {
				port_mp_send(kChannel, &a, (int)sizeof(a), 1, -1);
				port_mp_send(kChannel, &b, (int)sizeof(b), 1, -1);
			}
		}
	}
	// cops' pads stay idle during the robbers' head start
	const bool held = sState.phase == CR_HEAD && myRole() == R_COP;
	if (held != sHeld) {
		sHeld = held;
		port_mp_request_input(held ? 1 : 0);
	}
}

struct Register {
	Register() { port_mp_register_game_frame(gameFrame); }
} sRegister;

void sendLoot(u8 shine, u8 value)
{
	LootMsg l = { MSG_LOOT, shine, value, 0 };
	if (port_mp_is_host())
		hostLoot(me(), l);
	else
		port_mp_send(kChannel, &l, (int)sizeof(l), 1, -1);
}

} // namespace

extern "C" {

int port_mp_cnr_running(void) { return running() ? 1 : 0; }

// MapObjDolpic.cpp: a plaza bell was set up (police = the police station's).
void port_mp_cnr_bell_seen(void* bell, int police)
{
	std::fprintf(stderr, "[mp-cnr] plaza bell set up (%s, stage serial %u, at %.0f %.0f %.0f)\n", police ? "police" : "TV",
	             gPortMpStageSerial, ((TBellDolpic*)bell)->mPosition.x, ((TBellDolpic*)bell)->mPosition.y,
	             ((TBellDolpic*)bell)->mPosition.z);
	if (police) {
		sPoliceBell = bell;
		sBellSerial = gPortMpStageSerial;
	}
}

// MapObjDolpic.cpp (TBellDolpic::ring): a bell starts swinging. Our Mario rang the police bell as a
// free robber: the host opens the prison (if anyone is in it and the bell is ready).
void port_mp_cnr_bell_rung(int police)
{
	if (!police || sRingingForJailbreak || !running() || sState.phase != CR_PLAY || myRole() != R_ROBBER)
		return;
	if (count(R_JAILED) == 0) {
		port_mp_toast("The police bell rings... but nobody is in prison.");
		return;
	}
	std::fprintf(stderr, "[mp-cnr] we rang the police bell\n");
	const int wait = port_mp_is_host() ? (port_mp_time_ms() < sBellReadyAt ? (int)((sBellReadyAt - port_mp_time_ms()) / 1000.0) + 1 : 0)
	                                   : sState.bellWait;
	if (wait > 0) {
		char t[80];
		std::snprintf(t, sizeof(t), "The bell needs %d s before it opens the prison again.", wait);
		port_mp_toast(t);
		return;
	}
	if (port_mp_is_host()) {
		hostBell(me());
	} else {
		const u8 msg = MSG_BELL;
		port_mp_send(kChannel, &msg, 1, 1, -1);
	}
}

// port_mp_game.cpp: our Mario took a coin (TCoin / TCoinRed / TCoinBlue::taken).
void port_mp_cnr_coin_taken(void* item)
{
	if (!running() || !roundOn() || myRole() != R_ROBBER || item == nullptr)
		return;
	const TItem* it = (const TItem*)item;
	const u8 value  = it->isActorType(ACTOR_TYPE_COIN_BLUE) ? 5 : it->isActorType(ACTOR_TYPE_COIN_RED) ? 2 : 1;
	sendLoot(0, value);
}

// port_mp_game.cpp (the shine-get demo).
void port_mp_cnr_shine_got(int shineId)
{
	if (running() && roundOn() && myRole() == R_ROBBER && shineId >= 0 && shineId < 256)
		sendLoot(1, (u8)shineId);
}

// port_mp_arena.cpp: an arrested robber cannot leave the plaza (1 = sent back there).
int port_mp_cnr_route(int* nextState, unsigned char* stage, unsigned char* scenario)
{
	if (!running() || !roundOn() || myRole() != R_JAILED)
		return 0;
	if (*nextState == TApplication::APP_STATE_GAMEPLAY && *stage == 1)
		return 0;
	PortMpSessionSettings s;
	port_mp_get_settings(&s);
	int home, scen;
	port_mp_home_area(&s, &home, &scen);
	*nextState = TApplication::APP_STATE_GAMEPLAY;
	*stage     = (u8)home;
	*scenario  = (u8)scen;
	port_mp_toast("You are in prison: wait for a robber to free you.");
	return 1;
}

// port_mp_puppet.cpp, with the frame's view matrix and the game's projection set: a "PRISON" label over
// the police station's door for the overlay, while a round runs, in the plaza.
void port_mp_cnr_project(void* viewPtr)
{
	if (!running() || !roundOn() || SMSGetApplication()->mCurrArea.getStage() != 1)
		return;
	MtxPtr view = (MtxPtr)viewPtr;
	f32 proj[7], vp[6];
	GXGetProjectionv(proj);
	GXGetViewportv(vp);
	if (vp[2] <= 0.0f || vp[3] <= 0.0f)
		return;
	const f32 x = kDoorX, y = 305.0f + 380.0f, z = kDoorZ;
	const f32 vz = view[2][0] * x + view[2][1] * y + view[2][2] * z + view[2][3];
	if (vz > -10.0f || -vz > 7000.0f)
		return; // behind the camera, or far away
	f32 sx, sy, sz;
	GXProject(x, y, z, view, proj, vp, &sx, &sy, &sz);
	const float label[2] = { (sx - vp[0]) / vp[2], (sy - vp[1]) / vp[3] };
	port_mp_overlay_set_cage(label, label, nullptr, 0, label, 1, 0xFFB040, "PRISON");
}

// port_mp_puppet.cpp: name-tag colour (cops blue, prisoners grey). 1 = recoloured; *jailed = 1 for "(jailed)".
int port_mp_cnr_tag_rgb(int slot, u32* rgb, int* jailed)
{
	*jailed = 0;
	if (!running() || !roundOn())
		return 0;
	switch (roleOf(slot)) {
	case R_COP: *rgb = 0x5AA0FF; return 1;
	case R_JAILED: *rgb = 0x9A9A9A; *jailed = 1; return 1;
	default: return 0;
	}
}

int port_mp_cnr_headline(char* out, int cap)
{
	const int left = secondsLeft();
	switch (sState.phase) {
	case CR_HEAD: std::snprintf(out, cap, "Cops and Robbers: head start %d", left); return 1;
	case CR_PLAY:
		std::snprintf(out, cap, "Cops & Robbers %d:%02d  loot %d/%d coins, %d/%d Shines  (%d in prison)", left / 60,
		              left % 60, sState.coins, sState.coinQuota, sState.shines, sState.shineQuota, count(R_JAILED));
		return 1;
	case CR_OVER: std::snprintf(out, cap, "Cops and Robbers: the %s win", sState.winner == 1 ? "robbers" : "cops"); return 1;
	default: return 0;
	}
}

const char* port_mp_cnr_role(int slot)
{
	if (sState.phase == CR_OFF)
		return nullptr;
	switch (roleOf(slot)) {
	case R_COP: return "Cop";
	case R_ROBBER: return "Robber";
	case R_JAILED: return "In prison";
	default: return nullptr;
	}
}

} // extern "C"

#endif // SMS_PORT_AURORA
