// Multiplayer enemy / object sync (w11, task board P8; design: port/MULTIPLAYER.md "Enemy and
// object sync (P8)"). Host setting "Sync enemies: Off (default) / Experimental"
// (PortMpSessionSettings.reserved[2]).
//
// Scope (phase 2): the common TSmallEnemy families (Strollin' Stus and variants, Swoopin'
// Stus, Electrokoopas, Cataquacks, Bloopers, ...), moving map objects with map collision
// (lifts, rail blocks, rotating platforms), and the bosses Petey Piranha and Gooper Blooper.
//  - Enemies and platforms have one owner each, the player nearest to them (with hysteresis),
//    chosen by the stage leader (the lowest slot among the players in that stage + scenario).
//    The owner runs the object and sends pose, alive, HP and the current nerve; the others
//    show a puppet: TLiveActor::perform skips its movement (hook in liveactor.cpp) and takes
//    the interpolated pose. When ownership moves, the new owner continues from the last state
//    it received, in the owner's nerve with its HP. Stomps, hip drops, punches and water from
//    a non-owner are forwarded to the owner (hooks where Mario and his water hit actors).
//  - Bosses are owned the same way (the nearest player is the one fighting). The others run
//    the boss's AI locally too (animations, tentacles, attacks against their own Mario) but
//    follow the owner while their own copy is in its fight: its nerve is forced on every
//    change, HP and pose are taken over. A boss's own damage functions forward the damage to
//    the owner instead of applying it (hooks in bossgesso.cpp / bosspakkun.cpp).
//  - Ids are (manager index in TConductor's list, object index in that manager): scene objects
//    are created in file order on every client, as P1 uses for coins. Nerves travel as their
//    offset in the exe image, used only between peers with the same exe (build id).
// Nothing runs unless a session is active and the host turned the setting on; then only the
// objects above are touched. Single player never reaches this file (gPortMpObjPuppets == 0).
#ifdef SMS_PORT_AURORA

#include "port_mp.h"
#include <port_mp_obj.h>

#include <Enemy/BossGesso.hpp>
#include <Enemy/BossPakkun.hpp>
#include <Enemy/Conductor.hpp>
#include <Enemy/SmallEnemy.hpp>
#include <MoveBG/ItemManager.hpp>
#include <Player/Mario.hpp>
#include <Player/MarioAccess.hpp>
#include <Player/ModelWaterManager.hpp>
#include <Strategic/ActorTypes.hpp>
#include <Strategic/LiveActor.hpp>
#include <Strategic/LiveManager.hpp>
#include <Strategic/Spine.hpp>
#include <System/Application.hpp>
#include <System/MarDirector.hpp>
#include <JSystem/JDrama/JDRNameRefGen.hpp>

#define IMGUI_USER_CONFIG "aurora/imgui_config.h" // as Aurora builds imgui
#include <imgui.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

extern "C" unsigned port_frame_count(void);      // port_crash.cpp
extern "C" void port_mp_toast(const char* text); // port_mp_menu.cpp
extern "C" int port_mp_mapobj_claims(void);   // port_mp_mapobj.cpp (w7)

// Read by the hooks (port_mp_obj.h): number of actors currently driven by another player.
extern "C" int gPortMpObjPuppets = 0;

namespace {

const int kResSync = 2; // PortMpSessionSettings.reserved[2]: 0 = off (default), 1 = experimental

typedef TSpineBase<TLiveActor> Spine;
typedef const TNerveBase<TLiveActor>* Nerve;

// ---- wire (PORT_MP_CH_OBJECTS, packed, little-endian) ----------------------------------------
enum MsgType : u8 { MSG_STATE = 1, MSG_OWNERS = 2, MSG_HIT = 3, MSG_BOSS_DAMAGE = 4 };
#pragma pack(push, 1)
struct MsgHeader {
	u8 type;
	u8 stage, scenario;
	u8 count;
	u32 seq;   // STATE: sender's send counter; OWNERS: leader's table version
	u32 build; // sender's exe (nerve offsets are only used between equal builds)
};
struct StateEntry {
	u32 id;
	u8 flags; // bit 0 alive, bit 1 nerve valid
	u8 hp;    // TSpineEnemy::mHitPoints
	s16 pad;
	float pos[3];
	s16 rot[3]; // degrees * 65536 / 360
	s16 pad2;
	u32 nerve; // current nerve, offset in the exe image (0 = none)
};
struct OwnerEntry {
	u32 id;
	u8 slot;
	u8 pad[3];
};
struct HitBody { // MSG_HIT, to the owner: our Mario / water hit its object
	u32 id;
	u32 message;  // HIT_MESSAGE_*
	float pos[3]; // attacker (Mario, or the water particle)
	u8 water;
	u8 pad[3];
};
struct BossDamageBody { // MSG_BOSS_DAMAGE, to the leader
	u32 id;
	u8 kind; // PORT_MP_BOSS_*
	u8 pad[3];
};
#pragma pack(pop)
const int kMaxState  = (PORT_MP_APP_MAX - (int)sizeof(MsgHeader)) / (int)sizeof(StateEntry);
const int kMaxOwners = (PORT_MP_APP_MAX - (int)sizeof(MsgHeader)) / (int)sizeof(OwnerEntry);

enum Kind : u8 { KIND_ENEMY = 1, KIND_PLATFORM = 2, KIND_BOSS = 3, KIND_FOLLOW = 4 };

struct Sample {
	double t; // receive time (port_mp_time_ms)
	float pos[3];
	float rot[3];
};

struct Obj {
	TLiveActor* a;
	u32 id;
	u8 kind;
	int owner;          // slot; -1 = the stage leader
	int pendingOwner;   // while an owner table arrives
	double ownerSince;
	// owner side
	float lastPos[3], lastRot[3];
	bool lastAlive;
	u32 lastNerve;
	u8 lastHp;
	bool lastHidden;
	double movedAt;     // last time it moved here (platforms are sent while they move)
	bool everMoved;
	// receiving side
	Sample s[3];        // newest first
	int ns;
	bool remoteAlive;
	bool remoteHidden; // the owner's copy is hidden (goop Piranha in the goop, invisible Boo)
	bool haveRemoteState; // a state of this object arrived in this visit (nerve/HP below)
	Nerve remoteNerve;  // the owner's nerve (null: unknown / other build)
	u8 remoteHp;
	Nerve appliedNerve; // bosses: the leader's nerve last forced here
	bool puppet;        // enemies/platforms: driven by the network; bosses: following the leader
	bool dyingLocal;    // the owner reported a death: our copy plays its own death
	double lastWaterHit;
};

std::vector<Obj> sObjs;
std::unordered_map<u32, int> sById;
std::unordered_map<TLiveActor*, int> sByActor;
TMarDirector* sVisitDir;
unsigned sVisitDirSerial; // gPortMpStageSerial of this visit
int sVisitStage = -1, sVisitScenario = -1;
int sFramesInVisit;
bool sActive;           // enabled for this visit
u32 sSendSeq, sOwnerSeq, sOwnerSeen;
double sLastSent, sLastKeyframe, sLastOwnerCalc, sLastOwnerSent;
bool sOwnersDirty;
int sLeaderSeen = -1;
bool sApplyingRemote;   // a forwarded hit / boss damage is being applied: do not forward again

// exe image (nerve offsets) and build id
uintptr_t sImageBase;
u32 sImageSize, sBuildId;

// stats for the panel / log
int sStatOwned, sStatPuppets, sStatSentEntries, sStatRxEntries, sStatHitsSent, sStatHitsApplied;

bool logOn()
{
	static const bool sLog = std::getenv("SMS_PORT_MP_OBJ_LOG") != nullptr;
	return sLog;
}

void initImage()
{
	if (sImageBase != 0)
		return;
	HMODULE h  = GetModuleHandleW(nullptr);
	sImageBase = (uintptr_t)h;
	const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)h;
	const IMAGE_NT_HEADERS* nt  = (const IMAGE_NT_HEADERS*)(sImageBase + dos->e_lfanew);
	sImageSize = nt->OptionalHeader.SizeOfImage;
	sBuildId   = nt->FileHeader.TimeDateStamp ^ (sImageSize * 2654435761u) ^ 0x50380001u;
}

inline bool inImage(const void* p)
{
	const uintptr_t v = (uintptr_t)p;
	return v >= sImageBase && v < sImageBase + sImageSize;
}

u32 nerveToRva(Nerve n) { return n != nullptr && inImage(n) ? (u32)((uintptr_t)n - sImageBase) : 0; }

// A nerve offset from a peer: only from the same exe, and only if it points at an object with
// a vtable in our image (the nerve singletons are static objects of the exe).
Nerve rvaToNerve(u32 rva, u32 build)
{
	if (rva == 0 || build != sBuildId || rva >= sImageSize)
		return nullptr;
	const void* p = (const void*)(sImageBase + rva);
	if (((uintptr_t)p & (sizeof(void*) - 1)) != 0 || !inImage(*(void* const*)p))
		return nullptr;
	return (Nerve)p;
}

// Enemies synced in phase 2: TSmallEnemy families common across stages (all derive from
// TSmallEnemy: one AI driver, the die nerve on kill(), reset() on respawn).
// Left out on purpose: Popo (Petey Piranha's wake-up trigger). Goop Piranha Plants and Boos hide
// with LIVE_FLAG_HIDDEN, which the state carries; Pokeys and Wigglers run in follow mode
// (isFollowEnemy).
bool isSyncedEnemy(const TLiveActor* a)
{
	switch (a->getActorType()) {
	case ACTOR_TYPE_HAMU_KURI:      // Strollin' Stu
	case ACTOR_TYPE_HANE_HAMU_KURI: // Swoopin' Stu
	case ACTOR_TYPE_DANGO_HAMU_KURI:
	case ACTOR_TYPE_FIRE_HAMU_KURI:
	case ACTOR_TYPE_DORO_HAMU_KURI:
	case ACTOR_TYPE_DORO_HANE_KURI:
	case ACTOR_TYPE_NAME_KURI:
	case ACTOR_TYPE_ELEC_NOKONOKO: // Electrokoopa
	case ACTOR_TYPE_POI_HANA:      // Cataquack
	case ACTOR_TYPE_GESSO:         // Blooper (walking)
	case ACTOR_TYPE_MAME_GESSO:
	case ACTOR_TYPE_TAMA_NOKO:
	case ACTOR_TYPE_BOMB_HEI:
	case ACTOR_TYPE_IGAIGA:  // rolling enemies
	case ACTOR_TYPE_GOROGORO:
	case ACTOR_TYPE_PAKKUN: // goop Piranha Plant (in the goop: LIVE_FLAG_HIDDEN, mirrored)
	case ACTOR_TYPE_TELESA: // Boo (invisible: LIVE_FLAG_HIDDEN, mirrored)
		return true;
	default:
		return false;
	}
}

// Enemies whose own AI moves the parts of their body (Pokey's segments, Wiggler's body):
// skipping the AI would freeze the parts, so the others run them and follow the owner like
// bosses (nerve changes, HP, pose), plus the owner's deaths and respawns.
bool isFollowEnemy(const TLiveActor* a)
{
	// Pokey: the managed object is its head (TSamboHead), which drives the body segments
	return a->getActorType() == ACTOR_TYPE_SAMBO_HEAD || a->getActorType() == ACTOR_TYPE_HANA_SAMBO
	       || a->getActorType() == ACTOR_TYPE_CHUU_HANA;
}

bool isBoss(const TLiveActor* a)
{
	return a->getActorType() == ACTOR_TYPE_BOSS_PAKKUN || a->getActorType() == ACTOR_TYPE_BOSS_GESSO;
}

// A boss whose fight runs on this client. Petey Piranha's manager flag marks a copy that only
// stands there (not this client's fight yet); his damage handlers and fight nerves are not
// meant to run on it (TBossPakkun::receiveMessage refuses hits then).
bool bossActive(const TLiveActor* a)
{
	if (a->getActorType() == ACTOR_TYPE_BOSS_PAKKUN)
		return a->mManager != nullptr && static_cast<TBossPakkunManager*>(a->mManager)->unk54 == 0;
	return true;
}

bool isPlatform(const TLiveManager* m, const TLiveActor* a)
{
	return (TObjManager*)m != (TObjManager*)gpItemManager && a->getHitCategory() == HIT_CATEGORY_MAP_OBJECT
	       && a->mMapCollisionManager != nullptr;
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

bool alive(const TLiveActor* a) { return (a->mLiveFlag & LIVE_FLAG_DEAD) == 0; }

Nerve curNerve(const TLiveActor* a) { return a->mSpine != nullptr ? a->mSpine->getLatestNerve() : nullptr; }

u8 hpOf(const Obj& o) { return o.kind == KIND_PLATFORM ? 0 : ((TSpineEnemy*)o.a)->mHitPoints; }

// The game's own way to switch an enemy to a nerve (as forceKill() and the damage functions
// do): clear the stack, run `n` next, then the default nerve.
void forceNerve(TLiveActor* a, Nerve n)
{
	Spine* s = a->mSpine;
	if (s == nullptr || n == nullptr || s->getLatestNerve() == n)
		return;
	s->reset();
	s->setNext(n);
	s->pushAfterCurrent(s->getDefault());
}

// Players in our stage + scenario (including us).
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

int effectiveOwner(const Obj& o) { return o.owner >= 0 ? o.owner : leaderSlot(); }

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

void releaseAll()
{
	for (Obj& o : sObjs)
		o.puppet = false;
	gPortMpObjPuppets = 0;
}

void resetVisit()
{
	releaseAll();
	sObjs.clear();
	sById.clear();
	sByActor.clear();
	sSendSeq = sOwnerSeq = sOwnerSeen = 0;
	sLastSent = sLastKeyframe = sLastOwnerCalc = sLastOwnerSent = 0.0;
	sOwnersDirty = false;
	sLeaderSeen  = -1;
}

void header(MsgHeader& h, u8 type, u8 count, u32 seq)
{
	h.type     = type;
	h.stage    = (u8)sVisitStage;
	h.scenario = (u8)sVisitScenario;
	h.count    = count;
	h.seq      = seq;
	h.build    = sBuildId;
}

// The synced objects of this stage visit, in TConductor's manager order.
void buildList()
{
	resetVisit();
	// gpConductor is only set when a scene creates one and never cleared: a stage without a
	// conductor still points at the previous stage's (freed) one. Use it only if this
	// scene's name tree has it (TConductor's name "konDAKUTA-" in katakana, Shift-JIS as the
	// game's strings are).
	JDrama::TNameRefGen* gen = JDrama::TNameRefGen::getInstance();
	JDrama::TNameRef* found  = nullptr;
	if (gpConductor != nullptr && gen != nullptr && gen->getRootNameRef() != nullptr)
		found = gen->getRootNameRef()->search("\x83\x52\x83\x93\x83\x5F\x83\x4E\x83\x5E\x81\x5B");
	if (gpConductor == nullptr || found != (JDrama::TNameRef*)gpConductor) {
		std::fprintf(stderr, "[mp-obj] stage %d/%d: no conductor, nothing to sync\n", sVisitStage, sVisitScenario);
		return;
	}
	static const bool sDump = std::getenv("SMS_PORT_MP_OBJ_DUMP") != nullptr;
	int mgrIdx = 0, enemies = 0, bosses = 0;
	for (JGadget::TList<TLiveManager*>::iterator it = gpConductor->unk10.begin(), e = gpConductor->unk10.end(); it != e;
	     ++it, ++mgrIdx) {
		TLiveManager* m = *it;
		if (m == nullptr)
			continue;
		int en = 0, pl = 0, bo = 0;
		for (int i = 0; i < m->getObjNum(); i++) {
			TLiveActor* a = m->getObj(i);
			if (a == nullptr)
				continue;
			u8 kind = 0;
			if (isSyncedEnemy(a))
				kind = KIND_ENEMY, en++;
			else if (isBoss(a))
				kind = KIND_BOSS, bo++;
			else if (isFollowEnemy(a))
				kind = KIND_FOLLOW, en++;
			else if (isPlatform(m, a))
				kind = KIND_PLATFORM, pl++;
			if (kind == 0)
				continue;
			Obj o;
			std::memset(&o, 0, sizeof(o));
			o.a            = a;
			o.id           = ((u32)mgrIdx << 16) | (u32)i;
			o.kind         = kind;
			o.owner        = -1;
			o.pendingOwner = -1;
			o.lastAlive    = alive(a);
			o.lastPos[0] = a->mPosition.x, o.lastPos[1] = a->mPosition.y, o.lastPos[2] = a->mPosition.z;
			o.lastRot[0] = a->mRotation.x, o.lastRot[1] = a->mRotation.y, o.lastRot[2] = a->mRotation.z;
			sById[o.id] = (int)sObjs.size();
			sByActor[a] = (int)sObjs.size();
			sObjs.push_back(o);
		}
		enemies += en;
		bosses += bo;
		if (sDump)
			std::fprintf(stderr, "[mp-obj] manager %d \"%s\": %d objects, %d enemies, %d bosses, %d platforms\n", mgrIdx,
			             m->getName() ? m->getName() : "?", m->getObjNum(), en, bo, pl);
	}
	std::fprintf(stderr, "[mp-obj] stage %d/%d: %d synced objects (%d enemies, %d bosses)\n", sVisitStage,
	             sVisitScenario, (int)sObjs.size(), enemies, bosses);
}

// ---- receiving -------------------------------------------------------------------------------
void onState(int from, const MsgHeader& h, const StateEntry* e, int n)
{
	const double now = port_mp_time_ms();
	for (int i = 0; i < n; i++) {
		auto it = sById.find(e[i].id);
		if (it == sById.end())
			continue;
		Obj& o = sObjs[it->second];
		if (effectiveOwner(o) != from)
			continue; // not (or no longer) its owner: stale
		sStatRxEntries++;
		Sample s;
		s.t = now;
		std::memcpy(s.pos, e[i].pos, sizeof(s.pos));
		// angles travel wrapped; unwrap them next to the previous value, so an object whose
		// own code counts full turns keeps a continuous angle
		const float cur[3] = { o.a->mRotation.x, o.a->mRotation.y, o.a->mRotation.z };
		for (int k = 0; k < 3; k++) {
			const float prev = o.ns > 0 ? o.s[0].rot[k] : cur[k];
			s.rot[k]         = lerpAngle(prev, decAngle(e[i].rot[k]), 1.0f);
		}
		o.s[2] = o.s[1];
		o.s[1] = o.s[0];
		o.s[0] = s;
		if (o.ns < 3)
			o.ns++;
		o.remoteAlive     = (e[i].flags & 1) != 0;
		o.remoteHidden    = (e[i].flags & 4) != 0;
		o.haveRemoteState = true;
		o.remoteHp        = e[i].hp;
		o.remoteNerve     = (e[i].flags & 2) ? rvaToNerve(e[i].nerve, h.build) : nullptr;
	}
}

void onOwners(int from, const MsgHeader& h, const OwnerEntry* e, int n, bool first)
{
	if (from != leaderSlot())
		return; // only the current leader assigns
	if (first) {
		if (h.seq < sOwnerSeen && from == sLeaderSeen)
			return; // older table
		sOwnerSeen  = h.seq;
		sLeaderSeen = from;
		for (Obj& o : sObjs)
			o.pendingOwner = -1; // a table lists the exceptions to "owned by the leader"
	}
	for (int i = 0; i < n; i++) {
		auto it = sById.find(e[i].id);
		if (it != sById.end())
			sObjs[it->second].pendingOwner = e[i].slot;
	}
	for (Obj& o : sObjs) {
		if (o.owner != o.pendingOwner) {
			o.ownerSince = port_mp_time_ms();
			o.ns         = 0; // samples of the previous owner
		}
		o.owner = o.pendingOwner;
	}
}

// The owner applies a hit that another player's Mario or water landed on its object.
void onHit(int from, const HitBody& b)
{
	auto it = sById.find(b.id);
	if (it == sById.end())
		return;
	Obj& o = sObjs[it->second];
	if (effectiveOwner(o) != port_mp_local_slot() || !alive(o.a) || o.kind != KIND_ENEMY)
		return;
	sApplyingRemote = true;
	if (b.water) {
		auto& w = TModelWaterManager::mStaticHitActor;
		w.mPosition.set(b.pos[0], b.pos[1], b.pos[2]);
		w.unk68 = 0;
		o.a->receiveMessage(&w, HIT_MESSAGE_SPRAYED_BY_WATER);
	} else if (gpMarioOriginal != nullptr) {
		// The game's reactions read the attacker's position (knock-back direction): our Mario
		// stands in for the other player's for the length of the call.
		TMario* m                         = gpMarioOriginal;
		const JGeometry::TVec3<f32> saved = m->mPosition;
		m->mPosition.set(b.pos[0], b.pos[1], b.pos[2]);
		o.a->receiveMessage(m, b.message);
		m->mPosition = saved;
	}
	sApplyingRemote = false;
	sStatHitsApplied++;
	if (logOn())
		std::fprintf(stderr, "[mp-obj] hit %u on %08x from slot %d applied%s\n", b.message, b.id, from,
		             b.water ? " (water)" : "");
}

void onBossDamage(int from, const BossDamageBody& b)
{
	auto it = sById.find(b.id);
	if (it == sById.end())
		return;
	Obj& o = sObjs[it->second];
	if (o.kind != KIND_BOSS || effectiveOwner(o) != port_mp_local_slot() || !alive(o.a) || !bossActive(o.a))
		return;
	sApplyingRemote = true;
	switch (b.kind) {
	case PORT_MP_BOSS_GESSO_TENTACLE: ((TBossGesso*)o.a)->gotTentacleDamage(); break;
	case PORT_MP_BOSS_GESSO_EYE: ((TBossGesso*)o.a)->gotEyeDamage(); break;
	case PORT_MP_BOSS_GESSO_BEAK: ((TBossGesso*)o.a)->gotBeakDamage(); break;
	case PORT_MP_BOSS_PAKKUN_HIPDROP: ((TBossPakkun*)o.a)->gotHipDropDamage(); break;
	case PORT_MP_BOSS_PAKKUN_WATER: ((TBossPakkun*)o.a)->gotWaterDamage(); break;
	case PORT_MP_BOSS_PAKKUN_FLYING: ((TBossPakkun*)o.a)->gotFlyingDamage(); break;
	default: break;
	}
	sApplyingRemote = false;
	sStatHitsApplied++;
	std::fprintf(stderr, "[mp-obj] boss damage %d on %08x from slot %d applied (HP now %d)\n", b.kind, b.id, from,
	             (int)((TSpineEnemy*)o.a)->mHitPoints);
}

void netPoll()
{
	u8 buf[PORT_MP_APP_MAX];
	int from, n;
	while ((n = port_mp_recv(PORT_MP_CH_OBJECTS, buf, sizeof(buf), &from)) != 0) {
		if (!sActive || n < (int)sizeof(MsgHeader) || from < 0 || from >= PORT_MP_MAX_PLAYERS)
			continue;
		MsgHeader h;
		std::memcpy(&h, buf, sizeof(h));
		if (h.stage != sVisitStage || h.scenario != sVisitScenario)
			continue;
		const u8* body = buf + sizeof(h);
		const int len  = n - (int)sizeof(h);
		if (h.type == MSG_STATE && len == h.count * (int)sizeof(StateEntry)) {
			onState(from, h, (const StateEntry*)body, h.count);
		} else if ((h.type == MSG_OWNERS || h.type == (MSG_OWNERS | 0x80)) && len == h.count * (int)sizeof(OwnerEntry)) {
			// 0x80: continuation of the same table
			onOwners(from, h, (const OwnerEntry*)body, h.count, h.type == MSG_OWNERS);
		} else if (h.type == MSG_HIT && len == (int)sizeof(HitBody)) {
			HitBody b;
			std::memcpy(&b, body, sizeof(b));
			onHit(from, b);
		} else if (h.type == MSG_BOSS_DAMAGE && len == (int)sizeof(BossDamageBody)) {
			BossDamageBody b;
			std::memcpy(&b, body, sizeof(b));
			onBossDamage(from, b);
		}
	}
}

// ---- leader: who owns what -------------------------------------------------------------------
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
	for (Obj& o : sObjs) {

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
		// hysteresis: the new player must be clearly nearer, and not right after a change
		const bool ownerGone = cur < 0;
		const float bd = std::sqrt(bestD), cd = std::sqrt(curD);
		if (ownerGone || (bd < cd * 0.7f - 100.0f && now - o.ownerSince > 1000.0)) {
			o.owner      = best == me ? -1 : best;
			o.ownerSince = now;
			o.ns         = 0;
			sOwnersDirty = true;
		}
	}
	// players who left the stage give their objects back
	for (Obj& o : sObjs)
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
	const double now = port_mp_time_ms();
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
		header(h, chunk == 0 ? MSG_OWNERS : (MSG_OWNERS | 0x80), (u8)n, sOwnerSeq);
		std::memcpy(buf, &h, sizeof(h));
		port_mp_send(PORT_MP_CH_OBJECTS, buf, (int)sizeof(h) + n * (int)sizeof(OwnerEntry), reliable ? 1 : 0, -1);
		n = 0;
		chunk++;
	};
	for (const Obj& o : sObjs) {
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

// ---- owner: send what we run -----------------------------------------------------------------
void ownerSend()
{
	const double now = port_mp_time_ms();
	if (now - sLastSent < 66.0) // ~15 Hz
		return;
	sLastSent           = now;
	const bool keyframe = now - sLastKeyframe > 1000.0;
	if (keyframe)
		sLastKeyframe = now;
	const int me = port_mp_local_slot();
	u8 buf[PORT_MP_APP_MAX];
	int n = 0;
	auto flush = [&]() {
		if (n == 0)
			return;
		MsgHeader h;
		header(h, MSG_STATE, (u8)n, ++sSendSeq);
		std::memcpy(buf, &h, sizeof(h));
		port_mp_send(PORT_MP_CH_OBJECTS, buf, (int)sizeof(h) + n * (int)sizeof(StateEntry), 0, -1);
		sStatSentEntries += n;
		n = 0;
	};
	sStatOwned = 0;
	for (Obj& o : sObjs) {
		if (effectiveOwner(o) != me)
			continue;
		sStatOwned++;
		TLiveActor* a = o.a;
		const bool al = alive(a);
		const float p[3] = { a->mPosition.x, a->mPosition.y, a->mPosition.z };
		const float r[3] = { a->mRotation.x, a->mRotation.y, a->mRotation.z };
		const u32 nerve  = o.kind == KIND_PLATFORM ? 0 : nerveToRva(curNerve(a));
		const u8 hp      = hpOf(o);
		bool moved       = false;
		for (int k = 0; k < 3; k++)
			moved |= std::fabs(p[k] - o.lastPos[k]) > 0.5f || std::fabs(r[k] - o.lastRot[k]) > 0.3f;
		if (moved) {
			o.movedAt   = now;
			o.everMoved = true;
		}
		const bool hid     = (a->mLiveFlag & LIVE_FLAG_HIDDEN) != 0;
		const bool changed = moved || al != o.lastAlive || nerve != o.lastNerve || hp != o.lastHp || hid != o.lastHidden;
		// enemies/bosses: on change and in every keyframe; platforms: only once they have moved
		const bool send = o.kind != KIND_PLATFORM ? (changed || keyframe)
		                                          : (o.everMoved && (changed || keyframe || now - o.movedAt < 500.0));
		std::memcpy(o.lastPos, p, sizeof(p));
		std::memcpy(o.lastRot, r, sizeof(r));
		o.lastAlive = al;
		o.lastNerve = nerve;
		o.lastHp    = hp;
		o.lastHidden = hid;
		if (!send)
			continue;
		StateEntry e;
		std::memset(&e, 0, sizeof(e));
		e.id    = o.id;
		e.flags = (u8)((al ? 1 : 0) | (nerve != 0 ? 2 : 0) | (hid ? 4 : 0));
		e.hp    = hp;
		std::memcpy(e.pos, p, sizeof(p));
		for (int k = 0; k < 3; k++)
			e.rot[k] = encAngle(r[k]);
		e.nerve = nerve;
		std::memcpy(buf + sizeof(MsgHeader) + n * sizeof(StateEntry), &e, sizeof(e));
		if (++n == kMaxState)
			flush();
	}
	flush();
}

// ---- puppets ---------------------------------------------------------------------------------
// We just became the owner of an object we showed as a puppet: continue from the owner's last
// state (pose is already the interpolated one), in its nerve with its HP.
void takeOver(Obj& o)
{
	if (!o.haveRemoteState || !alive(o.a) || o.kind == KIND_PLATFORM)
		return;
	((TSpineEnemy*)o.a)->mHitPoints = o.remoteHp;
	if (o.remoteNerve != nullptr)
		forceNerve(o.a, o.remoteNerve);
	if (logOn())
		std::fprintf(stderr, "[mp-obj] %08x taken over (HP %d, nerve %s)\n", o.id, (int)o.remoteHp,
		             o.remoteNerve ? "set" : "kept");
}

void updatePuppets()
{
	const double now = port_mp_time_ms();
	const int me     = port_mp_local_slot();
	int puppets      = 0;
	for (Obj& o : sObjs) {
		const bool mine   = effectiveOwner(o) == me;
		const bool remote = !mine && o.ns > 0 && now - o.s[0].t < 2000.0;
		TLiveActor* a     = o.a;
		if (!remote) {
			if (o.puppet) {
				if (mine)
					takeOver(o);
				if (logOn())
					std::fprintf(stderr, "[mp-obj] %08x runs here now\n", o.id);
			}
			o.puppet       = false;
			o.dyingLocal   = false;
			o.appliedNerve = nullptr;
			continue;
		}
		if (o.kind == KIND_BOSS) {
			// follows its owner: AI runs here; nerve and HP are taken over here (some bosses skip
			// TLiveActor::perform in parts of their fight), the pose in the perform hook
			if (!o.puppet && logOn())
				std::fprintf(stderr, "[mp-obj] boss %08x follows slot %d\n", o.id, effectiveOwner(o));
			o.puppet = alive(a);
			puppets += o.puppet;
			if (o.puppet && bossActive(a)) {
				if (o.remoteNerve != nullptr && o.remoteNerve != o.appliedNerve) {
					forceNerve(a, o.remoteNerve);
					o.appliedNerve = o.remoteNerve;
				}
				((TSpineEnemy*)a)->mHitPoints = o.remoteHp;
			}
			continue;
		}
		const bool al = alive(a);
		if (!o.remoteAlive) {
			// the owner's copy died (or is not out): ours plays its own death, then stays dead
			if (al && !o.dyingLocal) {
				a->kill(); // TSmallEnemy: the die nerve; others: dead at once
				o.dyingLocal = true;
				if (logOn())
					std::fprintf(stderr, "[mp-obj] %08x died on its owner (slot %d)\n", o.id, effectiveOwner(o));
			}
			o.puppet = false; // let the death run locally
			continue;
		}
		o.dyingLocal = false;
		if (!al) {
			// respawned on its owner
			if (o.kind == KIND_ENEMY || o.kind == KIND_FOLLOW)
				((TSmallEnemy*)a)->reset();
			else
				a->mLiveFlag &= ~(LIVE_FLAG_DEAD | LIVE_FLAG_UNK40);
			a->mPosition.set(o.s[0].pos[0], o.s[0].pos[1], o.s[0].pos[2]);
			if (logOn())
				std::fprintf(stderr, "[mp-obj] %08x respawned on its owner\n", o.id);
		}
		if (!o.puppet && logOn())
			std::fprintf(stderr, "[mp-obj] %08x now %s slot %d\n", o.id,
			             o.kind == KIND_FOLLOW ? "follows" : "driven by", effectiveOwner(o));
		o.puppet = true;
		puppets++;
		if (o.kind == KIND_FOLLOW) {
			// multi-part enemy: AI runs here, nerve changes and HP from the owner (pose: hook)
			if (o.remoteNerve != nullptr && o.remoteNerve != o.appliedNerve) {
				forceNerve(a, o.remoteNerve);
				o.appliedNerve = o.remoteNerve;
			}
			((TSpineEnemy*)a)->mHitPoints = o.remoteHp;
		} else if (o.kind == KIND_ENEMY) {
			// hidden on the owner (goop Piranha in the goop, invisible Boo): the puppet runs no AI
			// of its own, so it takes the flag
			if (o.remoteHidden)
				a->mLiveFlag |= LIVE_FLAG_HIDDEN;
			else
				a->mLiveFlag &= ~LIVE_FLAG_HIDDEN;
		}
	}
	sStatPuppets      = puppets;
	gPortMpObjPuppets = puppets;
}

// SMS_PORT_MP_OBJ_TEST="<name>:<action>@<frame>;..." (debug, with mp_test.py's names):
//   kill  = kill the nearest synced enemy we own
//   near  = put Mario next to the first synced enemy (ownership moves to us)
//   stomp / spray = send a stomp / water hit from our Mario to the nearest enemy someone
//           else runs, through the same path as a real hit (port_mp_obj_hit + the local
//           receiveMessage)
//   boss  = damage the stage's boss through its own damage function (Gooper: beak, Petey:
//           hip drop: both take 1 HP), on whichever client runs this
void testKnob()
{
	static const char* sTest = std::getenv("SMS_PORT_MP_OBJ_TEST");
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
		const char* action = colon + 1;
		const int mySlot   = port_mp_local_slot();
		if (std::strcmp(action, "boss") == 0) {
			for (Obj& o : sObjs)
				if (o.kind == KIND_BOSS && alive(o.a)) {
					if (!bossActive(o.a)) {
						std::fprintf(stderr, "[mp-obj] test: boss %08x is not in its fight here\n", o.id);
						break;
					}
					std::fprintf(stderr, "[mp-obj] test: damaging boss %08x (HP %d)\n", o.id,
					             (int)((TSpineEnemy*)o.a)->mHitPoints);
					if (o.a->getActorType() == ACTOR_TYPE_BOSS_GESSO)
						((TBossGesso*)o.a)->gotBeakDamage();
					else
						((TBossPakkun*)o.a)->gotHipDropDamage();
					break;
				}
			continue;
		}
		const JGeometry::TVec3<f32>& mp = gpMarioOriginal->mPosition;
		Obj* best   = nullptr;
		float bestD = 1e30f;
		const bool wantRemote = std::strcmp(action, "stomp") == 0 || std::strcmp(action, "spray") == 0;
		for (Obj& o : sObjs) {
			if (o.kind != KIND_ENEMY || !alive(o.a))
				continue;
			if (std::strcmp(action, "kill") == 0 && effectiveOwner(o) != mySlot)
				continue;
			if (wantRemote && !o.puppet)
				continue;
			const float dx = o.a->mPosition.x - mp.x, dz = o.a->mPosition.z - mp.z;
			const float d  = dx * dx + dz * dz;
			if (std::strcmp(action, "near") == 0 ? best == nullptr : d < bestD)
				best = &o, bestD = d;
		}
		if (best == nullptr) {
			std::fprintf(stderr, "[mp-obj] test %s: no enemy\n", action);
			continue;
		}
		if (std::strcmp(action, "kill") == 0) {
			std::fprintf(stderr, "[mp-obj] test: killing %08x\n", best->id);
			best->a->kill();
		} else if (std::strcmp(action, "near") == 0) {
			JGeometry::TVec3<f32> to(best->a->mPosition.x + 250.0f, best->a->mPosition.y + 50.0f, best->a->mPosition.z);
			std::fprintf(stderr, "[mp-obj] test: Mario next to %08x\n", best->id);
			gpMarioOriginal->warpRequest(to, 0.0f);
		} else if (std::strcmp(action, "stomp") == 0) {
			std::fprintf(stderr, "[mp-obj] test: stomp on %08x (owner %d)\n", best->id, effectiveOwner(*best));
			PORT_MP_OBJ_HIT(best->a, &gpMarioOriginal->mPosition.x, HIT_MESSAGE_TRAMPLE, 0);
			best->a->receiveMessage(gpMarioOriginal, HIT_MESSAGE_TRAMPLE);
		} else if (std::strcmp(action, "spray") == 0) {
			std::fprintf(stderr, "[mp-obj] test: water on %08x (owner %d)\n", best->id, effectiveOwner(*best));
			auto& w = TModelWaterManager::mStaticHitActor;
			w.mPosition  = gpMarioOriginal->mPosition;
			w.unk68      = 0;
			for (int k = 0; k < 8; k++) // a short burst; the owner side throttles to 10 Hz
				PORT_MP_OBJ_HIT(best->a, &w.mPosition.x, HIT_MESSAGE_SPRAYED_BY_WATER, 1);
		}
	}
}

void logFrame()
{
	if (!logOn())
		return;
	static double sAt;
	if (port_mp_time_ms() - sAt < 5000.0)
		return;
	sAt = port_mp_time_ms();
	int alive = 0, total = 0;
	for (const Obj& o : sObjs)
		if (o.kind == KIND_ENEMY) {
			total++;
			alive += ::alive(o.a);
		}
	std::fprintf(stderr,
	             "[mp-obj] stage %d/%d leader %d: %d objects (%d/%d enemies alive), owned %d, puppets %d, sent %d, rx "
	             "%d, hits sent %d applied %d\n",
	             sVisitStage, sVisitScenario, leaderSlot(), (int)sObjs.size(), alive, total, sStatOwned, sStatPuppets,
	             sStatSentEntries, sStatRxEntries, sStatHitsSent, sStatHitsApplied);
	int shown = 0;
	for (const Obj& o : sObjs)
		if (o.kind == KIND_BOSS || (o.kind == KIND_ENEMY && shown < 4)) {
			shown += o.kind == KIND_ENEMY;
			std::fprintf(stderr, "[mp-obj]   %s %08x type %08x owner %d %s HP %d nerve %08x pos %.0f %.0f %.0f%s\n",
			             o.kind == KIND_BOSS ? "boss" : "enemy", o.id, (unsigned)o.a->getActorType(), effectiveOwner(o),
			             ::alive(o.a) ? "alive" : "dead", (int)hpOf(o), nerveToRva(curNerve(o.a)), o.a->mPosition.x,
			             o.a->mPosition.y, o.a->mPosition.z, o.puppet ? " (puppet)" : "");
		}
	shown = 0;
	for (const Obj& o : sObjs)
		if (o.kind == KIND_PLATFORM && (o.everMoved || o.puppet) && shown < 3) {
			shown++;
			std::fprintf(stderr, "[mp-obj]   platform %08x owner %d pos %.0f %.0f %.0f rot %.1f %.1f %.1f%s\n", o.id,
			             effectiveOwner(o), o.a->mPosition.x, o.a->mPosition.y, o.a->mPosition.z, o.a->mRotation.x,
			             o.a->mRotation.y, o.a->mRotation.z, o.puppet ? " (puppet)" : "");
		}
}

void gameFrame()
{
	initImage();
	TApplication* app   = SMSGetApplication();
	const int stage     = app->mCurrArea.getStage();
	const int scenario  = app->mCurrArea.getScenario();
	PortMpSessionSettings set;
	port_mp_get_settings(&set);
	// SMS_PORT_MP_SYNC_ENEMIES=1 (debug, host): turn the setting on once at the start.
	static bool sEnvDone;
	if (!sEnvDone && port_mp_is_host()) {
		sEnvDone = true;
		const char* e = std::getenv("SMS_PORT_MP_SYNC_ENEMIES");
		if (e != nullptr && *e != '\0') {
			set.reserved[kResSync] = (uint8_t)(std::atoi(e) == 1 ? 1 : 0);
			port_mp_host_set_settings(&set);
		}
	}
	const bool want = set.reserved[kResSync] == 1 && stage != 15;
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
	// build the list once the stage runs (director in its play state): during the load
	// gpConductor can still be null or the previous stage's (w12 saw it in the plaza).
	// NPCs are synced by w12's port_mp_npcs.cpp; they are not in this list (enemy types and
	// map objects only).
	if (!sActive && sFramesInVisit >= 2 && gpMarDirector != nullptr
	    && gpMarDirector->mState == TMarDirector::STATE_UNK4) {
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
	testKnob();
	ownerSend();
	updatePuppets();
	logFrame();
}

// ---- lobby panel -----------------------------------------------------------------------------
void lobbySection(int isHost)
{
	PortMpSessionSettings s;
	port_mp_get_settings(&s);
	static const char* const kModes[] = { "Off (default)", "Experimental" };
	int mode = s.reserved[kResSync] == 1 ? 1 : 0;
	if (isHost) {
		ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
		if (ImGui::Combo("Sync enemies", &mode, kModes, 2)) {
			s.reserved[kResSync] = (uint8_t)mode;
			port_mp_host_set_settings(&s);
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Experimental: common enemies (Stus, Electrokoopas, Cataquacks, Bloopers, ...), moving\n"
			                  "platforms and the bosses Petey Piranha and Gooper Blooper are shared by players in the\n"
			                  "same stage. Other enemies stay per player.");
	} else {
		ImGui::Text("Sync enemies: %s", kModes[mode]);
	}
	if (mode == 1)
		ImGui::TextDisabled("This stage: %d objects, %d run here, %d from other players", (int)sObjs.size(), sStatOwned,
		                    sStatPuppets);
}

struct Register {
	Register()
	{
		port_mp_register_game_frame(gameFrame);
		port_mp_register_lobby_section("Enemy sync (experimental)", lobbySection);
	}
} sRegister;

// The pose ~100 ms behind the newest sample, interpolated between the two around that time.
void samplePose(const Obj& o, float pos[3], float rot[3])
{
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
	for (int k = 0; k < 3; k++) {
		pos[k] = s1->pos[k] + (s0->pos[k] - s1->pos[k]) * f;
		rot[k] = lerpAngle(s1->rot[k], s0->rot[k], f);
	}
}

Obj* findLive(void* actor)
{
	if (!sActive || !port_mp_active() || (gpMarDirector != sVisitDir || gPortMpStageSerial != sVisitDirSerial))
		return nullptr;
	auto it = sByActor.find((TLiveActor*)actor);
	return it == sByActor.end() ? nullptr : &sObjs[it->second];
}

} // namespace

// TLiveActor::perform (liveactor.cpp, SMS_PORT_AURORA): 1 = this actor is driven by another
// player; its pose was set here and its own movement must be skipped.
extern "C" int port_mp_obj_puppet_move(TLiveActor* a)
{
	if (!port_mp_active() || (gpMarDirector != sVisitDir || gPortMpStageSerial != sVisitDirSerial)) {
		// the session ended or the stage changed without a game frame of ours: forget
		// everything, so no actor of a later stage (same address) is ever taken for a puppet
		resetVisit();
		sActive = false;
		return 0;
	}
	auto it = sByActor.find(a);
	if (it == sByActor.end())
		return 0;
	Obj& o = sObjs[it->second];
	if (!o.puppet || o.ns == 0)
		return 0;
	float pos[3], rot[3];
	samplePose(o, pos, rot);
	if (o.kind == KIND_BOSS || o.kind == KIND_FOLLOW) {
		// follow the owner: its nerve on every change, its HP, its pose; then our AI runs
		if (!bossActive(a))
			return 0; // our copy is not in the fight: leave it alone
		if (o.remoteNerve != nullptr && o.remoteNerve != o.appliedNerve) {
			forceNerve(a, o.remoteNerve);
			o.appliedNerve = o.remoteNerve;
		}
		((TSpineEnemy*)a)->mHitPoints = o.remoteHp;
		a->mPosition.set(pos[0], pos[1], pos[2]);
		a->mRotation.set(rot[0], rot[1], rot[2]);
		return 0;
	}
	a->mPosition.set(pos[0], pos[1], pos[2]);
	a->mRotation.set(rot[0], rot[1], rot[2]);
	if (o.kind == KIND_ENEMY)
		((TSpineEnemy*)a)->mHitPoints = o.remoteHp;
	a->setGroundCollision(); // moves a platform's collision (riders follow it)
	return 1;
}

// MarioCheckCol.cpp / MarioCollision.cpp / ModelWaterManager.cpp (port_mp_obj.h): our Mario
// or our water is about to hit `target`. If another player runs it, send the hit there; the
// local receiveMessage still runs (Mario's own bounce / the splash), but the puppet's
// reaction does not (its AI is skipped), so the owner's copy decides.
extern "C" void port_mp_mapreact_hit(void* target, const float* senderPos, unsigned message, int water);

extern "C" void port_mp_obj_hit(void* target, const float* senderPos, unsigned message, int water)
{
	port_mp_mapreact_hit(target, senderPos, message, water); // map objects (port_mp_mapreact.cpp)
	Obj* o = findLive(target);
	if (o == nullptr || !o->puppet || (o->kind != KIND_ENEMY && o->kind != KIND_FOLLOW) || sApplyingRemote)
		return;
	// an Electrokoopa answers a stomp by shocking "Mario": on its owner that would be the
	// owner's own Mario, so only water is forwarded for it
	if (!water && o->a->getActorType() == ACTOR_TYPE_ELEC_NOKONOKO)
		return;
	const double now = port_mp_time_ms();
	if (water) {
		if (now - o->lastWaterHit < 100.0)
			return; // the water stream hits every frame; 10 per second is plenty
		o->lastWaterHit = now;
	}
	u8 buf[sizeof(MsgHeader) + sizeof(HitBody)];
	MsgHeader h;
	header(h, MSG_HIT, 0, 0);
	HitBody b;
	std::memset(&b, 0, sizeof(b));
	b.id      = o->id;
	b.message = message;
	std::memcpy(b.pos, senderPos, sizeof(b.pos));
	b.water = (u8)(water ? 1 : 0);
	std::memcpy(buf, &h, sizeof(h));
	std::memcpy(buf + sizeof(h), &b, sizeof(b));
	port_mp_send(PORT_MP_CH_OBJECTS, buf, (int)sizeof(buf), water ? 0 : 1, effectiveOwner(*o));
	sStatHitsSent++;
	if (logOn())
		std::fprintf(stderr, "[mp-obj] hit %u on %08x sent to slot %d%s\n", message, o->id, effectiveOwner(*o),
		             water ? " (water)" : "");
}

// bossgesso.cpp / bosspakkun.cpp, top of the damage functions (port_mp_obj.h).
extern "C" int port_mp_obj_boss_damage(void* boss, int kind)
{
	Obj* o = findLive(boss);
	if (o == nullptr || o->kind != KIND_BOSS || sApplyingRemote || !o->puppet)
		return 0; // we run this boss (or no sync): apply here
	u8 buf[sizeof(MsgHeader) + sizeof(BossDamageBody)];
	MsgHeader h;
	header(h, MSG_BOSS_DAMAGE, 0, 0);
	BossDamageBody b;
	std::memset(&b, 0, sizeof(b));
	b.id   = o->id;
	b.kind = (u8)kind;
	std::memcpy(buf, &h, sizeof(h));
	std::memcpy(buf + sizeof(h), &b, sizeof(b));
	port_mp_send(PORT_MP_CH_OBJECTS, buf, (int)sizeof(buf), 1, effectiveOwner(*o));
	sStatHitsSent++;
	std::fprintf(stderr, "[mp-obj] boss damage %d on %08x sent to slot %d\n", kind, o->id, effectiveOwner(*o));
	return 1;
}

#endif // SMS_PORT_AURORA
