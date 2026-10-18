// Multiplayer NPC sync (w12, task board P8, NPC sub-package; design: port/MULTIPLAYER.md "NPC
// sync (P8, w12)"). Sibling of w11's port_mp_objects.cpp (enemies, platforms): same model, own
// channel (PORT_MP_CH_NPCS), behind the same host setting "Sync enemies: Experimental"
// (PortMpSessionSettings.reserved[2]).
//
//  - Synced: every TBaseNPC in the stage's NPC managers (Piantas, Nokis, Toadsworth, Peach,
//    Toads, villagers, Il Piantissimo's look-alikes, ...) except the dummy talk targets and
//    sign boards. Ids = (manager index in TConductor's list) << 16 | (object index): scene
//    objects are created in scene-file order on every client.
//  - Each NPC has one owner, the player nearest to it, chosen by the stage leader (lowest slot
//    in the stage + scenario) with hysteresis. The owner runs it with the game's own code (AI,
//    walking, reactions to its own Mario: spray, trample, carrying) and sends position, angles,
//    animation (BCK index, frame, rate), alive/held flags and the goop (pollution) amount at
//    15 Hz (changes) plus a 1 s keyframe. Everyone else shows a puppet: TBaseNPC::perform skips
//    moveObject and the walk/turn bookkeeping (one SMS_PORT_AURORA hook in NpcBase.cpp) and
//    takes the interpolated pose and the owner's animation; drawing, shadows and hit
//    collision stay.
//  - Talk stays local: the talk-eligibility pass (changeNerveProc_) still runs on puppets, and
//    an NPC the local Mario is talking to, holding, or that is in a talk/demo of ours runs
//    locally until that ends (the owner keeps sending; the puppet resumes afterwards).
// Nothing runs unless a session is active and the host chose "Experimental"; single player
// never takes the puppet path (gPortMpNpcPuppets == 0).
#ifdef SMS_PORT_AURORA

#include "port_mp.h"

#include <Enemy/Conductor.hpp>
#include <Enemy/Graph.hpp>
#include <Map/Map.hpp>
#include <Map/MapData.hpp>
#include <M3DUtil/MActor.hpp>
#include <NPC/NpcBase.hpp>
#include <NPC/NpcInbetween.hpp>
#include <NPC/NpcParts.hpp>
#include <NPC/NpcSave.hpp>
#include <Player/Mario.hpp>
#include <Player/MarioAccess.hpp>
#include <Player/ModelWaterManager.hpp>
#include <Strategic/HitActor.hpp>
#include <Strategic/ActorTypes.hpp>
#include <Strategic/LiveActor.hpp>
#include <Strategic/LiveManager.hpp>
#include <System/Application.hpp>
#include <System/MarDirector.hpp>
#include <JSystem/JDrama/JDRNameRefGen.hpp>
#include <JSystem/J3D/J3DGraphAnimator/J3DAnimation.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

extern "C" unsigned port_frame_count(void); // port_crash.cpp

// Read by the hook in TBaseNPC::perform: number of NPCs currently driven by the network.
extern "C" int gPortMpNpcPuppets = 0;

namespace {

const int kResSync = 2; // PortMpSessionSettings.reserved[2] (w11's "Sync enemies"): 1 = experimental

// ---- wire (PORT_MP_CH_NPCS, packed, little-endian) -------------------------------------------
enum MsgType : u8 { MSG_STATE = 1, MSG_OWNERS = 2, MSG_HIT = 3 };
// Parts models with their own BCK (TNpcParts slot at LOD row 0, as TBaseNPC::setNpcAnm_ drives
// them): Peach's hair 0, ponytail 3, parasol 4; the Noki elders' parts 9 (MB) and 10 (WB).
const int kPartSlots[5] = { 0, 3, 4, 9, 10 };
#pragma pack(push, 1)
struct MsgHeader {
	u8 type;
	u8 stage, scenario;
	u8 count;
	u32 seq;
};
struct StateEntry {
	u32 id;
	u8 flags;     // bit 0 alive, bit 1 held by its owner's Mario, bit 2 hidden
	u8 pollution; // goop amount 0..255 (TBaseNPC::mPollutionAmount, 0 = clean)
	s16 bck;      // current BCK index of the NPC's MActor (-1: none)
	float pos[3];
	s16 rot[3];   // degrees * 65536 / 360
	u16 frame;    // BCK frame * 8
	s16 rate;     // BCK rate * 256
	u8 btp1;      // w15: current BTP (face pattern) index + 1, 0 = none / not sent
	u8 btk1;      // w15: current BTK (texture scroll) index + 1
	// v2 (Peach's umbrella report): the parts' animations and the trample squash
	s8 parts[5];  // BCK index of the parts in kPartSlots (-1 none, -2 no such part)
	u8 state1D8;  // TBaseNPC::unk1D8 (Peach: parasol in / tired / recovered: chooses the parts shown)
	u8 scaleY;    // mScaling.y / mInitialScale.y * 64 (the trample squash)
	u8 partBtp1;  // BTP index + 1 of the slot-10 part (Noki elder WB), 0 = none
};
struct HitBody { // MSG_HIT, to the owner: our Mario / water hit its NPC
	u32 id;
	u32 message;
	float pos[3]; // attacker (Mario, or the water particle)
	u8 water;
	u8 pad[3];
};
struct OwnerEntry {
	u32 id;
	u8 slot;
	u8 pad[3];
};
#pragma pack(pop)
static_assert(sizeof(StateEntry) == 40, "NPC state entry size");
static_assert(sizeof(HitBody) == 24, "NPC hit body size");
const int kMaxState  = (PORT_MP_APP_MAX - (int)sizeof(MsgHeader)) / (int)sizeof(StateEntry);
const int kMaxOwners = (PORT_MP_APP_MAX - (int)sizeof(MsgHeader)) / (int)sizeof(OwnerEntry);

struct Sample {
	double t;
	float pos[3];
	float rot[3];
};

struct Npc {
	TBaseNPC* a;
	u32 id;
	int owner; // slot; -1 = the stage leader
	int pendingOwner;
	double ownerSince;
	// owner side
	float lastPos[3], lastRot[3];
	int lastBck, lastFlags, lastPoll, lastBtp, lastBtk;
	int lastParts[5], lastState1D8, lastScaleY, lastPartBtp;
	// receiving side
	Sample s[3];
	int ns;
	int bck, flags, poll, btp, btk; // btp/btk: index + 1, 0 = leave alone
	int parts[5], state1D8, scaleY, partBtp;
	float frame, rate;
	bool puppet;
	bool localOverride; // talking to / holding it here
	double lastWaterHit; // puppet side: last water hit forwarded to the owner
};

std::vector<Npc> sNpcs;
std::unordered_map<u32, int> sById;
std::unordered_map<TBaseNPC*, int> sByActor;
TMarDirector* sVisitDir;
unsigned sVisitDirSerial; // gPortMpStageSerial of this visit
int sVisitStage = -1, sVisitScenario = -1;
int sFramesInVisit;
bool sActive;
u32 sSendSeq, sOwnerSeq, sOwnerSeen;
double sLastSent, sLastKeyframe, sLastOwnerCalc, sLastOwnerSent;
bool sOwnersDirty;
int sLeaderSeen = -1;
int sStatOwned, sStatPuppets, sStatSent, sStatRx, sStatHitsSent, sStatHitsApplied;
bool sApplyingRemote; // the owner is applying a forwarded hit: do not forward it again

bool logOn()
{
	static const bool sLog = std::getenv("SMS_PORT_MP_NPC_LOG") != nullptr;
	return sLog;
}

inline s16 encAngle(float deg) { return (s16)(int)std::lround(std::fmod(deg, 360.0f) * (65536.0f / 360.0f)); }
inline float decAngle(s16 v) { return v * (360.0f / 65536.0f); }
inline float lerpAngle(float a, float b, float t)
{
	float d = std::fmod(b - a, 360.0f);
	if (d > 180.0f)
		d -= 360.0f;
	if (d < -180.0f)
		d += 360.0f;
	return a + d * t;
}

bool isSyncedNpc(const TLiveActor* a)
{
	if (!a->isHitCategory(HIT_CATEGORY_NPC))
		return false;
	const u32 t = a->getActorType();
	return t != ACTOR_TYPE_NPC_DUMMY && t != ACTOR_TYPE_NPC_BOARD;
}

int stagePlayers(int* slots)
{
	int n        = 0;
	const int me = port_mp_local_slot();
	for (int i = 0; i < PORT_MP_MAX_PLAYERS; i++)
		if (i == me || port_mp_same_stage(i))
			slots[n++] = i;
	return n;
}

int leaderSlot()
{
	int slots[PORT_MP_MAX_PLAYERS];
	const int n = stagePlayers(slots);
	return n > 0 ? slots[0] : port_mp_local_slot();
}

int effectiveOwner(const Npc& o) { return o.owner >= 0 ? o.owner : leaderSlot(); }

bool playerPos(int slot, float out[3])
{
	if (slot == port_mp_local_slot()) {
		if (gpMarioOriginal == nullptr)
			return false;
		out[0] = gpMarioOriginal->mPosition.x, out[1] = gpMarioOriginal->mPosition.y;
		out[2] = gpMarioOriginal->mPosition.z;
		return true;
	}
	PortMpSnapshotRx rx;
	if (port_mp_get_snapshots(slot, &rx, 1) < 1 || port_mp_time_ms() - rx.rxMs > 3000.0)
		return false;
	std::memcpy(out, rx.snap.pos, sizeof(float) * 3);
	return true;
}

void resetVisit()
{
	for (Npc& o : sNpcs)
		o.puppet = false;
	gPortMpNpcPuppets = 0;
	sNpcs.clear();
	sById.clear();
	sByActor.clear();
	sSendSeq = sOwnerSeq = sOwnerSeen = 0;
	sLastSent = sLastKeyframe = sLastOwnerCalc = sLastOwnerSent = 0.0;
	sOwnersDirty = false;
	sLeaderSeen  = -1;
}

int npcFlags(const TBaseNPC* a)
{
	int f = 0;
	if (!(a->mLiveFlag & LIVE_FLAG_DEAD))
		f |= 1;
	if (a->mHolder != nullptr)
		f |= 2;
	if (a->mLiveFlag & LIVE_FLAG_HIDDEN)
		f |= 4;
	return f;
}

int npcPollution(const TBaseNPC* a)
{
	float p = a->mPollutionAmount;
	p       = p < 0.0f ? 0.0f : (p > 1.0f ? 1.0f : p);
	return (int)std::lround(p * 255.0f);
}

int npcBck(TBaseNPC* a)
{
	MActor* m = a->getMActor();
	return m != nullptr ? m->getCurAnmIdx(ANM_TYPE_BCK) : -1;
}

// The parts' MActors (hair, parasol, ...) at LOD row 0, the ones TBaseNPC::setNpcAnm_ animates.
MActor* npcPart(TBaseNPC* a, int k)
{
	if (a->unk168 == nullptr || !a->isPartsAnmNpc())
		return nullptr;
	return a->unk168->getPartsMActor(kPartSlots[k], 0);
}

void npcParts(TBaseNPC* a, int parts[5], int* partBtp)
{
	*partBtp = 0;
	for (int k = 0; k < 5; k++) {
		MActor* m = npcPart(a, k);
		parts[k]  = m != nullptr ? m->getCurAnmIdx(ANM_TYPE_BCK) : -2;
		if (m != nullptr && kPartSlots[k] == 10)
			*partBtp = m->getCurAnmIdx(ANM_TYPE_BTP) + 1;
	}
}

int npcScaleY(const TBaseNPC* a)
{
	const float base = a->mInitialScale.y;
	float r          = base > 1e-6f ? a->mScaling.y / base : 1.0f;
	r                = r < 0.0f ? 0.0f : (r > 3.9f ? 3.9f : r);
	return (int)std::lround(r * 64.0f);
}

void buildList()
{
	resetVisit();
	// gpConductor survives into stages without one (never cleared): use it only if this scene's
	// name tree has it (as w11's port_mp_objects.cpp does; TConductor's Shift-JIS name).
	JDrama::TNameRefGen* gen = JDrama::TNameRefGen::getInstance();
	JDrama::TNameRef* found  = nullptr;
	if (gpConductor != nullptr && gen != nullptr && gen->getRootNameRef() != nullptr)
		found = gen->getRootNameRef()->search("\x83\x52\x83\x93\x83\x5F\x83\x4E\x83\x5E\x81\x5B");
	if (gpConductor == nullptr || found != (JDrama::TNameRef*)gpConductor) {
		std::fprintf(stderr, "[mp-npc] stage %d/%d: no conductor, no NPCs to sync\n", sVisitStage, sVisitScenario);
		return;
	}
	int mgrIdx = 0;
	for (JGadget::TList<TLiveManager*>::iterator it = gpConductor->unk10.begin(), e = gpConductor->unk10.end(); it != e;
	     ++it, ++mgrIdx) {
		TLiveManager* m = *it;
		if (m == nullptr)
			continue;
		for (int i = 0; i < m->getObjNum(); i++) {
			TLiveActor* a = m->getObj(i);
			if (a == nullptr || !isSyncedNpc(a))
				continue;
			Npc o;
			std::memset(&o, 0, sizeof(o));
			o.a            = (TBaseNPC*)a;
			o.id           = ((u32)mgrIdx << 16) | (u32)i;
			o.owner        = -1;
			o.pendingOwner = -1;
			o.lastBck = o.lastFlags = o.lastPoll = o.lastBtp = o.lastBtk = -2;
			o.lastState1D8 = o.lastScaleY = o.lastPartBtp = -2;
			for (int k = 0; k < 5; k++)
				o.lastParts[k] = -3, o.parts[k] = -2;
			o.bck                                = -1;
			o.state1D8                           = -1;
			o.scaleY                             = 64;
			sById[o.id]                          = (int)sNpcs.size();
			sByActor[o.a]                        = (int)sNpcs.size();
			sNpcs.push_back(o);
		}
	}
	std::fprintf(stderr, "[mp-npc] stage %d/%d: %d synced NPCs\n", sVisitStage, sVisitScenario, (int)sNpcs.size());
}

// ---- receiving -------------------------------------------------------------------------------
void onState(int from, const StateEntry* e, int n)
{
	const double now = port_mp_time_ms();
	for (int i = 0; i < n; i++) {
		auto it = sById.find(e[i].id);
		if (it == sById.end())
			continue;
		Npc& o = sNpcs[it->second];
		if (effectiveOwner(o) != from)
			continue;
		sStatRx++;
		Sample s;
		s.t = now;
		std::memcpy(s.pos, e[i].pos, sizeof(s.pos));
		const float cur[3] = { o.a->mRotation.x, o.a->mRotation.y, o.a->mRotation.z };
		for (int k = 0; k < 3; k++)
			s.rot[k] = lerpAngle(o.ns > 0 ? o.s[0].rot[k] : cur[k], decAngle(e[i].rot[k]), 1.0f);
		o.s[2] = o.s[1];
		o.s[1] = o.s[0];
		o.s[0] = s;
		if (o.ns < 3)
			o.ns++;
		o.flags = e[i].flags;
		o.poll  = e[i].pollution;
		o.bck   = e[i].bck;
		o.frame = e[i].frame / 8.0f;
		o.rate  = e[i].rate / 256.0f;
		o.btp   = e[i].btp1;
		o.btk   = e[i].btk1;
		for (int k = 0; k < 5; k++)
			o.parts[k] = e[i].parts[k];
		o.state1D8 = e[i].state1D8;
		o.scaleY   = e[i].scaleY;
		o.partBtp  = e[i].partBtp1;
	}
}

// The owner applies a hit that another player's Mario or water landed on its NPC: the NPC's own
// reaction runs here (Peach raises her umbrella, a Pianta gets wet or squashed) and reaches
// everyone with the next state.
void onHit(int from, const HitBody& b)
{
	auto it = sById.find(b.id);
	if (it == sById.end())
		return;
	Npc& o = sNpcs[it->second];
	if (effectiveOwner(o) != port_mp_local_slot() || o.puppet || (o.a->mLiveFlag & LIVE_FLAG_DEAD))
		return;
	sApplyingRemote = true;
	if (b.water) {
		auto& w = TModelWaterManager::mStaticHitActor;
		w.mPosition.set(b.pos[0], b.pos[1], b.pos[2]);
		w.unk68 = 0;
		o.a->receiveMessage(&w, HIT_MESSAGE_SPRAYED_BY_WATER);
	} else if (gpMarioOriginal != nullptr) {
		// our Mario stands in for the other player's for the length of the call
		TMario* m                         = gpMarioOriginal;
		const JGeometry::TVec3<f32> saved = m->mPosition;
		m->mPosition.set(b.pos[0], b.pos[1], b.pos[2]);
		o.a->receiveMessage(m, b.message);
		m->mPosition = saved;
	}
	sApplyingRemote = false;
	sStatHitsApplied++;
	if (logOn())
		std::fprintf(stderr, "[mp-npc] hit %u on %08x from slot %d applied%s\n", b.message, b.id, from,
		             b.water ? " (water)" : "");
}

void onOwners(int from, const MsgHeader& h, const OwnerEntry* e, int n, bool first)
{
	if (from != leaderSlot())
		return;
	if (first) {
		if (h.seq < sOwnerSeen && from == sLeaderSeen)
			return;
		sOwnerSeen  = h.seq;
		sLeaderSeen = from;
		for (Npc& o : sNpcs)
			o.pendingOwner = -1;
	}
	for (int i = 0; i < n; i++) {
		auto it = sById.find(e[i].id);
		if (it != sById.end())
			sNpcs[it->second].pendingOwner = e[i].slot;
	}
	for (Npc& o : sNpcs) {
		if (o.owner != o.pendingOwner) {
			o.ownerSince = port_mp_time_ms();
			o.ns         = 0;
		}
		o.owner = o.pendingOwner;
	}
}

void netPoll()
{
	u8 buf[PORT_MP_APP_MAX];
	int from, n;
	while ((n = port_mp_recv(PORT_MP_CH_NPCS, buf, sizeof(buf), &from)) != 0) {
		if (!sActive || n < (int)sizeof(MsgHeader) || from < 0 || from >= PORT_MP_MAX_PLAYERS)
			continue;
		MsgHeader h;
		std::memcpy(&h, buf, sizeof(h));
		if (h.stage != sVisitStage || h.scenario != sVisitScenario)
			continue;
		const u8* body = buf + sizeof(h);
		const int len  = n - (int)sizeof(h);
		if (h.type == MSG_STATE && len == h.count * (int)sizeof(StateEntry))
			onState(from, (const StateEntry*)body, h.count);
		else if ((h.type == MSG_OWNERS || h.type == (MSG_OWNERS | 0x80)) && len == h.count * (int)sizeof(OwnerEntry))
			onOwners(from, h, (const OwnerEntry*)body, h.count, h.type == MSG_OWNERS);
		else if (h.type == MSG_HIT && len == (int)sizeof(HitBody)) {
			HitBody b;
			std::memcpy(&b, body, sizeof(b));
			onHit(from, b);
		}
	}
}

// ---- leader ----------------------------------------------------------------------------------
void leaderAssign()
{
	const double now = port_mp_time_ms();
	if (now - sLastOwnerCalc < 500.0)
		return;
	sLastOwnerCalc = now;
	int slots[PORT_MP_MAX_PLAYERS];
	float pos[PORT_MP_MAX_PLAYERS][3];
	bool have[PORT_MP_MAX_PLAYERS];
	const int np = stagePlayers(slots);
	for (int i = 0; i < np; i++)
		have[i] = playerPos(slots[i], pos[i]);
	const int me = port_mp_local_slot();
	for (Npc& o : sNpcs) {
		const float* op = &o.a->mPosition.x;
		int best = -1, cur = -1;
		float bestD = 1e30f, curD = 1e30f;
		const int curOwner = o.owner >= 0 ? o.owner : me;
		for (int i = 0; i < np; i++) {
			if (!have[i])
				continue;
			const float dx = pos[i][0] - op[0], dy = pos[i][1] - op[1], dz = pos[i][2] - op[2];
			const float d  = dx * dx + dy * dy + dz * dz;
			if (d < bestD)
				bestD = d, best = slots[i];
			if (slots[i] == curOwner)
				curD = d, cur = slots[i];
		}
		if (best < 0 || best == curOwner)
			continue;
		const float bd = std::sqrt(bestD), cd = std::sqrt(curD);
		// a carried NPC stays with its carrier (the owner sends "held")
		const bool held = o.a->mHolder != nullptr || ((o.flags & 2) != 0 && effectiveOwner(o) != me);
		if (cur < 0 || (!held && bd < cd * 0.7f - 100.0f && now - o.ownerSince > 1000.0)) {
			o.owner      = best == me ? -1 : best;
			o.ownerSince = now;
			o.ns         = 0;
			sOwnersDirty = true;
		}
	}
	for (Npc& o : sNpcs)
		if (o.owner >= 0) {
			bool in = false;
			for (int i = 0; i < np; i++)
				in |= slots[i] == o.owner;
			if (!in) {
				o.owner      = -1;
				o.ownerSince = now;
				sOwnersDirty = true;
			}
		}
}

void leaderSendOwners()
{
	const double now    = port_mp_time_ms();
	const bool periodic = now - sLastOwnerSent > 2000.0;
	if (!sOwnersDirty && !periodic)
		return;
	const bool reliable = sOwnersDirty;
	sOwnersDirty        = false;
	sLastOwnerSent      = now;
	++sOwnerSeq;
	u8 buf[PORT_MP_APP_MAX];
	int n = 0, chunk = 0;
	auto flush = [&](bool force) {
		if (n == 0 && !(force && chunk == 0))
			return;
		MsgHeader h;
		h.type     = chunk == 0 ? MSG_OWNERS : (MSG_OWNERS | 0x80);
		h.stage    = (u8)sVisitStage;
		h.scenario = (u8)sVisitScenario;
		h.count    = (u8)n;
		h.seq      = sOwnerSeq;
		std::memcpy(buf, &h, sizeof(h));
		port_mp_send(PORT_MP_CH_NPCS, buf, (int)sizeof(h) + n * (int)sizeof(OwnerEntry), reliable ? 1 : 0, -1);
		n = 0;
		chunk++;
	};
	for (const Npc& o : sNpcs) {
		if (o.owner < 0)
			continue;
		OwnerEntry e;
		std::memset(&e, 0, sizeof(e));
		e.id   = o.id;
		e.slot = (u8)o.owner;
		std::memcpy(buf + sizeof(MsgHeader) + n * sizeof(OwnerEntry), &e, sizeof(e));
		if (++n == kMaxOwners)
			flush(false);
	}
	flush(true);
}

// ---- owner -----------------------------------------------------------------------------------
void ownerSend()
{
	const double now = port_mp_time_ms();
	if (now - sLastSent < 66.0)
		return;
	sLastSent           = now;
	const bool keyframe = now - sLastKeyframe > 1000.0;
	if (keyframe)
		sLastKeyframe = now;
	const int me = port_mp_local_slot();
	u8 buf[PORT_MP_APP_MAX];
	int n      = 0;
	auto flush = [&]() {
		if (n == 0)
			return;
		MsgHeader h;
		h.type     = MSG_STATE;
		h.stage    = (u8)sVisitStage;
		h.scenario = (u8)sVisitScenario;
		h.count    = (u8)n;
		h.seq      = ++sSendSeq;
		std::memcpy(buf, &h, sizeof(h));
		port_mp_send(PORT_MP_CH_NPCS, buf, (int)sizeof(h) + n * (int)sizeof(StateEntry), 0, -1);
		sStatSent += n;
		n = 0;
	};
	sStatOwned = 0;
	for (Npc& o : sNpcs) {
		if (effectiveOwner(o) != me)
			continue;
		sStatOwned++;
		TBaseNPC* a      = o.a;
		const float p[3] = { a->mPosition.x, a->mPosition.y, a->mPosition.z };
		const float r[3] = { a->mRotation.x, a->mRotation.y, a->mRotation.z };
		const int fl = npcFlags(a), po = npcPollution(a), bck = npcBck(a);
		MActor* ma   = a->getMActor();
		const int btp = ma != nullptr ? ma->getCurAnmIdx(ANM_TYPE_BTP) + 1 : 0;
		const int btk = ma != nullptr ? ma->getCurAnmIdx(ANM_TYPE_BTK) + 1 : 0;
		int parts[5], partBtp = 0;
		npcParts(a, parts, &partBtp);
		const int st1D8  = a->unk1D8;
		const int scaleY = npcScaleY(a);
		bool changed = fl != o.lastFlags || po != o.lastPoll || bck != o.lastBck || btp != o.lastBtp || btk != o.lastBtk
		               || st1D8 != o.lastState1D8 || scaleY != o.lastScaleY || partBtp != o.lastPartBtp;
		for (int k = 0; k < 5; k++)
			changed |= parts[k] != o.lastParts[k], o.lastParts[k] = parts[k];
		o.lastBtp = btp, o.lastBtk = btk, o.lastState1D8 = st1D8, o.lastScaleY = scaleY, o.lastPartBtp = partBtp;
		for (int k = 0; k < 3; k++)
			changed |= std::fabs(p[k] - o.lastPos[k]) > 1.0f || std::fabs(r[k] - o.lastRot[k]) > 0.5f;
		std::memcpy(o.lastPos, p, sizeof(p));
		std::memcpy(o.lastRot, r, sizeof(r));
		o.lastFlags = fl, o.lastPoll = po, o.lastBck = bck;
		if (!changed && !keyframe)
			continue;
		StateEntry e;
		std::memset(&e, 0, sizeof(e));
		e.id        = o.id;
		e.flags     = (u8)fl;
		e.pollution = (u8)po;
		e.bck       = (s16)bck;
		e.btp1      = (u8)(btp < 0 || btp > 255 ? 0 : btp);
		e.btk1      = (u8)(btk < 0 || btk > 255 ? 0 : btk);
		for (int k = 0; k < 5; k++)
			e.parts[k] = (s8)(parts[k] < -2 ? -2 : (parts[k] > 127 ? 127 : parts[k]));
		e.state1D8 = (u8)st1D8;
		e.scaleY   = (u8)scaleY;
		e.partBtp1 = (u8)(partBtp < 0 || partBtp > 255 ? 0 : partBtp);
		std::memcpy(e.pos, p, sizeof(p));
		for (int k = 0; k < 3; k++)
			e.rot[k] = encAngle(r[k]);
		MActor* m = a->getMActor();
		J3DFrameCtrl* fc = m != nullptr && bck >= 0 ? m->getFrameCtrl(ANM_TYPE_BCK) : nullptr;
		if (fc != nullptr) {
			float fr = fc->getFrame();
			e.frame  = (u16)(fr < 0.0f ? 0 : (fr > 8191.0f ? 65535 : (int)(fr * 8.0f)));
			e.rate   = (s16)(int)std::lround(fc->getRate() * 256.0f);
		}
		std::memcpy(buf + sizeof(MsgHeader) + n * sizeof(StateEntry), &e, sizeof(e));
		if (++n == kMaxState)
			flush();
	}
	flush();
}

// ---- puppets ---------------------------------------------------------------------------------
// A puppet that starts running here again (we became its owner) was moved by the network
// meanwhile, but its own path walk still aims at the graph node it had before: walking there
// in a straight line went through walls and off ledges. Re-attach it to the graph node nearest
// to where it is now, drop stale speed, and stand it on the ground under it.
void resumeHere(TBaseNPC* a)
{
	TGraphTracer* tr = a->getTracer();
	if (tr != nullptr && tr->getGraph() != nullptr && tr->getGraph()->getNodeNum() > 0)
		tr->setToNearest(a->mPosition);
	a->mVelocity.set(0.0f, 0.0f, 0.0f);
	a->mPositionDelta.zero();
	if (gpMap != nullptr && !a->isAirborne()) {
		const TBGCheckData* plane = nullptr;
		const f32 y = gpMap->checkGround(a->mPosition.x, a->mPosition.y + 200.0f, a->mPosition.z, &plane);
		if (plane != nullptr && !plane->isIllegalData() && y > a->mPosition.y - 400.0f)
			a->mPosition.y = y;
	}
	if (logOn())
		std::fprintf(stderr, "[mp-npc] resumed at %.0f %.0f %.0f, graph node %d\n", a->mPosition.x, a->mPosition.y,
		             a->mPosition.z, tr != nullptr ? tr->getCurGraphIndex() : -1);
}

void updatePuppets()
{
	const double now = port_mp_time_ms();
	const int me     = port_mp_local_slot();
	int puppets      = 0;
	for (Npc& o : sNpcs) {
		TBaseNPC* a = o.a;
		// local override: our Mario holds it, talks to it, or it is in one of our demos
		const bool talkingToIt = gpMarDirector != nullptr && gpMarDirector->isTalkOrDemoModeNow()
		                         && gpMarDirector->unkA0 == a;
		const bool heldHere = a->mHolder != nullptr && a->mHolder == (TTakeActor*)gpMarioOriginal;
		o.localOverride     = talkingToIt || heldHere;
		// 5 s without news before running it here: keyframes come once a second, unreliably, so
		// 2 s let a couple of lost packets flip an NPC between puppet and local AI (it then
		// walked off on its stale path, through walls).
		const bool remote   = effectiveOwner(o) != me && o.ns > 0 && now - o.s[0].t < 5000.0 && !o.localOverride;
		if (remote != o.puppet && logOn())
			std::fprintf(stderr, "[mp-npc] %08x %s\n", o.id, remote ? "now a puppet" : "runs here now");
		if (o.puppet && !remote)
			resumeHere(a);
		o.puppet = remote;
		if (!remote)
			continue;
		// alive / hidden follow the owner
		if ((o.flags & 1) && (a->mLiveFlag & LIVE_FLAG_DEAD))
			a->mLiveFlag &= ~LIVE_FLAG_DEAD;
		else if (!(o.flags & 1) && !(a->mLiveFlag & LIVE_FLAG_DEAD))
			a->mLiveFlag |= LIVE_FLAG_DEAD;
		puppets++;
	}
	sStatPuppets      = puppets;
	gPortMpNpcPuppets = puppets;
}

void logFrame()
{
	if (!logOn())
		return;
	static double sAt;
	if (port_mp_time_ms() - sAt < 5000.0)
		return;
	sAt = port_mp_time_ms();
	std::fprintf(stderr, "[mp-npc] stage %d/%d leader %d: %d NPCs, owned %d, puppets %d, sent %d, rx %d, hits sent %d applied %d\n",
	             sVisitStage, sVisitScenario, leaderSlot(), (int)sNpcs.size(), sStatOwned, sStatPuppets, sStatSent, sStatRx,
	             sStatHitsSent, sStatHitsApplied);
	int shown = 0;
	for (const Npc& o : sNpcs) {
		if (shown >= 5)
			break;
		shown++;
		MActor* lm = o.a->getMActor();
		J3DFrameCtrl* lfc = lm != nullptr && npcBck(o.a) >= 0 ? lm->getFrameCtrl(ANM_TYPE_BCK) : nullptr;
		std::fprintf(stderr, "[mp-npc]   %08x type %08x owner %d pos %.0f %.0f %.0f rotY %.1f bck %d f %.1f r %.2f btp %d poll %d%s%s\n", o.id,
		             (unsigned)o.a->getActorType(), effectiveOwner(o), o.a->mPosition.x, o.a->mPosition.y,
		             o.a->mPosition.z, o.a->mRotation.y, npcBck(o.a), lfc ? lfc->getFrame() : -1.0f, lfc ? lfc->getRate() : 0.0f,
		             lm != nullptr ? lm->getCurAnmIdx(ANM_TYPE_BTP) : -1, npcPollution(o.a), o.puppet ? " (puppet)" : "",
		             o.localOverride ? " (local: talk/held)" : "");
	}
}

// "peach", "monte", "kinopio", ... or a hex actor type: the NPC type an action / the watch picks.
u32 parseNpcType(const char* s)
{
	if (s == nullptr || *s == '\0')
		return 0;
	if (std::strcmp(s, "peach") == 0)
		return ACTOR_TYPE_NPC_PEACH;
	if (std::strcmp(s, "kinojii") == 0)
		return ACTOR_TYPE_NPC_KINOJII;
	if (std::strcmp(s, "kinopio") == 0)
		return ACTOR_TYPE_NPC_KINOPIO;
	return (u32)std::strtoul(s, nullptr, 16);
}

Npc* pickNpc(u32 type, bool wantRemote)
{
	if (gpMarioOriginal == nullptr)
		return nullptr;
	const JGeometry::TVec3<f32>& mp = gpMarioOriginal->mPosition;
	Npc* best   = nullptr;
	float bestD = 1e30f;
	for (Npc& o : sNpcs) {
		if (o.a->mLiveFlag & LIVE_FLAG_DEAD)
			continue;
		if (type != 0 && o.a->getActorType() != type)
			continue;
		if (wantRemote && !o.puppet)
			continue;
		const float dx = o.a->mPosition.x - mp.x, dz = o.a->mPosition.z - mp.z;
		const float d  = dx * dx + dz * dz;
		if (d < bestD)
			best = &o, bestD = d;
	}
	return best;
}

// SMS_PORT_MP_NPC_TEST="<name>:<action>[:<type>]@<frame>;..." (debug, with mp_test.py's names;
// <type> = peach | kinojii | kinopio | hex actor type, default: the nearest synced NPC):
//   list  = log every synced NPC (id, type, owner, position)
//   near  = put Mario next to the NPC (ownership moves to us after the leader's hysteresis)
//   spray = water from our Mario on the NPC through the real path (receiveMessage with the
//           water manager's hit actor: forwarded to the owner when it is a puppet here)
//   stomp = a trample from our Mario on the NPC, same path
void testKnob()
{
	static const char* sTest = std::getenv("SMS_PORT_MP_NPC_TEST");
	static unsigned sDone;
	if (sTest == nullptr || *sTest == '\0' || gpMarioOriginal == nullptr)
		return;
	const char* me = port_mp_get_name();
	int idx        = 0;
	for (const char* p = sTest; *p != '\0' && idx < 32; idx++) {
		const char* end  = std::strchr(p, ';');
		const size_t len = end ? (size_t)(end - p) : std::strlen(p);
		char e[96];
		std::snprintf(e, sizeof(e), "%.*s", (int)(len < 95 ? len : 95), p);
		p           = end ? end + 1 : p + len;
		char* colon = std::strchr(e, ':');
		char* at    = std::strchr(e, '@');
		if (colon == nullptr || at == nullptr || (sDone & (1u << idx)))
			continue;
		*colon = *at = '\0';
		if (std::strcmp(e, me) != 0 || port_frame_count() < (unsigned)std::strtoul(at + 1, nullptr, 10))
			continue;
		sDone |= 1u << idx;
		char* action = colon + 1;
		char* type   = std::strchr(action, ':');
		if (type != nullptr)
			*type++ = '\0';
		if (std::strcmp(action, "list") == 0) {
			for (const Npc& o : sNpcs)
				std::fprintf(stderr, "[mp-npc] test list: %08x type %08x owner %d%s pos %.0f %.0f %.0f\n", o.id,
				             (unsigned)o.a->getActorType(), effectiveOwner(o), o.puppet ? " (puppet)" : "",
				             o.a->mPosition.x, o.a->mPosition.y, o.a->mPosition.z);
			continue;
		}
		Npc* best = pickNpc(parseNpcType(type), false);
		if (best == nullptr) {
			std::fprintf(stderr, "[mp-npc] test %s: no NPC\n", action);
			continue;
		}
		TBaseNPC* a = best->a;
		if (std::strcmp(action, "near") == 0) {
			JGeometry::TVec3<f32> to(a->mPosition.x + 250.0f, a->mPosition.y + 50.0f, a->mPosition.z);
			std::fprintf(stderr, "[mp-npc] test: Mario next to %08x (owner %d)\n", best->id, effectiveOwner(*best));
			gpMarioOriginal->warpRequest(to, 0.0f);
		} else if (std::strcmp(action, "spray") == 0) {
			std::fprintf(stderr, "[mp-npc] test: water on %08x type %08x (owner %d%s)\n", best->id,
			             (unsigned)a->getActorType(), effectiveOwner(*best), best->puppet ? ", puppet here" : "");
			auto& w     = TModelWaterManager::mStaticHitActor;
			w.mPosition = gpMarioOriginal->mPosition;
			w.mPosition.y += 80.0f;
			w.unk68 = 0;
			for (int k = 0; k < 8; k++) // a short burst; the forwarding throttles to 10 Hz
				a->receiveMessage(&w, HIT_MESSAGE_SPRAYED_BY_WATER);
		} else if (std::strcmp(action, "stomp") == 0) {
			std::fprintf(stderr, "[mp-npc] test: stomp on %08x type %08x (owner %d%s)\n", best->id,
			             (unsigned)a->getActorType(), effectiveOwner(*best), best->puppet ? ", puppet here" : "");
			a->receiveMessage(gpMarioOriginal, HIT_MESSAGE_TRAMPLE);
		}
	}
}

// SMS_PORT_MP_NPC_WATCH=<type|id hex> (debug): one line per logic frame for that NPC (the first
// of that type): its animation state as drawn here, to compare an owner's log with a puppet's.
void watchLog()
{
	static const char* sWatch = std::getenv("SMS_PORT_MP_NPC_WATCH");
	if (sWatch == nullptr || *sWatch == '\0')
		return;
	const u32 want = parseNpcType(sWatch);
	for (const Npc& o : sNpcs) {
		if (o.a->getActorType() != want && o.id != want)
			continue;
		TBaseNPC* a      = o.a;
		MActor* m        = a->getMActor();
		J3DFrameCtrl* fc = m != nullptr && npcBck(a) >= 0 ? m->getFrameCtrl(ANM_TYPE_BCK) : nullptr;
		int parts[5], partBtp;
		npcParts(a, parts, &partBtp);
		std::fprintf(stderr, "[npc-watch] t=%.0f f=%u %08x owner %d%s bck %d fr %.1f rate %.2f btp %d parts %d %d %d %d %d 1D8 %02x scale %d blend %d pos %.0f %.0f %.0f rotY %.1f\n",
		             port_mp_time_ms(), port_frame_count(), o.id, effectiveOwner(o), o.puppet ? " P" : " L", npcBck(a),
		             fc ? fc->getFrame() : -1.0f, fc ? fc->getRate() : 0.0f, m ? m->getCurAnmIdx(ANM_TYPE_BTP) : -1,
		             parts[0], parts[1], parts[2], parts[3], parts[4], (unsigned)a->unk1D8, npcScaleY(a),
		             a->mInbetweenCtrl != nullptr ? a->mInbetweenCtrl->mMotionBlendTimer : -1, a->mPosition.x,
		             a->mPosition.y, a->mPosition.z, a->mRotation.y);
		break;
	}
}

void gameFrame()
{
	TApplication* app  = SMSGetApplication();
	const int stage    = app->mCurrArea.getStage();
	const int scenario = app->mCurrArea.getScenario();
	PortMpSessionSettings set;
	port_mp_get_settings(&set);
	const bool want     = set.reserved[kResSync] == 1 && stage != 15;
	const bool newVisit = (gpMarDirector != sVisitDir || gPortMpStageSerial != sVisitDirSerial) || stage != sVisitStage || scenario != sVisitScenario;
	if (newVisit) {
		sVisitDir      = gpMarDirector;
		sVisitDirSerial = gPortMpStageSerial;
		sVisitStage    = stage;
		sVisitScenario = scenario;
		sFramesInVisit = 0;
		sActive        = false;
		resetVisit();
	} else {
		sFramesInVisit++;
	}
	if (!want) {
		if (sActive) {
			sActive = false;
			resetVisit();
		}
		netPoll(); // drain
		return;
	}
	// build once the scene is loaded: the game-frame hook already runs while the stage loads
	// (director state < 4), when the scene's managers (and gpConductor) do not exist yet
	if (!sActive && sFramesInVisit >= 2 && gpMarDirector != nullptr && gpMarDirector->mState == 4) {
		buildList();
		sActive = true;
	}
	netPoll();
	if (!sActive)
		return;
	if (leaderSlot() == port_mp_local_slot()) {
		leaderAssign();
		leaderSendOwners();
	}
	ownerSend();
	updatePuppets();
	testKnob();
	watchLog();
	logFrame();
}

struct Register {
	Register() { port_mp_register_game_frame(gameFrame); }
} sRegister;

} // namespace

// TBaseNPC::perform (NpcBase.cpp, SMS_PORT_AURORA), CUE_MOVE: 1 = this NPC is driven by another
// player; its pose and animation were set here and its own movement must be skipped.
extern "C" int port_mp_npc_puppet_move(TBaseNPC* a)
{
	if (!port_mp_active() || (gpMarDirector != sVisitDir || gPortMpStageSerial != sVisitDirSerial)) {
		resetVisit();
		sActive = false;
		return 0;
	}
	auto it = sByActor.find(a);
	if (it == sByActor.end())
		return 0;
	Npc& o = sNpcs[it->second];
	if (!o.puppet || o.ns == 0)
		return 0;
	const double t   = port_mp_time_ms() - 100.0;
	const Sample* s0 = &o.s[0];
	const Sample* s1 = o.ns > 1 ? &o.s[1] : &o.s[0];
	if (o.ns > 2 && t < o.s[1].t) {
		s0 = &o.s[1];
		s1 = &o.s[2];
	}
	float f = 1.0f;
	if (s0 != s1 && s0->t > s1->t)
		f = (float)((t - s1->t) / (s0->t - s1->t));
	f = f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
	a->mPosition.set(s1->pos[0] + (s0->pos[0] - s1->pos[0]) * f, s1->pos[1] + (s0->pos[1] - s1->pos[1]) * f,
	                 s1->pos[2] + (s0->pos[2] - s1->pos[2]) * f);
	// interpolated on the unwrapped samples, applied wrapped to (-180, 180] so the NPC's own
	// code sees a normal angle when it takes over again
	float r[3];
	for (int k = 0; k < 3; k++) {
		r[k] = std::fmod(lerpAngle(s1->rot[k], s0->rot[k], f), 360.0f);
		if (r[k] > 180.0f)
			r[k] -= 360.0f;
		else if (r[k] <= -180.0f)
			r[k] += 360.0f;
	}
	a->mRotation.set(r[0], r[1], r[2]);
	// the owner's animation: switch BCK when it differs, keep the frame close (the local
	// frame control advances at the owner's rate in between)
	MActor* m = a->getMActor();
	if (m != nullptr && o.bck >= 0) {
		if (m->getCurAnmIdx(ANM_TYPE_BCK) != o.bck) {
			// a new animation blends in from the old pose over the NPC's own blend length, as
			// setNpcAnm_ does on the owner (execMotionBlend_ still runs on a puppet)
			m->setBckFromIndex(o.bck);
			if (a->mInbetweenCtrl != nullptr)
				a->onStopMotionBlend();
		}
		J3DFrameCtrl* fc = m->getFrameCtrl(ANM_TYPE_BCK);
		if (fc != nullptr) {
			fc->setRate(o.rate);
			if (std::fabs(fc->getFrame() - o.frame) > 4.0f)
				fc->setFrame(o.frame);
		}
	}
	// face pattern and texture animations (w15): a sprayed or talking NPC's expression
	if (m != nullptr && o.btp > 0 && m->getCurAnmIdx(ANM_TYPE_BTP) != o.btp - 1)
		m->setBtpFromIndex(o.btp - 1);
	if (m != nullptr && o.btk > 0 && m->getCurAnmIdx(ANM_TYPE_BTK) != o.btk - 1)
		m->setBtkFromIndex(o.btk - 1);
	// parts (Peach's hair, ponytail and parasol, the Noki elders' parts): the owner's BCK per
	// part, started at the body's frame as setNpcAnm_ does, then kept near it; their own frame
	// controls advance in partsFrameUpdate
	for (int k = 0; k < 5; k++) {
		MActor* pm = npcPart(a, k);
		if (pm == nullptr || o.parts[k] < -1)
			continue;
		if (pm->getCurAnmIdx(ANM_TYPE_BCK) != o.parts[k])
			pm->setBckFromIndex(o.parts[k]);
		J3DFrameCtrl* pfc = o.parts[k] >= 0 ? pm->getFrameCtrl(ANM_TYPE_BCK) : nullptr;
		if (pfc != nullptr) {
			pfc->setRate(o.rate);
			float f = o.frame;
			if (pfc->getEnd() > 0 && f > (float)pfc->getEnd())
				f = std::fmod(f, (float)pfc->getEnd());
			if (std::fabs(pfc->getFrame() - f) > 4.0f)
				pfc->setFrame(f);
		}
		if (kPartSlots[k] == 10 && o.partBtp > 0 && pm->getCurAnmIdx(ANM_TYPE_BTP) != o.partBtp - 1)
			pm->setBtpFromIndex(o.partBtp - 1);
	}
	// Peach's parasol / tired / recovered state picks the parts drawn (TNpcParts::partsPerform)
	if (o.state1D8 >= 0)
		a->unk1D8 = (u8)o.state1D8;
	// the trample squash
	a->mScaling.y = a->mInitialScale.y * (o.scaleY / 64.0f);
	// goop: the amount and its colour alpha (perform sets the alpha only in the movement pass)
	a->mPollutionAmount = o.poll / 255.0f;
	if (a->isPollutionNpc())
		a->unk174.a = (u8)(a->mPollutionAmount * a->mIndividualParams->mPollutionMax.get());
	return 1;
}

// TBaseNPC::receiveMessage (NpcBase.cpp, SMS_PORT_AURORA): our Mario or our water hit `a`. If
// another player runs it, water and stomps go to that player (its copy reacts and the state
// comes back) and 1 is returned: the local copy, whose AI is skipped, must not react itself
// (its nerve stack would keep the reaction for the day it runs here again). Taking stays
// local (the holder override), as do hits from objects (barrels).
extern "C" int port_mp_npc_puppet_hit(TBaseNPC* a, THitActor* sender, u32 message)
{
	if (sApplyingRemote || !sActive || !port_mp_active() || gpMarDirector != sVisitDir)
		return 0;
	if (message != HIT_MESSAGE_SPRAYED_BY_WATER && message != HIT_MESSAGE_TRAMPLE)
		return 0;
	auto it = sByActor.find(a);
	if (it == sByActor.end())
		return 0;
	Npc& o = sNpcs[it->second];
	if (!o.puppet)
		return 0;
	const bool water = message == HIT_MESSAGE_SPRAYED_BY_WATER;
	const double now = port_mp_time_ms();
	if (water) {
		if (now - o.lastWaterHit < 100.0)
			return 1; // the water stream hits every frame; 10 per second is plenty
		o.lastWaterHit = now;
	}
	u8 buf[sizeof(MsgHeader) + sizeof(HitBody)];
	MsgHeader h;
	h.type     = MSG_HIT;
	h.stage    = (u8)sVisitStage;
	h.scenario = (u8)sVisitScenario;
	h.count    = 1;
	h.seq      = ++sSendSeq;
	HitBody b;
	std::memset(&b, 0, sizeof(b));
	b.id      = o.id;
	b.message = message;
	if (sender != nullptr)
		b.pos[0] = sender->mPosition.x, b.pos[1] = sender->mPosition.y, b.pos[2] = sender->mPosition.z;
	b.water = (u8)(water ? 1 : 0);
	std::memcpy(buf, &h, sizeof(h));
	std::memcpy(buf + sizeof(h), &b, sizeof(b));
	port_mp_send(PORT_MP_CH_NPCS, buf, (int)sizeof(buf), water ? 0 : 1, effectiveOwner(o));
	sStatHitsSent++;
	if (logOn())
		std::fprintf(stderr, "[mp-npc] hit %u on %08x sent to slot %d%s\n", message, o.id, effectiveOwner(o),
		             water ? " (water)" : "");
	return 1;
}

// TBaseNPC::perform (NpcBase.cpp, SMS_PORT_AURORA): 1 = keep this NPC moving although our camera
// does not see it (w15). The game freezes NPCs out of the camera's sight; an NPC we run for
// other players must keep walking, or they watch a frozen pose.
extern "C" int port_mp_npc_keep_moving(TBaseNPC* a)
{
	if (!sActive || !port_mp_active() || gpMarDirector != sVisitDir)
		return 0;
	auto it = sByActor.find(a);
	if (it == sByActor.end() || effectiveOwner(sNpcs[it->second]) != port_mp_local_slot())
		return 0;
	int slots[PORT_MP_MAX_PLAYERS];
	return stagePlayers(slots) > 1 ? 1 : 0;
}

#endif // SMS_PORT_AURORA