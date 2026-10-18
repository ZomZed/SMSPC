// port_mp_water.cpp - the other players' FLUDD water as real game water (w15).
//
// Sender: every emit request of the local Mario's own emit infos (FLUDD and Yoshi juice
// through TWaterGun::mEmitInfo, the drips of a wet Mario (unk158), the water lost on
// damage (unk154)) is seen through gPortWaterEmitHook (ModelWaterManager.cpp, SMS_PORT)
// and kept until the logic-frame hook. There the frame's emits go out on their own channel
// (PORT_MP_CH_WATER, unreliable), tagged with the snapshot tick, positions relative to
// Mario. Each record holds what makeEmit reads (count actually emitted, direction,
// power, size, tremble, particle type, lifetime, flag) and the simulation tick it was
// emitted in (two per logic frame).
//
// Receiver: records queue per player slot and are played when that player's puppet
// (drawn ~100 ms behind, port_mp_puppet.cpp) reaches their tick, at the puppet's
// position: TModelWaterManager::portEmitRemote makes the same particles (streams, drops,
// hover/rocket/turbo jets, juice) in the local water manager, flagged remote. They fly,
// land, splash, leave puddles and run down walls like the local water, but are visual
// only: no enemy or object hits, no goop cleaning, no sounds, their own random numbers;
// the owner's game does all that and the goop / enemy / event sync carries it. They use
// slots past the local SLOT_NUM budget (at most PORT_REMOTE_MAX at once), so the local
// water never runs short because of them.
//
// Ghost harness: SMS_PORT_MP_GHOST=<file> also writes <file>.wtr; SMS_PORT_MP_GHOST_PLAY
// plays it back with the ghost puppets (port_mp_water_ghost_feed).
#include "port_mp_puppet.h"
#include "port_mp.h"

#include <Player/Mario.hpp>
#include <Player/MarioAccess.hpp>
#include <Player/ModelWaterManager.hpp>
#include <Player/WaterGun.hpp>
#include <System/MarDirector.hpp>
#include <dolphin/os.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

// Fallbacks (lld-link /alternatename) when the frame-rate code is not linked.
extern "C" {
float port_mp_w15_no_interp_alpha(void) { return 1.0f; }
bool port_mp_w15_no_interp_bool(void) { return false; }
}
#pragma comment(linker, "/alternatename:port_interp_alpha=port_mp_w15_no_interp_alpha")
#pragma comment(linker, "/alternatename:port_interp_drawing=port_mp_w15_no_interp_bool")
#pragma comment(linker, "/alternatename:port_interp_interpolating=port_mp_w15_no_interp_bool")

namespace {

const uint8_t kChannel = 20; // PORT_MP_CH_WATER (MULTIPLAYER.md, "Puppet block")
const uint8_t kVersion = 1;

#pragma pack(push, 1)
struct Rec {
	uint8_t age;   // simulation ticks between the emit and the snapshot (0 = last tick)
	uint8_t num;   // particles emitted
	uint8_t type;  // TWaterEmitInfo::mType (TModelWaterManager::mWaterParticleTypes)
	uint8_t flag;  // TWaterEmitInfo::mFlag, low byte (0x40 FLUDD water, 0x80 ...)
	int16_t alive; // TWaterEmitInfo::mAlive
	uint16_t pad;
	float pos[3]; // mPos - TMario::mPosition at the snapshot
	float dir[3];
	float v[3];
	float dirTremble, pow, powTremble, size, sizeTremble;
};
struct MsgHdr {
	uint8_t version;
	uint8_t count;
	uint16_t pad;
	uint32_t tick; // the PortMpSnapshot tick these belong to
};
#pragma pack(pop)
static_assert(sizeof(Rec) == 64, "water record");

const int kMaxPerFrame = 12;

// ---- sender --------------------------------------------------------------------------
struct Pending {
	Rec r;
	JGeometry::TVec3<f32> pos; // absolute
	int tick;                  // TMarDirector::mTickCount
};
Pending sPending[kMaxPerFrame];
int sPendingN;
bool sCollect; // a session or a ghost recording wants the local water

FILE* sGhostRec;
bool sGhostRecChecked;

void onEmit(const TWaterEmitInfo& info, int emitted)
{
	if (!sCollect || emitted <= 0 || sPendingN >= kMaxPerFrame)
		return;
	TMario* m = gpMarioOriginal;
	if (m == nullptr || gpMarDirector == nullptr)
		return;
	const bool mine = (m->mWaterGun != nullptr && &info == m->mWaterGun->mEmitInfo)
	                  || &info == m->unk154 || &info == m->unk158;
	if (!mine)
		return;
	Pending& p = sPending[sPendingN++];
	Rec& r     = p.r;
	memset(&r, 0, sizeof(r));
	r.num   = (uint8_t)(emitted > 255 ? 255 : emitted);
	r.type  = (uint8_t)info.mType.get();
	r.flag  = (uint8_t)info.mFlag.get();
	r.alive = info.mAlive.get();
	const JGeometry::TVec3<f32>& d = info.mDir.get();
	const JGeometry::TVec3<f32>& v = info.mV.get();
	r.dir[0] = d.x, r.dir[1] = d.y, r.dir[2] = d.z;
	r.v[0] = v.x, r.v[1] = v.y, r.v[2] = v.z;
	r.dirTremble  = info.mDirTremble.get();
	r.pow         = info.mPow.get();
	r.powTremble  = info.mPowTremble.get();
	r.size        = info.mSize.get();
	r.sizeTremble = info.mSizeTremble.get();
	p.pos         = info.mPos.get();
	p.tick        = gpMarDirector->mTickCount;
}

struct HookInstall {
	HookInstall() { gPortWaterEmitHook = onEmit; }
} sHookInstall;

// ---- receiver ------------------------------------------------------------------------
const int kQueue = 64;
struct Queued {
	uint32_t seq;
	Rec r;
};
struct SlotQ {
	Queued q[kQueue];
	int head, count;
};
SlotQ sQ[PORT_MP_MAX_PUPPETS];
float sLastPuppetPos[3]; // the last puppet that emitted (log only)

void enqueue(int slot, uint32_t seq, const Rec& r)
{
	if (slot < 0 || slot >= PORT_MP_MAX_PUPPETS)
		return;
	SlotQ& s = sQ[slot];
	if (s.count == kQueue) { // drop the oldest
		s.head = (s.head + 1) % kQueue;
		--s.count;
	}
	Queued& e = s.q[(s.head + s.count) % kQueue];
	e.seq     = seq;
	e.r       = r;
	++s.count;
}

// ---- ghost sidecar -------------------------------------------------------------------
struct GhostRec {
	uint32_t seq;
	Rec r;
};
GhostRec* sGhost;
int sGhostN;
int sGhostCursor;
bool sGhostLoaded;

void ghostLoad()
{
	if (sGhostLoaded)
		return;
	sGhostLoaded     = true;
	const char* path = getenv("SMS_PORT_MP_GHOST_PLAY");
	if (path == nullptr || path[0] == '\0')
		return;
	char wp[1024];
	snprintf(wp, sizeof(wp), "%s.wtr", path);
	FILE* f = fopen(wp, "rb");
	if (f == nullptr)
		return;
	char magic[8];
	if (fread(magic, 1, 8, f) != 8 || memcmp(magic, "SMSWTR01", 8) != 0) {
		fclose(f);
		return;
	}
	fseek(f, 0, SEEK_END);
	const long n = (ftell(f) - 8) / (long)sizeof(GhostRec);
	fseek(f, 8, SEEK_SET);
	sGhost  = (GhostRec*)malloc(sizeof(GhostRec) * (n > 0 ? n : 1));
	sGhostN = (int)fread(sGhost, sizeof(GhostRec), n > 0 ? n : 0, f);
	fclose(f);
	OSReport("[mp] water: %d ghost water records from %s\n", sGhostN, wp);
}

} // namespace

// port_mp_logic_frame, after the capture: the water the local Mario emitted since the
// last call goes out with snapshot `seq` (pos = the snapshot's Mario position).
extern "C" void port_mp_water_local_frame(int captured, uint32_t seq, const float pos[3], int send,
                                         int record)
{
	sCollect = send || record;
	// SMS_PORT_MP_WATER_LOG=1: particle counts once a second (local / remote / slots).
	static const bool sLog = getenv("SMS_PORT_MP_WATER_LOG") != nullptr;
	if (sLog && captured && (seq % 30) == 0 && gpModelWaterManager != nullptr) {
		const TModelWaterManager* w = gpModelWaterManager;
		f32 c[2][3] = {};
		int n[2]    = {};
		int hits = 0, ground[2] = {};
		for (int i = 0; i < w->mParticleCount; ++i) {
			const int k = w->portIsRemote(i) ? 1 : 0;
			++n[k];
			hits += w->unk2514[i] != nullptr;
			ground[k] += (w->mParticleFlagSOA[i] & 0xF) != 1;
			for (int j = 0; j < 3; ++j)
				c[k][j] += (&w->mParticlePositionSOA[i].x)[j] - (k ? sLastPuppetPos[j] : pos[j]);
		}
		for (int k = 0; k < 2; ++k)
			for (int j = 0; j < 3; ++j)
				c[k][j] = n[k] ? c[k][j] / n[k] : 0.0f;
		OSReport("[mp] water: seq %u local %d remote %d (budget %d + %d); local centre %.0f %.0f %.0f "
		         "from Mario, remote centre %.0f %.0f %.0f from its puppet; landed %d / %d, hitting %d\n",
		         seq, n[0], n[1], w->unk10, (int)TModelWaterManager::PORT_REMOTE_MAX, c[0][0], c[0][1],
		         c[0][2], c[1][0], c[1][1], c[1][2], ground[0], ground[1], hits);
	}
	if (!captured || sPendingN == 0) {
		sPendingN = 0;
		return;
	}
	const int now = gpMarDirector != nullptr ? gpMarDirector->mTickCount : 0;
	uint8_t buf[sizeof(MsgHdr) + sizeof(Rec) * kMaxPerFrame];
	MsgHdr h;
	memset(&h, 0, sizeof(h));
	h.version = kVersion;
	h.count   = (uint8_t)sPendingN;
	h.tick    = seq;
	memcpy(buf, &h, sizeof(h));
	for (int i = 0; i < sPendingN; ++i) {
		Pending& p = sPending[i];
		const int age = now - p.tick;
		p.r.age       = (uint8_t)(age < 0 ? 0 : (age > 3 ? 3 : age));
		p.r.pos[0]    = p.pos.x - pos[0];
		p.r.pos[1]    = p.pos.y - pos[1];
		p.r.pos[2]    = p.pos.z - pos[2];
		memcpy(buf + sizeof(h) + i * sizeof(Rec), &p.r, sizeof(Rec));
	}
	if (send)
		port_mp_send(kChannel, buf, (int)(sizeof(h) + sPendingN * sizeof(Rec)), 0, -1);
	if (record) {
		if (!sGhostRecChecked) {
			sGhostRecChecked = true;
			if (const char* g = getenv("SMS_PORT_MP_GHOST")) {
				char wp[1024];
				snprintf(wp, sizeof(wp), "%s.wtr", g);
				sGhostRec = fopen(wp, "wb");
				if (sGhostRec != nullptr)
					fwrite("SMSWTR01", 1, 8, sGhostRec);
			}
		}
		if (sGhostRec != nullptr) {
			for (int i = 0; i < sPendingN; ++i) {
				GhostRec g;
				g.seq = seq;
				g.r   = sPending[i].r;
				fwrite(&g, sizeof(g), 1, sGhostRec);
			}
			if ((seq & 31) == 0)
				fflush(sGhostRec);
		}
	}
	sPendingN = 0;
}

// Network: take the other players' water messages (game thread, every logic frame).
extern "C" void port_mp_water_receive(void)
{
	uint8_t buf[PORT_MP_APP_MAX];
	int from = -1, n;
	while ((n = port_mp_recv(kChannel, buf, sizeof(buf), &from)) != 0) {
		if (n < (int)sizeof(MsgHdr))
			continue;
		MsgHdr h;
		memcpy(&h, buf, sizeof(h));
		if (h.version != kVersion || (int)(sizeof(h) + h.count * sizeof(Rec)) > n)
			continue;
		for (int i = 0; i < h.count; ++i) {
			Rec r;
			memcpy(&r, buf + sizeof(h) + i * sizeof(Rec), sizeof(r));
			enqueue(from, h.tick, r);
		}
	}
}

// Ghost playback: the recording's snapshot `srcSeq` was just fed to `slot` as `seq`.
extern "C" void port_mp_water_ghost_feed(uint32_t srcSeq, uint32_t seq, int slot)
{
	ghostLoad();
	if (sGhostN == 0)
		return;
	if (sGhostCursor >= sGhostN || sGhost[sGhostCursor].seq > srcSeq)
		sGhostCursor = 0; // (a stage restart plays from earlier on)
	while (sGhostCursor < sGhostN && sGhost[sGhostCursor].seq < srcSeq)
		++sGhostCursor;
	for (int i = sGhostCursor; i < sGhostN && sGhost[i].seq == srcSeq; ++i)
		enqueue(slot, seq, sGhost[i].r);
}

// Every logic frame per puppet slot: play the queued water up to the puppet's playback
// tick `playSeq`, from its drawn position. show = 0 (not drawn, other stage, too far)
// drops it. Late records (a few ticks) still play; older ones are dropped.
extern "C" void port_mp_water_replay(int slot, int havePlay, uint32_t playSeq, const float pos[3],
                                    int show)
{
	if (slot < 0 || slot >= PORT_MP_MAX_PUPPETS)
		return;
	SlotQ& s = sQ[slot];
	TModelWaterManager* w = gpModelWaterManager;
	while (s.count > 0) {
		Queued& e = s.q[s.head];
		if (havePlay && (int32_t)(e.seq - playSeq) > 0)
			break; // not yet
		if (havePlay && show && w != nullptr && (int32_t)(playSeq - e.seq) <= 4) {
			TPortWaterEmit em;
			em.pos.set(pos[0] + e.r.pos[0], pos[1] + e.r.pos[1], pos[2] + e.r.pos[2]);
			em.dir.set(e.r.dir[0], e.r.dir[1], e.r.dir[2]);
			em.v.set(e.r.v[0], e.r.v[1], e.r.v[2]);
			em.dirTremble  = e.r.dirTremble;
			em.pow         = e.r.pow;
			em.powTremble  = e.r.powTremble;
			em.size        = e.r.size;
			em.sizeTremble = e.r.sizeTremble;
			em.alive       = e.r.alive;
			em.flag        = e.r.flag;
			em.type        = e.r.type;
			w->portEmitRemote(em, e.r.num, e.r.age);
			memcpy(sLastPuppetPos, pos, sizeof(sLastPuppetPos));
		}
		s.head = (s.head + 1) % kQueue;
		--s.count;
	}
}

extern "C" void port_mp_water_clear(int slot)
{
	if (slot >= 0 && slot < PORT_MP_MAX_PUPPETS)
		sQ[slot].head = sQ[slot].count = 0;
}
