// Multiplayer game mode "Prop Hunt" (mode PORT_MP_MODE_PROP, run by port_mp_modes.cpp like Hide and
// Seek, port_mp_hideseek.cpp, whose round structure this follows; played in the plaza, one level or
// all levels: the "Map" setting, port_mp_arena.cpp).
//
//  - Hiders are Shadow Mario (the game's own /kagemario model and paintbrush, read once from the
//    disc's Shadow Mario episode archive; posed from the player's Mario like TEnemyMario::perform
//    does) and have no FLUDD (MARIO_FLAG_HAS_FLUDD off, as in the secret courses). R (the spray
//    button, free without FLUDD) next to an object (no bigger than kPropMaxSize each way: no walls or
//    buildings) turns the hider into a copy of it (a J3DModel of
//    that object's model data, drawn where his Mario is, everyone sees it); R away from objects turns
//    him back into Shadow Mario. His own Mario model is not drawn meanwhile (TMario::perform hook).
//    The object is named by its actor type and scene name (the same on every client in the stage).
//  - Hunters are normal Marios with FLUDD whose tank empties much faster (host setting, default 4x).
//    Their water hitting a hider (as drawn: the disguise's size) costs him health (host setting,
//    default 2 of 8 per hit; the game's own damageExec: knock-back, health meter, invincibility).
//    A hider whose health runs out dies, and a hider who dies is caught: he hunts (or is out, host
//    setting), the same rule as Hide and Seek and Freeze Tag.
//  - Hide phase (default 45 s): hunters' screens are black and their pads idle. Hunt phase (default
//    4 min): hunters win when no hider is left, hiders when the time runs out.
// State on PORT_MP_CH_PROP from the host (reliable on every change, once a second for late joiners).
#ifdef SMS_PORT_AURORA

#include "port_mp.h"

#include <Camera/CubeManagerBase.hpp>
#include <JSystem/J3D/J3DGraphAnimator/J3DAnimation.hpp>
#include <JSystem/J3D/J3DGraphAnimator/J3DJoint.hpp>
#include <JSystem/J3D/J3DGraphAnimator/J3DMaterialAnm.hpp>
#include <JSystem/J3D/J3DGraphAnimator/J3DModel.hpp>
#include <JSystem/J3D/J3DGraphLoader/J3DAnmLoader.hpp>
#include <JSystem/J3D/J3DGraphLoader/J3DModelLoader.hpp>
#include <JSystem/JDrama/JDRNameRef.hpp>
#include <JSystem/JKernel/JKRHeap.hpp>
#include <M3DUtil/M3UModelMario.hpp>
#include <M3DUtil/MActor.hpp>
#include <MSound/MSound.hpp>
#include <MSound/SoundEffects.hpp>
#include <Map/Map.hpp>
#include <Map/MapData.hpp>
#include <MarioUtil/LightUtil.hpp>
#include <MarioUtil/MathUtil.hpp>
#include <MarioUtil/PacketUtil.hpp>
#include <MarioUtil/ScreenUtil.hpp>
#include <Enemy/Conductor.hpp>
#include <Player/Mario.hpp>
#include <Player/MarioAccess.hpp>
#include <Player/ModelWaterManager.hpp>
#include <Player/Watergun.hpp>
#include <Strategic/ActorTypes.hpp>
#include <Strategic/LiveActor.hpp>
#include <Strategic/LiveManager.hpp>
#include <System/Application.hpp>
#include <System/MarDirector.hpp>
#include <System/MarioGamePad.hpp>
#include <dolphin/dvd.h>
#include <dolphin/os.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>

extern "C" void port_mp_toast(const char* text); // port_mp_menu.cpp
extern "C" void port_mp_prop_settings(int* hideS, int* huntS, int* convert, int* pick, int* damage,
                                      int* waterMul);  // port_mp_modes.cpp
extern "C" void port_mp_modes_round_over(void);       // port_mp_modes.cpp
extern "C" int port_mp_home_stage(void);              // port_mp_arena.cpp
extern "C" void* port_mp_puppet_body(int slot);       // port_mp_puppet.cpp: the posed body (J3DModel*)
extern "C" int port_mp_puppet_pos(int slot, float pos[3]);
extern "C" int port_mp_prop_taunt_seconds(void); // port_mp_modes.cpp (host setting)
extern "C" int port_mp_manhunt_running(void);   // port_mp_manhunt.cpp: its hunters use these looks
extern "C" int port_mp_manhunt_shadow(int slot);

namespace {

const uint8_t kChannel = PORT_MP_CH_PROP;
const int kLocal       = PORT_MP_MAX_PLAYERS; // look slot of the local player
const f32 kPropMaxSize  = 320.0f; // the biggest object a hider can become (each way; Mario is ~160 tall)
enum Phase : u8 { PP_OFF = 0, PP_HIDE = 1, PP_HUNT = 2, PP_OVER = 3 };
enum Role : u8 { R_NONE = 0, R_HIDER = 1, R_HUNTER = 2, R_OUT = 3 };
enum : u8 { MSG_STATE = 1, MSG_PROP = 2, MSG_DIED = 3, MSG_SPRAY = 4 };

#pragma pack(push, 1)
struct State {
	u8 type; // MSG_STATE
	u8 phase;
	u8 round;  // +1 per round
	u8 winner; // PP_OVER: 1 hiders, 2 hunters
	u32 leftMs;
	u8 role[PORT_MP_MAX_PLAYERS];
	u8 died[PORT_MP_MAX_PLAYERS];         // 1 = caught by dying (toasts)
	u32 propType[PORT_MP_MAX_PLAYERS];    // the disguise: the object's actor type (0 = Shadow Mario)
	u16 propKey[PORT_MP_MAX_PLAYERS];     // and its scene name's key code
};
struct PropMsg {
	u8 type; // MSG_PROP (hider -> host)
	u8 pad;
	u16 key;
	u32 actorType; // 0 = back to Shadow Mario
};
struct SprayMsg {
	u8 type; // MSG_SPRAY (hunter -> the hider hit)
	u8 victim;
	u8 damage;
	u8 pad;
	float pos[3]; // the hunter (knock-back direction)
};
#pragma pack(pop)

State sState = { MSG_STATE, PP_OFF, 0, 0, 0, {}, {}, {}, {} };
State sSeen;
double sDeadline;
double sLastSend;
bool sDirty;
double sModeSeenAt;
bool sBlind;

bool running()
{
	PortMpSessionSettings s;
	port_mp_get_settings(&s);
	return port_mp_active() && s.gameMode == PORT_MP_MODE_PROP;
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
u8 myRole() { return me() >= 0 && me() < PORT_MP_MAX_PLAYERS ? sState.role[me()] : R_NONE; }
bool roundOn() { return sState.phase == PP_HIDE || sState.phase == PP_HUNT; }

// ---- host ------------------------------------------------------------------------------------
void hostStartRound()
{
	int hideS, huntS, convert, pick, damage, water;
	port_mp_prop_settings(&hideS, &huntS, &convert, &pick, &damage, &water);
	int slots[PORT_MP_MAX_PLAYERS], n = 0;
	for (int i = 0; i < PORT_MP_MAX_PLAYERS; i++)
		if (used(i))
			slots[n++] = i;
	std::memset(sState.role, R_NONE, sizeof(sState.role));
	std::memset(sState.died, 0, sizeof(sState.died));
	std::memset(sState.propType, 0, sizeof(sState.propType));
	std::memset(sState.propKey, 0, sizeof(sState.propKey));
	if (n == 0)
		return;
	const int hunter = pick >= 0 && pick < PORT_MP_MAX_PLAYERS && used(pick) ? pick : slots[std::rand() % n];
	for (int i = 0; i < n; i++)
		sState.role[slots[i]] = slots[i] == hunter ? R_HUNTER : R_HIDER;
	sState.round++;
	sState.winner = 0;
	setPhase(PP_HIDE, hideS);
	char nm[32];
	std::fprintf(stderr, "[mp-prop] round %d: hunter %s (slot %d), %d hiders, hide %d s\n", sState.round,
	             nameOf(hunter, nm, sizeof(nm)), hunter, n - 1, hideS);
}

void hostDied(int slot)
{
	if (!roundOn() || slot < 0 || slot >= PORT_MP_MAX_PLAYERS || sState.role[slot] != R_HIDER)
		return;
	int hideS, huntS, convert, pick, damage, water;
	port_mp_prop_settings(&hideS, &huntS, &convert, &pick, &damage, &water);
	sState.role[slot]     = convert ? R_HUNTER : R_OUT;
	sState.died[slot]     = 1;
	sState.propType[slot] = 0;
	sState.propKey[slot]  = 0;
	sDirty                = true;
	char a[32];
	std::fprintf(stderr, "[mp-prop] %s was caught: %s\n", nameOf(slot, a, sizeof(a)), convert ? "hunts now" : "out");
}

void hostProp(int slot, u32 type, u16 key)
{
	if (!roundOn() || slot < 0 || slot >= PORT_MP_MAX_PLAYERS || sState.role[slot] != R_HIDER)
		return;
	sState.propType[slot] = type;
	sState.propKey[slot]  = key;
	sDirty                = true;
	char a[32];
	std::fprintf(stderr, "[mp-prop] %s: %s %08x/%04x\n", nameOf(slot, a, sizeof(a)), type ? "disguise" : "no disguise",
	             (unsigned)type, key);
}

void hostFrame()
{
	if (!running()) {
		if (sState.phase != PP_OFF) {
			std::memset(sState.role, R_NONE, sizeof(sState.role));
			setPhase(PP_OFF, 0);
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
	switch (sState.phase) {
	case PP_OFF: {
		TMarDirector* d = gpMarDirector;
		if (now - sModeSeenAt > 3000.0 && d != nullptr && d->mState == TMarDirector::STATE_UNK4
		    && SMSGetApplication()->mCurrArea.getStage() == port_mp_home_stage())
			hostStartRound();
		break;
	}
	case PP_HIDE:
		if (count(R_HIDER) == 0) {
			sState.winner = 2;
			setPhase(PP_OVER, 6);
		} else if (now >= sDeadline) {
			int hideS, huntS, convert, pick, damage, water;
			port_mp_prop_settings(&hideS, &huntS, &convert, &pick, &damage, &water);
			setPhase(PP_HUNT, huntS);
		}
		break;
	case PP_HUNT:
		if (count(R_HIDER) == 0 || count(R_HUNTER) == 0) {
			sState.winner = count(R_HIDER) == 0 ? 2 : 1;
			setPhase(PP_OVER, 6);
		} else if (now >= sDeadline) {
			sState.winner = 1;
			setPhase(PP_OVER, 6);
		}
		break;
	case PP_OVER:
		if (now >= sDeadline) {
			std::memset(sState.role, R_NONE, sizeof(sState.role));
			setPhase(PP_OFF, 0);
			sModeSeenAt = 0.0;
			port_mp_modes_round_over();
		}
		break;
	}
}

// ---- Shadow Mario assets (read once from the disc) --------------------------------------------
struct KageFiles {
	bool tried, ok;
	std::vector<u8> body, brush, btk;
} sKage;

std::vector<u8> yaz0(const std::vector<u8>& d)
{
	std::vector<u8> out;
	if (d.size() < 16 || std::memcmp(d.data(), "Yaz0", 4) != 0)
		return d;
	const u32 size = (u32)d[4] << 24 | (u32)d[5] << 16 | (u32)d[6] << 8 | d[7];
	out.reserve(size);
	size_t i = 16;
	while (out.size() < size && i < d.size()) {
		const u8 code = d[i++];
		for (int bit = 0; bit < 8 && out.size() < size && i < d.size(); bit++) {
			if (code & (0x80 >> bit)) {
				out.push_back(d[i++]);
				continue;
			}
			if (i + 1 >= d.size())
				return std::vector<u8>();
			const u8 b1 = d[i], b2 = d[i + 1];
			i += 2;
			const size_t dist = (size_t)((b1 & 15) << 8 | b2) + 1;
			size_t n          = b1 >> 4;
			if (n == 0) {
				if (i >= d.size())
					return std::vector<u8>();
				n = d[i++] + 0x12;
			} else {
				n += 2;
			}
			if (dist > out.size())
				return std::vector<u8>();
			for (size_t k = 0; k < n; k++)
				out.push_back(out[out.size() - dist]);
		}
	}
	return out;
}

u32 be32(const std::vector<u8>& d, size_t o)
{
	return o + 4 <= d.size() ? (u32)d[o] << 24 | (u32)d[o + 1] << 16 | (u32)d[o + 2] << 8 | d[o + 3] : 0;
}
u16 be16(const std::vector<u8>& d, size_t o) { return o + 2 <= d.size() ? (u16)(d[o] << 8 | d[o + 1]) : 0; }

// A file of a RARC archive by its path ("/kagemario/default.bmd"; the root folder's name skipped).
bool rarcFile(const std::vector<u8>& d, const char* want, std::vector<u8>& out)
{
	if (d.size() < 0x40 || std::memcmp(d.data(), "RARC", 4) != 0)
		return false;
	const u32 dataOff = be32(d, 0xC) + 0x20;
	const u32 nodeOff = be32(d, 0x24) + 0x20, entOff = be32(d, 0x2C) + 0x20, strOff = be32(d, 0x34) + 0x20;
	struct Walk {
		const std::vector<u8>& d;
		u32 dataOff, nodeOff, entOff, strOff;
		const char* want;
		std::vector<u8>& out;
		int depth;
		const char* name(u32 o) const { return strOff + o < d.size() ? (const char*)&d[strOff + o] : ""; }
		bool node(u32 ni, const std::string& path)
		{
			if (++depth > 16)
				return false;
			const u32 o = nodeOff + ni * 16;
			const u16 n = be16(d, o + 10);
			const u32 first = be32(d, o + 12);
			for (u32 k = 0; k < n; k++) {
				const u32 e     = entOff + (first + k) * 20;
				const u8 type   = e + 4 < d.size() ? d[e + 4] : 0;
				const std::string nm = name(be16(d, e + 6));
				const u32 off = be32(d, e + 8), size = be32(d, e + 12);
				if (type & 2) {
					if (nm != "." && nm != ".." && node(off, path + nm + "/"))
						return true;
				} else if (path + nm == want && dataOff + off + size <= d.size()) {
					out.assign(d.begin() + dataOff + off, d.begin() + dataOff + off + size);
					return true;
				}
			}
			depth--;
			return false;
		}
	} w = { d, dataOff, nodeOff, entOff, strOff, want, out, 0 };
	return w.node(0, "/");
}

// Shadow Mario's model, brush and texture scroll from the first Shadow Mario episode archive found.
bool loadKageFiles()
{
	if (sKage.tried)
		return sKage.ok;
	sKage.tried = true;
	static const char* const kArchives[] = { "/data/scene/bianco6.szs", "/data/scene/ricco6.szs",
		                                     "/data/scene/mamma6.szs", "/data/scene/sirena6.szs",
		                                     "/data/scene/monte6.szs", "/data/scene/mare6.szs" };
	for (const char* path : kArchives) {
		DVDFileInfo fi;
		if (!DVDOpen((char*)path, &fi))
			continue;
		const u32 len = (fi.length + 31) & ~31u;
		std::vector<u8> raw(len + 64);
		u8* aligned = (u8*)(((uintptr_t)raw.data() + 31) & ~(uintptr_t)31);
		const s32 got = DVDReadPrio(&fi, aligned, (s32)len, 0, 2);
		DVDClose(&fi);
		if (got <= 0)
			continue;
		std::vector<u8> szs(aligned, aligned + fi.length);
		const std::vector<u8> arc = yaz0(szs);
		if (rarcFile(arc, "/kagemario/default.bmd", sKage.body) && rarcFile(arc, "/kagemario/kagemario_brush.bmd", sKage.brush)) {
			rarcFile(arc, "/kagemario/kagemario_scroll.btk", sKage.btk);
			sKage.ok = true;
			std::fprintf(stderr, "[mp-prop] Shadow Mario model from %s (%u + %u + %u bytes)\n", path,
			             (unsigned)sKage.body.size(), (unsigned)sKage.brush.size(), (unsigned)sKage.btk.size());
			return true;
		}
	}
	std::fprintf(stderr, "[mp-prop] Shadow Mario model not found on the disc: hiders drawn as Mario\n");
	return false;
}

// ---- looks: Shadow Mario or a disguise, per player, per stage ----------------------------------
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

struct Look {
	J3DModel* kage;
	J3DModel* brush;
	bool kageTried;
	J3DModel* prop;
	u32 propType;
	u16 propKey;
	f32 propRadius, propHeight;
	Mtx propRot; // rotation and scale of the object it copies
	int mode;    // this frame: 0 = normal Mario, 1 = Shadow Mario, 2 = disguise
	f32 pos[3];
	bool drawn;
};
struct StageLooks {
	unsigned serial;
	bool valid;
	J3DModelData* kageData;
	J3DModelData* brushData;
	J3DAnmTextureSRTKey* btk;
	J3DTexMtxAnm* btkAnm;
	f32 btkFrame;
	bool dataTried;
	u16 handL;
	Look look[PORT_MP_MAX_PLAYERS + 1];
} sLooks;

void resetLooksIfNewStage()
{
	if (sLooks.valid && sLooks.serial == gPortMpStageSerial)
		return;
	std::memset((void*)&sLooks, 0, sizeof(sLooks));
	sLooks.serial = gPortMpStageSerial;
	sLooks.valid  = true;
}

void* heapCopy(const std::vector<u8>& v)
{
	JKRHeap* heap = JKRGetCurrentHeap();
	if (heap == nullptr || v.empty() || heap->getFreeSize() < (s32)v.size() + 0x40000)
		return nullptr;
	void* p = JKRHeap::alloc((u32)v.size(), 0x20, heap);
	if (p != nullptr) {
		std::memcpy(p, v.data(), v.size());
		DCFlushRange(p, (u32)v.size());
	}
	return p;
}

bool kageData()
{
	if (sLooks.dataTried)
		return sLooks.kageData != nullptr;
	sLooks.dataTried = true;
	if (!loadKageFiles())
		return false;
	StageHeapScope scope;
	void* body  = heapCopy(sKage.body);
	void* brush = heapCopy(sKage.brush);
	if (body == nullptr || brush == nullptr)
		return false;
	// as TEMario::init (the model flags, the screen texture of its see-through look) and
	// TEnemyMario::initEnemyValues (the brush)
	sLooks.kageData  = J3DModelLoaderDataBase::load(body, 0x11300000);
	sLooks.brushData = J3DModelLoaderDataBase::load(brush, 0x11040000);
	if (sLooks.kageData == nullptr)
		return false;
	if (gpScreenTexture != nullptr)
		gpScreenTexture->replace(sLooks.kageData, "H_kagemario_dummy");
	if (!sKage.btk.empty())
		if (void* btk = heapCopy(sKage.btk)) {
			sLooks.btk = (J3DAnmTextureSRTKey*)J3DAnmLoaderDataBase::load(btk);
			if (sLooks.btk != nullptr) {
				sLooks.btk->searchUpdateMaterialID(sLooks.kageData);
				const u16 n = sLooks.btk->getUpdateMaterialNum();
				void* mem   = JKRHeap::alloc(sizeof(J3DTexMtxAnm) * (n ? n : 1), 0x10, JKRGetCurrentHeap());
				if (mem != nullptr) {
					sLooks.btkAnm = (J3DTexMtxAnm*)mem;
					for (u16 j = 0; j < n; j++) {
						new (&sLooks.btkAnm[j]) J3DTexMtxAnm();
						sLooks.btkAnm[j].setAnmIndex(j);
						sLooks.btkAnm[j].setAnmTransform(sLooks.btk);
					}
				}
			}
		}
	return true;
}

J3DModel* newModel(J3DModelData* d)
{
	JKRHeap* heap = JKRGetCurrentHeap();
	void* mem     = d != nullptr && heap != nullptr ? JKRHeap::alloc(sizeof(J3DModel), 0x10, heap) : nullptr;
	return mem != nullptr ? new (mem) J3DModel(d, 0, 1) : nullptr;
}

bool kageModel(Look& l)
{
	if (l.kage != nullptr)
		return true;
	if (l.kageTried || !kageData())
		return false;
	l.kageTried = true;
	StageHeapScope scope;
	l.kage = newModel(sLooks.kageData);
	if (l.kage == nullptr)
		return false;
	for (u16 i = 0; i < sLooks.kageData->getMaterialNum(); i++)
		SMS_InitPacket_Fog(l.kage, i);
	l.brush = newModel(sLooks.brushData);
	return true;
}

// Every live actor with a model (the conductor's managers, then its lone actors).
template <typename F> void forEachModelActor(F f)
{
	if (gpConductor == nullptr)
		return;
	for (JGadget::TList<TLiveManager*>::iterator it = gpConductor->unk10.begin(); it != gpConductor->unk10.end(); ++it) {
		TLiveManager* mgr = *it;
		if (mgr == nullptr)
			continue;
		for (int i = 0; i < mgr->getObjNum(); ++i)
			if (TLiveActor* a = mgr->getObj(i))
				if (a->getMActor() != nullptr && a->getMActor()->getModel() != nullptr)
					f(a);
	}
	for (JGadget::TList<TLiveActor*>::iterator it = gpConductor->unk30.begin(); it != gpConductor->unk30.end(); ++it)
		if (TLiveActor* a = *it)
			if (a->getMActor() != nullptr && a->getMActor()->getModel() != nullptr)
				f(a);
}

// An object's size: the largest side of its model's bounding box (every joint's box and its
// radius, scaled), in world units.
f32 propSize(TLiveActor* a)
{
	J3DModelData* d = a->getMActor()->getModel()->getModelData();
	f32 lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
	f32 rad   = 0.0f;
	for (u16 i = 0; i < d->getJointNum(); i++) {
		J3DJoint* j = d->getJointNodePointer(i);
		if (j == nullptr)
			continue;
		const Vec& mn = j->getMin();
		const Vec& mx = j->getMax();
		const f32 a0[3] = { mn.x, mn.y, mn.z }, a1[3] = { mx.x, mx.y, mx.z };
		for (int k = 0; k < 3; k++) {
			lo[k] = a0[k] < lo[k] ? a0[k] : lo[k];
			hi[k] = a1[k] > hi[k] ? a1[k] : hi[k];
		}
		rad = j->getRadius() > rad ? j->getRadius() : rad;
	}
	const f32 sc[3] = { std::fabs(a->mScaling.x), std::fabs(a->mScaling.y), std::fabs(a->mScaling.z) };
	f32 size        = 0.0f;
	for (int k = 0; k < 3; k++)
		if (hi[k] >= lo[k] && (hi[k] - lo[k]) * sc[k] > size)
			size = (hi[k] - lo[k]) * sc[k];
	const f32 big = sc[0] > sc[1] ? (sc[0] > sc[2] ? sc[0] : sc[2]) : (sc[1] > sc[2] ? sc[1] : sc[2]);
	return size > rad * 2.0f * big ? size : rad * 2.0f * big;
}

bool propCandidate(TLiveActor* a)
{
	const u32 cat = a->getHitCategory();
	if (cat != HIT_CATEGORY_MAP_OBJECT && cat != HIT_CATEGORY_ITEM)
		return false;
	if (a->checkLiveFlag(LIVE_FLAG_DEAD) || a->isActorType(ACTOR_TYPE_SHINE) || a->isActorType(ACTOR_TYPE_COIN)
	    || a->isActorType(ACTOR_TYPE_COIN_BLUE) || a->isActorType(ACTOR_TYPE_COIN_RED))
		return false;
	if (a->getName() == nullptr || a->getName()[0] == '\0')
		return false;
	return propSize(a) <= kPropMaxSize; // no walls, buildings or other big things
}

u16 nameKey(TLiveActor* a) { return JDrama::TNameRef::calcKeyCode(a->getName()); }

TLiveActor* findProp(u32 type, u16 key)
{
	TLiveActor* found = nullptr;
	forEachModelActor([&](TLiveActor* a) {
		if (found == nullptr && a->getActorType() == type && nameKey(a) == key)
			found = a;
	});
	return found;
}

bool propModel(Look& l, u32 type, u16 key)
{
	if (l.prop != nullptr && l.propType == type && l.propKey == key)
		return true;
	l.prop     = nullptr;
	l.propType = type;
	l.propKey  = key;
	TLiveActor* a = findProp(type, key);
	if (a == nullptr)
		return false;
	{
		StageHeapScope scope;
		l.prop = newModel(a->getMActor()->getModel()->getModelData());
	}
	if (l.prop == nullptr)
		return false;
	const JGeometry::TVec3<f32>& r = a->mRotation;
	const JGeometry::TVec3<f32>& s = a->mScaling;
	Mtx rot, sc;
	MsMtxSetRotRPH(rot, r.x, r.y, r.z);
	MTXScale(sc, s.x, s.y, s.z);
	MTXConcat(rot, sc, l.propRot);
	// the size water has to hit: its hit radius, or its model's bounding radius
	J3DJoint* root = a->getMActor()->getModel()->getModelData()->getJointNodePointer(0);
	const f32 big  = s.x > s.y ? (s.x > s.z ? s.x : s.z) : (s.y > s.z ? s.y : s.z);
	f32 rad        = root != nullptr ? root->getRadius() * big : 0.0f;
	if (a->mDamageRadius > rad)
		rad = a->mDamageRadius;
	if (a->mAttackRadius > rad)
		rad = a->mAttackRadius;
	l.propRadius = rad < 50.0f ? 50.0f : (rad > 500.0f ? 500.0f : rad);
	l.propHeight = l.propRadius * 1.6f < 80.0f ? 80.0f : l.propRadius * 1.6f;
	return true;
}

s16 lightId(const f32 pos[3])
{
	const JGeometry::TVec3<f32> p(pos[0], pos[1], pos[2]);
	if (gpCubeShadow != nullptr && gpCubeShadow->getInCubeNo(p) != -1)
		return 1;
	if (gpMap == nullptr)
		return 0;
	const TBGCheckData* plane = nullptr;
	gpMap->checkGround(p.x, p.y + 80.0f, p.z, &plane);
	if (plane == nullptr || !plane->isShadow())
		return 0;
	const f32 groundY = -(p.x * plane->mNormal.x + p.z * plane->mNormal.z + plane->mPlaneDistance) / plane->mNormal.y;
	return groundY + 200.0f > p.y ? (plane->getData() < 0 ? 0 : plane->getData()) : 0;
}

// What a player looks like now (look slot kLocal = us): 1 Shadow Mario, 2 a disguise, 0 Mario.
int lookMode(int slot)
{
	if (port_mp_manhunt_running()) { // Manhunt's hunters are Shadow Mario too
		const int s = slot == kLocal ? me() : slot;
		return port_mp_manhunt_shadow(s) && !(sKage.tried && !sKage.ok) ? 1 : 0;
	}
	if (!running() || !roundOn())
		return 0;
	const int s = slot == kLocal ? me() : slot;
	if (s < 0 || s >= PORT_MP_MAX_PLAYERS || sState.role[s] != R_HIDER)
		return 0;
	if (sState.propType[s] != 0)
		return 2;
	return sKage.tried && !sKage.ok ? 0 : 1;
}

// ---- every client: the round ---------------------------------------------------------------------
void toastChanges()
{
	const int m = me();
	char t[160], nm[32];
	if (sState.round != sSeen.round && sState.phase == PP_HIDE && m >= 0) {
		if (sState.role[m] == R_HUNTER)
			std::snprintf(t, sizeof(t), "Prop Hunt: you hunt. Wait %d s...", secondsLeft());
		else
			std::snprintf(t, sizeof(t), "Prop Hunt: hide! Press R next to an object to become it. (%d s)", secondsLeft());
		port_mp_toast(t);
	}
	if (sState.phase == PP_HUNT && sSeen.phase != PP_HUNT)
		port_mp_toast(myRole() == R_HUNTER ? "Hunt! Spray the props that are players (your water runs out fast)."
		                                   : "The hunters are coming!");
	if (roundOn() && sSeen.phase == sState.phase)
		for (int i = 0; i < PORT_MP_MAX_PLAYERS; i++)
			if (sSeen.role[i] == R_HIDER && sState.role[i] != R_HIDER && sState.role[i] != R_NONE) {
				if (i == m)
					std::snprintf(t, sizeof(t), sState.role[i] == R_HUNTER ? "You were caught: now you hunt!" : "You were caught: you are out.");
				else
					std::snprintf(t, sizeof(t), "%s was caught%s.", nameOf(i, nm, sizeof(nm)),
					              sState.role[i] == R_HUNTER ? " and hunts now" : "");
				port_mp_toast(t);
			}
	if (sState.phase == PP_OVER && sSeen.phase != PP_OVER) {
		std::snprintf(t, sizeof(t), sState.winner == 2 ? "Every hider was caught: the hunters win!"
		                                               : "Time is up: the hiders win! (%d left)",
		              count(R_HIDER));
		port_mp_toast(t);
		std::fprintf(stderr, "[mp-prop] %s\n", t);
	}
	sSeen = sState;
}

void sendHost(const void* msg, int n, void (*hostFn)(const void*))
{
	if (port_mp_is_host())
		hostFn(msg);
	else
		port_mp_send(kChannel, msg, n, 1, -1);
}

// The hiders' Shadow Mario sound: every <host setting> seconds of the hunt, every client plays it at
// each hider's position (our own Mario, or his puppet in our stage). Timed from the shared round clock,
// so no message is needed and everyone hears the same hiders at the same moment.
void tauntFrame()
{
	static int sLastTick = -1;
	static u8 sRound     = 0xFF;
	const int every      = port_mp_prop_taunt_seconds();
	if (sState.phase != PP_HUNT || every <= 0 || SMSGetMSound() == nullptr) {
		sLastTick = -1;
		return;
	}
	int hideS, huntS, convert, pick, damage, mul;
	port_mp_prop_settings(&hideS, &huntS, &convert, &pick, &damage, &mul);
	const int elapsed = huntS - secondsLeft();
	const int tick    = elapsed / every;
	if (sRound != sState.round) {
		sRound    = sState.round;
		sLastTick = tick; // no sound for ticks already past when we joined / the round began
		return;
	}
	if (sLastTick < 0 || tick <= sLastTick) {
		if (sLastTick < 0)
			sLastTick = tick;
		return;
	}
	sLastTick = tick;
	static Vec sPos[PORT_MP_MAX_PLAYERS]; // the sound engine keeps the position pointer while it plays
	int played = 0;
	for (int i = 0; i < PORT_MP_MAX_PLAYERS; i++) {
		if (sState.role[i] != R_HIDER)
			continue;
		float p[3];
		if (i == me()) {
			if (gpMarioOriginal == nullptr)
				continue;
			p[0] = gpMarioOriginal->mPosition.x, p[1] = gpMarioOriginal->mPosition.y, p[2] = gpMarioOriginal->mPosition.z;
		} else if (!port_mp_same_stage(i) || !port_mp_puppet_pos(i, p)) {
			continue;
		}
		sPos[i] = { p[0], p[1] + 80.0f, p[2] };
		SMSGetMSound()->startSoundActor(MSD_SE_MA_KAGE_FIELD_APPEAR, &sPos[i], 0, nullptr, 0, 4);
		played++;
	}
	if (myRole() == R_HIDER)
		port_mp_toast("You let out a Shadow Mario sound: the hunters can hear where you are!");
	std::fprintf(stderr, "[mp-prop] taunt at %d s of the hunt: %d hider sound(s)\n", elapsed, played);
}

void watchDeath()
{
	static int sLastState = -1;
	const int st = gpMarDirector != nullptr ? gpMarDirector->mState : -1;
	const bool died = st == TMarDirector::STATE_UNK7 && sLastState != TMarDirector::STATE_UNK7;
	sLastState = st;
	if (!died || !roundOn() || myRole() != R_HIDER)
		return;
	const u8 msg = MSG_DIED;
	sendHost(&msg, 1, [](const void*) { hostDied(port_mp_local_slot()); });
}

bool localPlayable(TMario* m)
{
	return m != nullptr && gpMarDirector != nullptr && gpMarDirector->mState == TMarDirector::STATE_UNK4
	       && !m->checkFlag(MARIO_FLAG_GAME_OVER);
}

// A hider: no FLUDD; R next to an object takes its shape, R away from objects drops it.
void hiderFrame(TMario* m)
{
	if (myRole() != R_HIDER || !roundOn())
		return; // (the stage reload at the round's end, or a caught hider's, gives FLUDD back)
	if (m->checkFlag(MARIO_FLAG_HAS_FLUDD))
		m->offFlag(MARIO_FLAG_HAS_FLUDD);
	// SMS_PORT_MP_PROP_TEST=1 (tests): disguise as the nearest object once the hide phase starts
	static const bool sTest = std::getenv("SMS_PORT_MP_PROP_TEST") != nullptr;
	static u8 sTestRound    = 0xFF;
	const bool testNow      = sTest && sTestRound != sState.round && sState.phase == PP_HIDE && secondsLeft() < 25;
	// R: the analog trigger pulled a third of the way (the pad's R trigger edge is already used up by
	// the time this runs: it never shows here), or the digital click
	static bool sAnalogDown;
	const bool analog  = m->mGamePad != nullptr && m->mGamePad->mButton.mAnalogRf > (sAnalogDown ? 0.15f : 0.3f);
	const bool press   = analog && !sAnalogDown;
	sAnalogDown        = analog;
	const bool pressed = (m->mGamePad != nullptr && m->mGamePad->testTrigger(PAD_TRIGGER_R)) || press || testNow;
	if (!pressed)
		return;
	if (testNow)
		sTestRound = sState.round;
	TLiveActor* best = nullptr;
	f32 bestD        = 600.0f * 600.0f;
	forEachModelActor([&](TLiveActor* a) {
		if (!propCandidate(a))
			return;
		const f32 dx = a->mPosition.x - m->mPosition.x, dz = a->mPosition.z - m->mPosition.z;
		const f32 dy = a->mPosition.y - m->mPosition.y;
		const f32 d  = dx * dx + dz * dz;
		if (d < bestD && dy > -300.0f && dy < 300.0f) {
			bestD = d;
			best  = a;
		}
	});
	if (testNow && best == nullptr) { // tests: anything in the stage
		bestD = 1e30f;
		forEachModelActor([&](TLiveActor* a) {
			if (!propCandidate(a))
				return;
			const f32 dx = a->mPosition.x - m->mPosition.x, dz = a->mPosition.z - m->mPosition.z;
			if (dx * dx + dz * dz < bestD) {
				bestD = dx * dx + dz * dz;
				best  = a;
			}
		});
	}
	PropMsg msg = { MSG_PROP, 0, best != nullptr ? nameKey(best) : (u16)0, best != nullptr ? best->getActorType() : 0u };
	const int s = me();
	if (best == nullptr && s >= 0 && s < PORT_MP_MAX_PLAYERS && sState.propType[s] == 0) {
		TLiveActor* near = nullptr; // (log) the closest one anywhere
		f32 nearD        = 1e30f;
		forEachModelActor([&](TLiveActor* a) {
			const f32 dx = a->mPosition.x - m->mPosition.x, dz = a->mPosition.z - m->mPosition.z;
			if (propCandidate(a) && dx * dx + dz * dz < nearD) {
				nearD = dx * dx + dz * dz;
				near  = a;
			}
		});
		if (near != nullptr)
			std::fprintf(stderr, "[mp-prop] R: nothing within reach; closest %s at %.0f %.0f %.0f (%.0f away, size %.0f)\n",
			             near->getName(), near->mPosition.x, near->mPosition.y, near->mPosition.z, std::sqrt(nearD),
			             propSize(near));
		port_mp_toast("No object close enough to become: stand right next to one and press R.");
		return;
	}
	if (s >= 0 && s < PORT_MP_MAX_PLAYERS && msg.actorType == sState.propType[s] && msg.key == sState.propKey[s])
		return;
	std::fprintf(stderr, "[mp-prop] R: %s %s (size %.0f)\n", best != nullptr ? "become" : "drop the disguise",
	             best != nullptr ? best->getName() : "", best != nullptr ? propSize(best) : 0.0f);
	if (port_mp_is_host())
		hostProp(s, msg.actorType, msg.key);
	else
		port_mp_send(kChannel, &msg, (int)sizeof(msg), 1, -1);
}

// A hunter: the tank empties faster (each frame's use counted `mul` times).
void hunterWater(TMario* m)
{
	static s32 sLast = -1;
	if (myRole() != R_HUNTER || !roundOn() || m->mWaterGun == nullptr) {
		sLast = -1;
		return;
	}
	int hideS, huntS, convert, pick, damage, mul;
	port_mp_prop_settings(&hideS, &huntS, &convert, &pick, &damage, &mul);
	TWaterGun* g = m->mWaterGun;
	if (sLast >= 0 && g->mCurrentWater < sLast && mul > 1) {
		const s32 extra = (sLast - g->mCurrentWater) * (mul - 1);
		g->mCurrentWater = g->mCurrentWater > extra ? g->mCurrentWater - extra : 0;
	}
	sLast = g->mCurrentWater;
}

int sSprayCooldown[PORT_MP_MAX_PLAYERS];

// A hunter's water reaching a hider as drawn (Shadow Mario, or his disguise's size).
void hunterSpray(TMario* m)
{
	for (int i = 0; i < PORT_MP_MAX_PLAYERS; i++)
		if (sSprayCooldown[i] > 0)
			sSprayCooldown[i]--;
	if (myRole() != R_HUNTER || sState.phase != PP_HUNT || gpModelWaterManager == nullptr)
		return;
	int hideS, huntS, convert, pick, damage, mul;
	port_mp_prop_settings(&hideS, &huntS, &convert, &pick, &damage, &mul);
	// SMS_PORT_MP_PROP_TEST=1 (tests): a hunter hits every hider in his stage every 2 s (the
	// message and damage path without aiming water)
	static const bool sTest = std::getenv("SMS_PORT_MP_PROP_TEST") != nullptr;
	static double sTestAt;
	if (sTest && port_mp_time_ms() - sTestAt > 2000.0) {
		sTestAt = port_mp_time_ms();
		for (int s = 0; s < PORT_MP_MAX_PLAYERS; s++)
			if (s != me() && sState.role[s] == R_HIDER && port_mp_same_stage(s)) {
				SprayMsg msg = { MSG_SPRAY, (u8)s, (u8)damage, 0, { m->mPosition.x, m->mPosition.y, m->mPosition.z } };
				port_mp_send(kChannel, &msg, (int)sizeof(msg), 1, s);
				std::fprintf(stderr, "[mp-prop] test: spray hit sent to slot %d (damage %d)\n", s, damage);
			}
	}
	const JGeometry::TVec3<f32>* pp = gpModelWaterManager->getParticlePositions();
	const f32 wr = TModelWaterManager::mStaticHitActor.getAttackRadius();
	const f32 wh = TModelWaterManager::mStaticHitActor.getAttackHeight();
	const int nParticles = gpModelWaterManager->getParticleCount();
	for (int s = 0; s < PORT_MP_MAX_PLAYERS; s++) {
		if (s == me() || sState.role[s] != R_HIDER || sSprayCooldown[s] > 0 || !port_mp_same_stage(s))
			continue;
		f32 pos[3];
		if (!port_mp_puppet_pos(s, pos))
			continue;
		const Look& l = sLooks.look[s];
		const f32 radius = l.mode == 2 && l.prop != nullptr ? l.propRadius : 50.0f;
		const f32 height = l.mode == 2 && l.prop != nullptr ? l.propHeight : 160.0f;
		for (int i = 0; i < nParticles; i++) {
			if (!gpModelWaterManager->checkFlagBottom4Bits(i, 0x1) || gpModelWaterManager->portIsRemote(i))
				continue;
			const JGeometry::TVec3<f32>& q = pp[i];
			if (q.y > pos[1] + height || q.y + wh < pos[1])
				continue;
			const f32 qx = q.x - pos[0], qz = q.z - pos[2];
			if (qx * qx + qz * qz >= (wr + radius) * (wr + radius))
				continue;
			SprayMsg msg = { MSG_SPRAY, (u8)s, (u8)damage, 0, { m->mPosition.x, m->mPosition.y, m->mPosition.z } };
			port_mp_send(kChannel, &msg, (int)sizeof(msg), 1, s);
			sSprayCooldown[s] = 30;
			char nm[32];
			std::fprintf(stderr, "[mp-prop] sprayed %s (damage %d)\n", nameOf(s, nm, sizeof(nm)), damage);
			break;
		}
	}
}

// Stand-in for damageExec's knock-back direction (never registered with the hit check).
class TPropHitProxy : public THitActor {
public:
	TPropHitProxy()
	    : THitActor("mp prop hunt spray")
	{
	}
};
TPropHitProxy sProxy;

void applySpray(int from, const SprayMsg& msg)
{
	TMario* m = gpMarioOriginal;
	if (sState.phase != PP_HUNT || myRole() != R_HIDER || !localPlayable(m) || !port_mp_same_stage(from))
		return;
	if (m->mStatus == MARIO_STATUS_NOMOTION) // an emote's demo status refuses the knock-back
		m->changePlayerStatus(MARIO_STATUS_WAIT, 0, true);
	sProxy.mPosition.set(msg.pos[0], msg.pos[1], msg.pos[2]);
	const int before = m->mHealth;
	m->damageExec(&sProxy, msg.damage ? msg.damage : 2, 0, 0, 10.0f, 4, 0.0f, 30);
	std::fprintf(stderr, "[mp-prop] sprayed by slot %d: health %d -> %d\n", from, before, (int)m->mHealth);
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
		} else if (buf[0] == MSG_PROP && n == (int)sizeof(PropMsg) && host) {
			const PropMsg* p = (const PropMsg*)buf;
			hostProp(from, p->actorType, p->key);
		} else if (buf[0] == MSG_DIED && n == 1 && host) {
			hostDied(from);
		} else if (buf[0] == MSG_SPRAY && n == (int)sizeof(SprayMsg)) {
			const SprayMsg* s = (const SprayMsg*)buf;
			if (s->victim == me())
				applySpray(from, *s);
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
	tauntFrame();
	TMario* m = gpMarioOriginal;
	if (localPlayable(m)) {
		hiderFrame(m);
		hunterWater(m);
		hunterSpray(m);
	}
	// hunters' pads stay idle while the hiders hide
	const bool blind = sState.phase == PP_HIDE && myRole() == R_HUNTER;
	if (blind != sBlind) {
		sBlind = blind;
		port_mp_request_input(blind ? 1 : 0);
	}
	if (running() && roundOn())
		loadKageFiles(); // once, early (not in the middle of drawing)
}

struct Register {
	Register() { port_mp_register_game_frame(gameFrame); }
} sRegister;

// Pose and draw one look. body: the posed Mario (Shadow Mario copies its joints).
void lookCalc(Look& l, J3DModel* body)
{
	l.drawn = false;
	if (l.mode == 1) {
		if (body == nullptr || !kageModel(l))
			return;
		J3DModel* k = l.kage;
		k->setBaseTRMtx(body->getBaseTRMtx());
		k->calc();
		const u16 joints = body->getModelData()->getJointNum() < k->getModelData()->getJointNum()
		                       ? body->getModelData()->getJointNum()
		                       : k->getModelData()->getJointNum();
		for (u16 i = 0; i < joints; i++)
			k->setAnmMtx(i, body->getAnmMtx(i));
		k->calcWeightEnvelopeMtx();
		if (l.brush != nullptr) {
			if (sLooks.handL == 0)
				sLooks.handL = (u16)body->getModelData()->getJointName()->getIndex("jnt_hand_L");
			if (sLooks.handL < joints) {
				l.brush->setBaseTRMtx(k->getAnmMtx(sLooks.handL));
				l.brush->calc();
			}
		}
		l.drawn = true;
	} else if (l.mode == 2 && l.prop != nullptr) {
		Mtx base;
		MTXCopy(l.propRot, base);
		base[0][3] = l.pos[0];
		base[1][3] = l.pos[1];
		base[2][3] = l.pos[2];
		l.prop->setBaseTRMtx(base);
		l.prop->calc();
		l.drawn = true;
	}
}

void lookView(Look& l)
{
	if (!l.drawn)
		return;
	if (l.mode == 1) {
		l.kage->viewCalc();
		if (l.brush != nullptr)
			l.brush->viewCalc();
	} else {
		l.prop->viewCalc();
	}
}

void lookEntry(Look& l)
{
	if (!l.drawn)
		return;
	TLightWithDBSet* set = gpLightManager != nullptr ? gpLightManager->getLightSet(1) : nullptr;
	if (set != nullptr)
		set->changeLightDrawBuffer(lightId(l.pos));
	if (l.mode == 1) {
		l.kage->entry();
		if (l.brush != nullptr)
			l.brush->entry();
	} else {
		l.prop->entry();
	}
	if (set != nullptr)
		set->resetLightDrawBuffer();
}

} // namespace

extern "C" {

int port_mp_prop_running(void) { return running() ? 1 : 0; }

// port_mp_puppet.cpp: 0 = draw this player's Mario, 1 = Shadow Mario (pose the body, draw nothing),
// 2 = a disguise (no Mario at all).
int port_mp_prop_replaces(int slot) { return lookMode(slot); }

// MarioMain.cpp (TMario::perform): 1 = do not draw the local Mario's models (we draw his look).
int port_mp_local_body_hidden(void) { return lookMode(kLocal) != 0 && sLooks.valid && sLooks.serial == gPortMpStageSerial ? 1 : 0; }

// port_mp_puppet.cpp, at the end of its view object's perform: the looks of the hiders and ours.
void port_mp_prop_perform(unsigned cue, void* graphics)
{
	(void)graphics;
	if (!running() && !port_mp_manhunt_running())
		return;
	if (port_mp_manhunt_running())
		loadKageFiles(); // (Prop Hunt loads them in its game frame)
	resetLooksIfNewStage();
	if (cue & CUE_ENTRY) {
		// Shadow Mario's texture scroll (TEMario: MActor::setBtk("kagemario_scroll"))
		if (sLooks.btk != nullptr && sLooks.btkAnm != nullptr && sLooks.kageData != nullptr) {
			sLooks.btkFrame += 1.0f;
			if (sLooks.btk->getFrameMax() > 0 && sLooks.btkFrame >= sLooks.btk->getFrameMax())
				sLooks.btkFrame = 0.0f;
			sLooks.btk->setFrame(sLooks.btkFrame);
			sLooks.kageData->setTexMtxAnimator(sLooks.btk, sLooks.btkAnm, sLooks.btkAnm);
		}
		for (int i = 0; i <= PORT_MP_MAX_PLAYERS; i++) {
			Look& l = sLooks.look[i];
			l.mode  = lookMode(i);
			l.drawn = false;
			if (l.mode == 0)
				continue;
			const int s = i == kLocal ? me() : i;
			J3DModel* body = nullptr;
			if (i == kLocal) {
				TMario* m = gpMarioOriginal;
				if (m == nullptr || m->mModel == nullptr)
					continue;
				body     = m->mModel->getModel();
				l.pos[0] = m->mPosition.x;
				l.pos[1] = m->mPosition.y;
				l.pos[2] = m->mPosition.z;
			} else {
				if (!port_mp_same_stage(i) || !port_mp_puppet_pos(i, l.pos))
					continue;
				body = (J3DModel*)port_mp_puppet_body(i);
			}
			if (l.mode == 2 && !propModel(l, sState.propType[s], sState.propKey[s]))
				l.mode = 1; // the object is not in this client's stage: Shadow Mario
			lookCalc(l, body);
		}
	}
	if (cue & CUE_CALC_VIEW)
		for (int i = 0; i <= PORT_MP_MAX_PLAYERS; i++)
			lookView(sLooks.look[i]);
	if (cue & CUE_ENTRY)
		for (int i = 0; i <= PORT_MP_MAX_PLAYERS; i++)
			lookEntry(sLooks.look[i]);
}

// Name tags and off-screen markers: 1 = do not show this player to us (a hider, to a hunter).
int port_mp_prop_hide_tag(int slot)
{
	const int m = me();
	return running() && roundOn() && m >= 0 && slot >= 0 && slot < PORT_MP_MAX_PLAYERS && sState.role[m] != R_HIDER
	       && sState.role[slot] == R_HIDER;
}

// The player list: the black screen of a hunter while the others hide.
int port_mp_prop_blind(void) { return sBlind ? 1 : 0; }

// port_mp_arena.cpp: 1 = we are a hider in a running round (the exit lock holds hiders).
int port_mp_prop_local_hider(void) { return roundOn() && myRole() == R_HIDER ? 1 : 0; }

int port_mp_prop_headline(char* out, int cap)
{
	const int left = secondsLeft();
	switch (sState.phase) {
	case PP_HIDE: std::snprintf(out, cap, "Prop Hunt: hide!  %d:%02d", left / 60, left % 60); return 1;
	case PP_HUNT:
		std::snprintf(out, cap, "Prop Hunt: hunt  %d:%02d  (%d hiding)", left / 60, left % 60, count(R_HIDER));
		return 1;
	case PP_OVER: std::snprintf(out, cap, "Prop Hunt: %s win", sState.winner == 2 ? "hunters" : "hiders"); return 1;
	default: return 0;
	}
}

const char* port_mp_prop_role(int slot)
{
	if (sState.phase == PP_OFF || slot < 0 || slot >= PORT_MP_MAX_PLAYERS)
		return nullptr;
	switch (sState.role[slot]) {
	case R_HIDER: return "Hider";
	case R_HUNTER: return "Hunter";
	case R_OUT: return "Out";
	default: return nullptr;
	}
}

} // extern "C"

#endif // SMS_PORT_AURORA
