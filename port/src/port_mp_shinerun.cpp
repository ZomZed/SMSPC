// Multiplayer game mode "Shine Runners" (mode PORT_MP_MODE_SHINERUN, run by port_mp_modes.cpp like the
// other round modes). Everyone races for Shine Sprites:
//  - Every level is open on a copy of the save without its Shine flags (port_mp_modes.cpp, as
//    Manhunt), so every Shine is collectible again. A Shine from a level (the shine-get demo,
//    port_mp_shine_got) is +1, each one once per round (then marked taken in everyone's save).
//  - A player hit by another (a stomp, a ground pound or FLUDD's push: port_mp_interact.cpp's hits)
//    while holding a Shine loses one: it jumps out of him and bounces around (a copy of the stage's
//    shine.bmd, simulated on every client against the map's floors) until someone touches it (+1;
//    the host decides who was first) or 30 s pass. At most one knocked out per 2.5 s.
//  - Dying (a lost life, a game over) loses some (host setting, default 3).
//  - The first to the goal (default 5) wins; with a time limit (default 15 min, 0 = none) the most
//    Shines wins when it runs out (a tie is a draw).
// State on PORT_MP_CH_SHINERUN from the host (reliable on every change, once a second).
#ifdef SMS_PORT_AURORA

#include "port_mp.h"

#include <JSystem/J3D/J3DGraphAnimator/J3DModel.hpp>
#include <JSystem/J3D/J3DGraphLoader/J3DModelLoader.hpp>
#include <JSystem/JKernel/JKRFileLoader.hpp>
#include <JSystem/JKernel/JKRHeap.hpp>
#include <M3DUtil/MActor.hpp>
#include <MSound/MSound.hpp>
#include <MSound/SoundEffects.hpp>
#include <Map/Map.hpp>
#include <Map/MapData.hpp>
#include <MarioUtil/LightUtil.hpp>
#include <MarioUtil/MathUtil.hpp>
#include <Enemy/Conductor.hpp>
#include <Player/Mario.hpp>
#include <Player/MarioAccess.hpp>
#include <Strategic/ActorTypes.hpp>
#include <Strategic/LiveActor.hpp>
#include <Strategic/LiveManager.hpp>
#include <System/Application.hpp>
#include <System/FlagManager.hpp>
#include <System/MarDirector.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

extern "C" void port_mp_toast(const char* text);                                            // port_mp_menu.cpp
extern "C" void port_mp_shinerun_settings(int* goal, int* timeS, int* deathLoss);           // port_mp_modes.cpp
extern "C" void port_mp_modes_round_over(void);                                             // port_mp_modes.cpp
extern "C" int port_mp_home_stage(void);                                                    // port_mp_arena.cpp
extern "C" void port_mp_shinerun_shine_got(int shineId);                                    // below
extern "C" void port_mp_shinerun_hit(int fromSlot);                                         // below
extern "C" void port_shot_request(unsigned tag);                                            // port_movetest.cpp (tests)

namespace {

const uint8_t kChannel = PORT_MP_CH_SHINERUN;
const int kMaxDrops    = 12;
const double kDropLifeMs = 30000.0;
enum Phase : u8 { SR_OFF = 0, SR_READY = 1, SR_PLAY = 2, SR_OVER = 3 };
enum : u8 { MSG_STATE = 1, MSG_SHINE = 2, MSG_DIED = 3, MSG_DROP = 4, MSG_GRAB = 5 };

#pragma pack(push, 1)
struct Drop {
	u16 id; // 0 = free
	u8 stage, scenario;
	u8 dropper; // slot (cannot grab it back for 2 s)
	u8 pad;
	float pos[3];
	float vel[3];
};
struct State {
	u8 type; // MSG_STATE
	u8 phase;
	u8 round;
	u8 winner; // SR_OVER: slot, 0xFF = a draw
	u32 leftMs;
	u8 goal;
	u8 inRound[PORT_MP_MAX_PLAYERS]; // 1 = playing this round
	u8 scores[PORT_MP_MAX_PLAYERS];
	u8 got[32]; // level Shines taken this round (bits)
	u16 lastGrab;  // the drop picked up last (toasts)
	u8 lastGrabber;
	u8 pad;
	Drop drops[kMaxDrops];
};
struct ShineMsg {
	u8 type; // MSG_SHINE
	u8 id;
};
struct DropMsg {
	u8 type; // MSG_DROP (the hit player -> host)
	u8 pad[3];
	float pos[3];
};
struct GrabMsg {
	u8 type; // MSG_GRAB
	u8 pad;
	u16 id;
};
#pragma pack(pop)
static_assert(sizeof(State) < PORT_MP_APP_MAX, "Shine Runners state fits a message");

State sState;
State sSeen;
double sDeadline;
double sLastSend;
bool sDirty;
double sModeSeenAt;
u16 sNextDropId = 1;
double sDropBorn[kMaxDrops]; // host: when each drop appeared
u8 sApplied[32];             // level Shines already marked taken in our save
double sDropCooldownUntil;   // hit player: no second Shine knocked out before this

bool running()
{
	PortMpSessionSettings s;
	port_mp_get_settings(&s);
	return port_mp_active() && s.gameMode == PORT_MP_MODE_SHINERUN;
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
bool roundOn() { return sState.phase == SR_READY || sState.phase == SR_PLAY; }
bool inRound(int slot) { return slot >= 0 && slot < PORT_MP_MAX_PLAYERS && sState.inRound[slot]; }

// The leader (0xFF: nobody, or a tie for the most).
int leader(bool* tie)
{
	int best = -1, bestN = -1;
	*tie = false;
	for (int i = 0; i < PORT_MP_MAX_PLAYERS; i++) {
		if (!inRound(i))
			continue;
		if (sState.scores[i] > bestN) {
			best  = i;
			bestN = sState.scores[i];
			*tie  = false;
		} else if (sState.scores[i] == bestN) {
			*tie = true;
		}
	}
	return best;
}

// ---- host ------------------------------------------------------------------------------------
void endRound(int winner, const char* why)
{
	sState.winner = (u8)winner;
	std::memset(sState.drops, 0, sizeof(sState.drops));
	setPhase(SR_OVER, 8);
	std::fprintf(stderr, "[mp-shinerun] round over (%s): winner slot %d\n", why, winner == 0xFF ? -1 : winner);
}

void hostStartRound()
{
	int goal, timeS, loss;
	port_mp_shinerun_settings(&goal, &timeS, &loss);
	const u8 round = (u8)(sState.round + 1);
	std::memset(&sState, 0, sizeof(sState));
	sState.type  = MSG_STATE;
	sState.round = round;
	sState.goal  = (u8)goal;
	sState.winner = 0xFF;
	int n = 0;
	for (int i = 0; i < PORT_MP_MAX_PLAYERS; i++)
		if (used(i)) {
			sState.inRound[i] = 1;
			n++;
		}
	setPhase(SR_READY, 5);
	std::fprintf(stderr, "[mp-shinerun] round %d: %d players, %d Shines to win\n", sState.round, n, goal);
}

void hostScore(int slot, int delta, const char* why)
{
	if (!inRound(slot))
		return;
	int v = sState.scores[slot] + delta;
	sState.scores[slot] = (u8)(v < 0 ? 0 : (v > 255 ? 255 : v));
	sDirty              = true;
	char nm[32];
	std::fprintf(stderr, "[mp-shinerun] %s %s: %d\n", nameOf(slot, nm, sizeof(nm)), why, sState.scores[slot]);
	if (sState.phase == SR_PLAY && sState.scores[slot] >= sState.goal)
		endRound(slot, "goal reached");
}

void hostShine(int slot, int id)
{
	if (sState.phase != SR_PLAY || id < 0 || id >= 256 || (sState.got[id >> 3] & (1 << (id & 7))))
		return;
	sState.got[id >> 3] |= (u8)(1 << (id & 7));
	hostScore(slot, +1, "got a Shine from the level");
}

void hostDrop(int slot, const float pos[3])
{
	if (sState.phase != SR_PLAY || !inRound(slot) || sState.scores[slot] == 0)
		return;
	PortMpPlayerInfo p;
	if (!port_mp_get_player(slot, &p))
		return;
	int k = 0;
	while (k < kMaxDrops && sState.drops[k].id != 0)
		k++;
	if (k == kMaxDrops) { // full: the oldest goes
		k = 0;
		for (int i = 1; i < kMaxDrops; i++)
			if (sDropBorn[i] < sDropBorn[k])
				k = i;
	}
	hostScore(slot, -1, "was hit and lost a Shine");
	Drop& d    = sState.drops[k];
	if (sNextDropId == 0)
		sNextDropId = 1;
	d.id       = sNextDropId++;
	d.stage    = p.stage;
	d.scenario = p.scenario;
	d.dropper  = (u8)slot;
	const f32 a = (f32)(d.id * 137 % 360) * 3.14159265f / 180.0f;
	d.pos[0] = pos[0];
	d.pos[1] = pos[1] + 120.0f;
	d.pos[2] = pos[2];
	d.vel[0] = std::sin(a) * 9.0f;
	d.vel[1] = 32.0f;
	d.vel[2] = std::cos(a) * 9.0f;
	if (std::getenv("SMS_PORT_MP_SHINERUN_TEST") != nullptr)
		d.vel[0] = d.vel[2] = 0.0f; // tests: straight up, back onto the dropper (in view, picked up again)
	sDropBorn[k] = port_mp_time_ms();
	sDirty       = true;
	std::fprintf(stderr, "[mp-shinerun] drop %u in stage %d/%d at %.0f %.0f %.0f\n", d.id, d.stage, d.scenario, d.pos[0],
	             d.pos[1], d.pos[2]);
}

void hostGrab(int slot, u16 id)
{
	if (sState.phase != SR_PLAY || !inRound(slot) || id == 0)
		return;
	for (int k = 0; k < kMaxDrops; k++)
		if (sState.drops[k].id == id) {
			sState.drops[k].id = 0;
			sState.lastGrab    = id;
			sState.lastGrabber = (u8)slot;
			hostScore(slot, +1, "picked up a Shine");
			return;
		}
}

void hostFrame()
{
	if (!running()) {
		if (sState.phase != SR_OFF) {
			std::memset(sState.inRound, 0, sizeof(sState.inRound));
			std::memset(sState.drops, 0, sizeof(sState.drops));
			setPhase(SR_OFF, 0);
		}
		sModeSeenAt = 0.0;
		return;
	}
	const double now = port_mp_time_ms();
	if (sModeSeenAt == 0.0)
		sModeSeenAt = now;
	for (int i = 0; i < PORT_MP_MAX_PLAYERS; i++)
		if (sState.inRound[i] && !used(i)) {
			sState.inRound[i] = 0;
			sDirty            = true;
		}
	for (int k = 0; k < kMaxDrops; k++)
		if (sState.drops[k].id != 0 && now - sDropBorn[k] > kDropLifeMs) {
			sState.drops[k].id = 0; // nobody took it
			sDirty             = true;
		}
	int goal, timeS, loss;
	port_mp_shinerun_settings(&goal, &timeS, &loss);
	switch (sState.phase) {
	case SR_OFF: {
		TMarDirector* d = gpMarDirector;
		if (now - sModeSeenAt > 3000.0 && d != nullptr && d->mState == TMarDirector::STATE_UNK4
		    && SMSGetApplication()->mCurrArea.getStage() == port_mp_home_stage())
			hostStartRound();
		break;
	}
	case SR_READY:
		if (now >= sDeadline)
			setPhase(SR_PLAY, timeS);
		break;
	case SR_PLAY: {
		int players = 0;
		for (int i = 0; i < PORT_MP_MAX_PLAYERS; i++)
			players += sState.inRound[i];
		if (players == 0) {
			endRound(0xFF, "everyone left");
		} else if (sDeadline > 0.0 && now >= sDeadline) {
			bool tie;
			const int best = leader(&tie);
			endRound(tie || best < 0 ? 0xFF : best, "time is up");
		}
		break;
	}
	case SR_OVER:
		if (now >= sDeadline) {
			std::memset(sState.inRound, 0, sizeof(sState.inRound));
			setPhase(SR_OFF, 0);
			sModeSeenAt = 0.0;
			port_mp_modes_round_over();
		}
		break;
	}
}

// ---- drops on every client: a bouncing copy of shine.bmd ----------------------------------------
struct StageHeapScope {
	JKRHeap* prev;
	StageHeapScope()
	    : prev(nullptr)
	{
		JKRHeap* stage = SMSGetApplication()->mHeap;
		if (stage != nullptr && JKRGetCurrentHeap() != stage)
			prev = stage->becomeCurrentHeap();
	}
	~StageHeapScope()
	{
		if (prev != nullptr)
			prev->becomeCurrentHeap();
	}
};

struct LocalDrop {
	u16 id;
	f32 pos[3], vel[3];
	f32 spin;
	int age;      // logic frames since it appeared here
	bool resting;
	bool asked;   // we asked the host for it
	bool drawn;
};
struct Looks {
	unsigned serial;
	bool valid;
	J3DModelData* data;
	bool dataTried;
	J3DModel* model[kMaxDrops];
	LocalDrop drop[kMaxDrops];
} sLooks;

void resetIfNewStage()
{
	if (sLooks.valid && sLooks.serial == gPortMpStageSerial)
		return;
	std::memset((void*)&sLooks, 0, sizeof(sLooks));
	sLooks.serial = gPortMpStageSerial;
	sLooks.valid  = true;
}

J3DModelData* shineData()
{
	if (sLooks.dataTried)
		return sLooks.data;
	sLooks.dataTried = true;
	StageHeapScope scope;
	// a Shine actor's model (set up as the game draws it: TShine::makeMActors) ...
	if (gpConductor != nullptr)
		for (JGadget::TList<TLiveManager*>::iterator it = gpConductor->unk10.begin();
		     it != gpConductor->unk10.end() && sLooks.data == nullptr; ++it)
			if (TLiveManager* mgr = *it)
				for (int i = 0; i < mgr->getObjNum() && sLooks.data == nullptr; ++i)
					if (TLiveActor* a = mgr->getObj(i))
						if (a->isActorType(ACTOR_TYPE_SHINE) && a->getMActor() != nullptr && a->getMActor()->getModel() != nullptr)
							sLooks.data = a->getMActor()->getModel()->getModelData();
	// ... else the stage's mapobj/shine.bmd with TShine's flags
	const u32 flags = J3DMLF_MaterialPEFull | J3DMLF_UseUniqueMaterials | (2 << J3DMLF_TevStageNumShift);
	if (sLooks.data == nullptr)
		if (void* res = JKRFileLoader::getGlbResource("/scene/mapobj/shine.bmd"))
			sLooks.data = J3DModelLoaderDataBase::load(res, flags);
	std::fprintf(stderr, sLooks.data != nullptr ? "[mp-shinerun] Shine model ready\n"
	                                            : "[mp-shinerun] no Shine model in this stage: drops not drawn\n");
	return sLooks.data;
}

J3DModel* dropModel(int k)
{
	if (sLooks.model[k] != nullptr)
		return sLooks.model[k];
	J3DModelData* d = shineData();
	if (d == nullptr)
		return nullptr;
	StageHeapScope scope;
	JKRHeap* heap = JKRGetCurrentHeap();
	void* mem     = heap != nullptr && heap->getFreeSize() > 0x20000 ? JKRHeap::alloc(sizeof(J3DModel), 0x10, heap) : nullptr;
	if (mem != nullptr)
		sLooks.model[k] = new (mem) J3DModel(d, 0, 1);
	return sLooks.model[k];
}

bool inThisStage(const Drop& d)
{
	const TGameSequence& a = SMSGetApplication()->mCurrArea;
	return d.id != 0 && d.stage == a.getStage() && d.scenario == a.getScenario();
}

// Follow the host's list: new drops start where the host put them; gone ones disappear.
void syncDrops()
{
	for (int k = 0; k < kMaxDrops; k++) {
		const Drop& d  = sState.drops[k];
		LocalDrop& l   = sLooks.drop[k];
		const bool here = running() && sState.phase == SR_PLAY && inThisStage(d);
		if (!here) {
			l.id = 0;
			continue;
		}
		if (l.id != d.id) {
			std::memset(&l, 0, sizeof(l));
			l.id = d.id;
			std::memcpy(l.pos, d.pos, sizeof(l.pos));
			std::memcpy(l.vel, d.vel, sizeof(l.vel));
			std::fprintf(stderr, "[mp-shinerun] drop %u bounces here\n", l.id);
		}
	}
}

// One logic frame of bouncing (gravity, the map's floor under it).
void stepDrop(LocalDrop& l)
{
	l.age++;
	l.spin += l.resting ? 3.0f : 9.0f;
	if (l.resting || gpMap == nullptr)
		return;
	l.vel[1] -= 2.0f;
	if (l.vel[1] < -60.0f)
		l.vel[1] = -60.0f;
	l.pos[0] += l.vel[0];
	l.pos[1] += l.vel[1];
	l.pos[2] += l.vel[2];
	const TBGCheckData* plane = nullptr;
	const f32 ground = gpMap->checkGround(l.pos[0], l.pos[1] + 80.0f, l.pos[2], &plane);
	const f32 floorY = ground + 60.0f; // the Shine floats a little above the floor
	if (plane != nullptr && ground > -100000.0f && l.pos[1] < floorY && l.vel[1] <= 0.0f) {
		l.pos[1] = floorY;
		const TMario* m = gpMarioOriginal;
		const f32 mx = m != nullptr ? l.pos[0] - m->mPosition.x : 1e9f, mz = m != nullptr ? l.pos[2] - m->mPosition.z : 0.0f;
		if (l.vel[1] < -6.0f && SMSGetMSound() != nullptr && l.age > 2 && mx * mx + mz * mz < 2000.0f * 2000.0f)
			SMSGetMSound()->startSoundSystemSE(MSD_SE_SY_COIN_BOUND, 0, nullptr, 0); // (near us only)
		l.vel[1] = -l.vel[1] * 0.55f;
		l.vel[0] *= 0.75f;
		l.vel[2] *= 0.75f;
		if (l.vel[1] < 5.0f) {
			l.vel[0] = l.vel[1] = l.vel[2] = 0.0f;
			l.resting = true;
			std::fprintf(stderr, "[mp-shinerun] drop %u rests at %.0f %.0f %.0f\n", l.id, l.pos[0], l.pos[1], l.pos[2]);
		}
	}
}

void dropsFrame(TMario* m)
{
	resetIfNewStage();
	syncDrops();
	const int self = me();
	for (int k = 0; k < kMaxDrops; k++) {
		LocalDrop& l = sLooks.drop[k];
		if (l.id == 0)
			continue;
		stepDrop(l);
		// touching it: ask the host (the dropper only after 2 s)
		if (m == nullptr || l.asked || !inRound(self))
			continue;
		if (sState.drops[k].dropper == self && l.age < 60)
			continue;
		if (l.age < 10)
			continue;
		const f32 dx = l.pos[0] - m->mPosition.x, dz = l.pos[2] - m->mPosition.z;
		const f32 dy = l.pos[1] - m->mPosition.y;
		static const bool sTestLog = std::getenv("SMS_PORT_MP_SHINERUN_TEST") != nullptr;
		if (sTestLog && l.age % 30 == 0)
			std::fprintf(stderr, "[mp-shinerun] test: drop %u age %d, Mario %.0f away, dy %.0f\n", l.id, l.age,
			             std::sqrt(dx * dx + dz * dz), dy);
		if (sTestLog && l.resting && l.age == 90) { // tests: walk onto it
			m->mPosition.set(l.pos[0] + 30.0f, l.pos[1] - 40.0f, l.pos[2]);
			std::fprintf(stderr, "[mp-shinerun] test: Mario moves onto drop %u\n", l.id);
			continue;
		}
		if (sTestLog && (l.age == 120 || l.age == 135))
			port_shot_request(800000 + l.age); // tests: the Shine next to Mario
		if (sTestLog && l.age < 150)
			continue; // tests: let the camera arrive (screenshots) before taking it
		if (dx * dx + dz * dz < 100.0f * 100.0f && dy > -60.0f && dy < 220.0f) {
			l.asked = true;
			if (port_mp_is_host()) {
				hostGrab(self, l.id);
			} else {
				GrabMsg g = { MSG_GRAB, 0, l.id };
				port_mp_send(kChannel, &g, (int)sizeof(g), 1, -1);
			}
		}
	}
}

// ---- every client: the round ---------------------------------------------------------------------
void toastChanges()
{
	const int m = me();
	char t[160], nm[32];
	if (sState.round != sSeen.round && sState.phase == SR_READY)
		port_mp_toast("Shine Runners: get the most Shine Sprites! Hit others to knock theirs out.");
	if (sState.phase == SR_PLAY && sSeen.phase != SR_PLAY) {
		std::snprintf(t, sizeof(t), "Go! First to %d Shine Sprites wins.", sState.goal);
		port_mp_toast(t);
	}
	if (sState.round == sSeen.round && sState.phase == SR_PLAY && sState.lastGrab != sSeen.lastGrab
	    && sState.lastGrab != 0) {
		if (sState.lastGrabber == m) {
			std::snprintf(t, sizeof(t), "You grabbed a Shine Sprite! (%d)", sState.scores[m]);
			if (SMSGetMSound() != nullptr)
				SMSGetMSound()->startSoundSystemSE(MSD_SE_SY_BLUE_COIN_GET, 0, nullptr, 0);
		} else {
			std::snprintf(t, sizeof(t), "%s grabbed a Shine Sprite.", nameOf(sState.lastGrabber, nm, sizeof(nm)));
		}
		port_mp_toast(t);
	}
	if (sState.phase == SR_OVER && sSeen.phase != SR_OVER) {
		if (sState.winner < PORT_MP_MAX_PLAYERS)
			std::snprintf(t, sizeof(t), "%s wins with %d Shine Sprites!", nameOf(sState.winner, nm, sizeof(nm)),
			              sState.scores[sState.winner]);
		else
			std::snprintf(t, sizeof(t), "Time is up: a draw!");
		port_mp_toast(t);
		std::fprintf(stderr, "[mp-shinerun] %s\n", t);
	}
	sSeen = sState;
}

void sendHost(const void* msg, int n)
{
	port_mp_send(kChannel, msg, n, 1, -1);
}

void watchDeath()
{
	static int sLastState = -1;
	const int st = gpMarDirector != nullptr ? gpMarDirector->mState : -1;
	const bool died = st == TMarDirector::STATE_UNK7 && sLastState != TMarDirector::STATE_UNK7;
	sLastState = st;
	if (!died || sState.phase != SR_PLAY || !inRound(me()))
		return;
	if (port_mp_is_host()) {
		int goal, timeS, loss;
		port_mp_shinerun_settings(&goal, &timeS, &loss);
		hostScore(me(), -loss, "died");
	} else {
		const u8 msg = MSG_DIED;
		sendHost(&msg, 1);
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
		} else if (buf[0] == MSG_SHINE && n == (int)sizeof(ShineMsg)) {
			hostShine(from, ((const ShineMsg*)buf)->id);
		} else if (buf[0] == MSG_DIED && n == 1) {
			int goal, timeS, loss;
			port_mp_shinerun_settings(&goal, &timeS, &loss);
			hostScore(from, -loss, "died");
		} else if (buf[0] == MSG_DROP && n == (int)sizeof(DropMsg)) {
			hostDrop(from, ((const DropMsg*)buf)->pos);
		} else if (buf[0] == MSG_GRAB && n == (int)sizeof(GrabMsg)) {
			hostGrab(from, ((const GrabMsg*)buf)->id);
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
	// every level Shine counted this round is marked taken in our save
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
	if (sState.phase == SR_OFF)
		std::memset(sApplied, 0, sizeof(sApplied));
	toastChanges();
	watchDeath();
	TMario* m = gpMarioOriginal;
	const bool playable = m != nullptr && gpMarDirector != nullptr && gpMarDirector->mState == TMarDirector::STATE_UNK4;
	if (running())
		dropsFrame(playable ? m : nullptr);
	// SMS_PORT_MP_SHINERUN_TEST=1 (tests): once play starts, report a level Shine (5 + slot) twice,
	// then knock one out of ourselves 3 s later (as if hit)
	static const bool sTest = std::getenv("SMS_PORT_MP_SHINERUN_TEST") != nullptr;
	static u8 sTestRound    = 0xFF;
	static double sTestHitAt;
	if (sTest && sState.phase == SR_PLAY && sTestRound != sState.round && playable) {
		sTestRound = sState.round;
		sTestHitAt = port_mp_time_ms() + 3000.0;
		std::fprintf(stderr, "[mp-shinerun] test: reporting Shine %d twice\n", 5 + me());
		port_mp_shinerun_shine_got(5 + me());
		port_mp_shinerun_shine_got(5 + me());
	}
	if (sTest && sTestHitAt > 0.0 && port_mp_time_ms() >= sTestHitAt && playable) {
		sTestHitAt = 0.0;
		std::fprintf(stderr, "[mp-shinerun] test: as if hit\n");
		port_mp_shinerun_hit(-1);
	}
}

struct Register {
	Register() { port_mp_register_game_frame(gameFrame); }
} sRegister;

} // namespace

extern "C" {

int port_mp_shinerun_running(void) { return running() ? 1 : 0; }

// port_mp_game.cpp (the shine-get demo).
void port_mp_shinerun_shine_got(int shineId)
{
	if (!running() || sState.phase != SR_PLAY || !inRound(me()))
		return;
	if (port_mp_is_host()) {
		hostShine(me(), shineId);
	} else {
		ShineMsg msg = { MSG_SHINE, (u8)shineId };
		port_mp_send(kChannel, &msg, (int)sizeof(msg), 1, -1);
	}
}

// port_mp_interact.cpp: another player's hit reached our Mario (the knock-back happened).
void port_mp_shinerun_hit(int fromSlot)
{
	(void)fromSlot;
	if (!running() || sState.phase != SR_PLAY || !inRound(me()) || sState.scores[me()] == 0)
		return;
	if (port_mp_time_ms() < sDropCooldownUntil || gpMarioOriginal == nullptr)
		return;
	sDropCooldownUntil = port_mp_time_ms() + 2500.0;
	const TMario* m    = gpMarioOriginal;
	DropMsg msg        = { MSG_DROP, {}, { m->mPosition.x, m->mPosition.y, m->mPosition.z } };
	std::fprintf(stderr, "[mp-shinerun] hit: a Shine jumps out\n");
	if (port_mp_is_host())
		hostDrop(me(), msg.pos);
	else
		port_mp_send(kChannel, &msg, (int)sizeof(msg), 1, -1);
}

// port_mp_puppet.cpp, at the end of its view object's perform: the bouncing Shines here.
void port_mp_shinerun_perform(unsigned cue, void* graphics)
{
	(void)graphics;
	if (!running())
		return;
	resetIfNewStage();
	if (cue & CUE_ENTRY)
		for (int k = 0; k < kMaxDrops; k++) {
			LocalDrop& l = sLooks.drop[k];
			l.drawn      = false;
			if (l.id == 0)
				continue;
			J3DModel* md = dropModel(k);
			if (md == nullptr)
				continue;
			Mtx base;
			MsMtxSetRotRPH(base, 0.0f, l.spin, 0.0f);
			base[0][3] = l.pos[0];
			base[1][3] = l.pos[1];
			base[2][3] = l.pos[2];
			md->setBaseTRMtx(base);
			md->calc();
			l.drawn = true;
		}
	if (cue & CUE_CALC_VIEW)
		for (int k = 0; k < kMaxDrops; k++)
			if (sLooks.drop[k].drawn)
				sLooks.model[k]->viewCalc();
	if (cue & CUE_ENTRY)
		for (int k = 0; k < kMaxDrops; k++)
			if (sLooks.drop[k].drawn)
				sLooks.model[k]->entry();
}

int port_mp_shinerun_headline(char* out, int cap)
{
	char nm[32];
	const int left = secondsLeft();
	bool tie;
	const int best = leader(&tie);
	const int m    = me();
	const int mine = inRound(m) ? sState.scores[m] : 0;
	switch (sState.phase) {
	case SR_READY: std::snprintf(out, cap, "Shine Runners: ready... %d", left); return 1;
	case SR_PLAY:
		if (sDeadline > 0.0)
			std::snprintf(out, cap, "Shine Runners  %d:%02d  (you %d/%d, lead: %s %d)", left / 60, left % 60, mine,
			              sState.goal, best >= 0 && !tie ? nameOf(best, nm, sizeof(nm)) : "-", best >= 0 ? sState.scores[best] : 0);
		else
			std::snprintf(out, cap, "Shine Runners  (you %d/%d, lead: %s %d)", mine, sState.goal,
			              best >= 0 && !tie ? nameOf(best, nm, sizeof(nm)) : "-", best >= 0 ? sState.scores[best] : 0);
		return 1;
	case SR_OVER:
		if (sState.winner < PORT_MP_MAX_PLAYERS)
			std::snprintf(out, cap, "Shine Runners: %s wins", nameOf(sState.winner, nm, sizeof(nm)));
		else
			std::snprintf(out, cap, "Shine Runners: a draw");
		return 1;
	default: return 0;
	}
}

// The player list: each player's Shines ("3/5").
const char* port_mp_shinerun_role(int slot)
{
	if (sState.phase == SR_OFF || !inRound(slot))
		return nullptr;
	static char sBuf[PORT_MP_MAX_PLAYERS][16];
	std::snprintf(sBuf[slot], sizeof(sBuf[slot]), "%d/%d", sState.scores[slot], sState.goal);
	return sBuf[slot];
}

} // extern "C"

#endif // SMS_PORT_AURORA
