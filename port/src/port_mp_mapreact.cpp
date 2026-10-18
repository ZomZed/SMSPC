// Multiplayer map-object reactions (w11): water on the plaza's hanging signs (TMapObjBillboard), the
// rooster weathervane (TMapObjTurn, DPT_WEATHERCOCK) and the "spray here" coin spots
// (TWaterHitHideObj) runs on every screen, and the coin such an object drops exists once.
//
// Remote FLUDD water is visual only (w15), so only the sprayer's game used to react: the sign
// swung, the vane spun and the coin dropped on one screen. Now:
//  - Our water landing on one of these objects (the hook in TModelWaterManager, port_mp_obj.h) is
//    counted per object and sent every 3 logic frames to everyone in the stage + scenario
//    (PORT_MP_CH_MAPREACT = 21, reliable): id, number of water hits, the last particle's
//    position. Each receiver applies the same number of water hits to its copy, through the
//    object's own receiveMessage, so its reaction is the game's own (swing direction from the
//    particle position, the vane's speed-up per hit, the reveal).
//  - Coins: each client's reveal throws a coin from the item manager's run-time pool. Every
//    frame, a reveal (the object's "hidden coin" flag going off) is matched to the pool coin that
//    appeared in that frame nearest to it, which links that coin to the object. When the local
//    Mario takes a linked coin (TCoin::taken hook), TAKEN with the object's id goes to the
//    others, who remove their linked coin (or remove it as soon as it appears, if their reveal
//    comes later). The coin total is w8's run-time coin credit (port_mp_objectives_extra.cpp,
//    channel 19, with shared objectives on): one take, one credit.
// Ids as w10's/w11's/w15's: (manager index in TConductor's list) << 16 | object index.
// Only objects reviewed here are forwarded; w15 owns nozzle boxes, the red X, sewer grates,
// fruit and birds, w10 the world events. Nothing runs outside a session or alone in a stage.
//
// Debug (SMS_PORT_MP_MAPREACT_TEST="<name>:<action>@<frame>;...", frames since boot):
//   sign / vane / spot <n>  n water hits on the nearest sign / weathervane / coin spot, sent as if
//                           our water had landed (default n = 1)
//   take                    our Mario takes the nearest linked coin
#ifdef SMS_PORT_AURORA

#include "port_mp.h"
#include "port_mp_obj.h"

#include <Enemy/Conductor.hpp>
#include <MoveBG/Item.hpp>
#include <MoveBG/ItemManager.hpp>
#include <MoveBG/MapObjHide.hpp>
#include <MoveBG/MapObjTurn.hpp>
#include <Player/Mario.hpp>
#include <Player/ModelWaterManager.hpp>
#include <Strategic/ActorTypes.hpp>
#include <Strategic/LiveManager.hpp>
#include <Strategic/ObjHitCheck.hpp>
#include <System/Application.hpp>
#include <System/MarDirector.hpp>
#include <JSystem/JDrama/JDRNameRefGen.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

extern "C" unsigned port_frame_count(void); // port_crash.cpp

// Read by the hooks (port_mp_obj.h): map-object reactions are forwarded (session, others here).
extern "C" int gPortMpMapReact = 0;

namespace {

const uint8_t kChannel = 21; // PORT_MP_CH_MAPREACT: 21, kept for w11 in port_mp.h (w15 has 22)

enum MsgType : u8 { MSG_WATER = 1, MSG_TAKEN = 2 };
enum Kind : u8 { K_SIGN = 1, K_VANE = 2, K_SPOT = 3 };

#pragma pack(push, 1)
struct Msg {
	u8 type;
	u8 stage, scenario;
	u8 count; // WATER: hits
	u32 id;
	float pos[3]; // WATER: last particle
};
#pragma pack(pop)

struct Obj {
	THideObjBase* a;
	u32 id;
	u8 kind;
	bool avail;     // still holds its coin
	u32 pending;    // water hits not sent yet
	float pendPos[3];
	bool takenRemote; // someone took this object's coin
};

std::vector<Obj> sObjs;
std::unordered_map<u32, int> sById;
std::unordered_map<void*, int> sByActor;
std::unordered_map<void*, u32> sCoinLink; // coin -> object id
std::vector<u8> sCoinAlive;               // per item-manager index
TMarDirector* sVisitDir;
unsigned sVisitDirSerial; // gPortMpStageSerial of this visit
int sVisitStage = -1, sVisitScenario = -1;
int sFrames;
bool sBuilt;
bool sApplying;
int sStatSent, sStatApplied, sStatLinked, sStatRemoved;

bool logOn()
{
	static const bool sLog = std::getenv("SMS_PORT_MP_OBJ_LOG") != nullptr;
	return sLog;
}

TConductor* conductor()
{
	JDrama::TNameRefGen* gen = JDrama::TNameRefGen::getInstance();
	if (gpConductor == nullptr || gen == nullptr || gen->getRootNameRef() == nullptr)
		return nullptr;
	JDrama::TNameRef* found = gen->getRootNameRef()->search("\x83\x52\x83\x93\x83\x5F\x83\x4E\x83\x5E\x81\x5B");
	return found == (JDrama::TNameRef*)gpConductor ? gpConductor : nullptr;
}

// The reviewed classes, by the object's map-object data name (TMapObjBase::unkF4).
u8 kindOf(TLiveActor* a)
{
	// Both categories are TMapObjBase. The plaza's water-hit coin spots (open windows,
	// "WaterHitHideObj") are in the item category, not the map-object one.
	if (a->getHitCategory() != HIT_CATEGORY_MAP_OBJECT && a->getHitCategory() != HIT_CATEGORY_ITEM)
		return 0;
	const char* n = ((TMapObjBase*)a)->getUnkF4();
	if (n == nullptr)
		return 0;
	if (std::strncmp(n, "billboard", 9) == 0)
		return K_SIGN; // TMapObjBillboard ("Billboard")
	if (a->isActorType(ACTOR_TYPE_DPT_WEATHERCOCK))
		return K_VANE; // TMapObjTurn ("DolWeathercock")
	if (std::strcmp(n, "WaterHitHideObj") == 0)
		return K_SPOT; // TWaterHitHideObj
	// Water-hit pictures (TWaterHitPictureHideObj): Shadow Mario posters and the painted
	// signs. The water is forwarded so the picture fades the same way on every screen and its
	// coin is linked like a coin spot's (port_mp_events.cpp EV_PICTURE only finishes it).
	if (std::strcmp(n, "KoopaJrSignM") == 0 || std::strcmp(n, "SignCross") == 0
	    || std::strcmp(n, "SignCircle") == 0 || std::strcmp(n, "SignTriangle") == 0)
		return K_SPOT;
	return 0;
}

bool alive(const TLiveActor* a) { return (a->mLiveFlag & LIVE_FLAG_DEAD) == 0; }

bool holdsCoin(const Obj& o)
{
	THideObjBase* h = o.a;
	if (h->mHiddenObj == nullptr || !h->mHiddenObj->isActorType(ACTOR_TYPE_COIN))
		return false;
	if (o.kind == K_VANE)
		return ((TMapObjTurn*)h)->unk168 != 0;
	return h->mAllowReveal && alive(h);
}

const char* kindName(u8 k) { return k == K_SIGN ? "sign" : k == K_VANE ? "weathervane" : "coin spot"; }

void reset()
{
	sObjs.clear();
	sById.clear();
	sByActor.clear();
	sCoinLink.clear();
	sCoinAlive.clear();
	sBuilt = false;
}

void buildList()
{
	reset();
	sBuilt = true;
	TConductor* c = conductor();
	if (c == nullptr)
		return;
	int mgr = 0;
	for (JGadget::TList<TLiveManager*>::iterator it = c->unk10.begin(), e = c->unk10.end(); it != e; ++it, ++mgr) {
		TLiveManager* m = *it;
		if (m == nullptr)
			continue;
		for (int i = 0; i < m->getObjNum(); i++) {
			TLiveActor* a = m->getObj(i);
			if (a == nullptr)
				continue;
			const u8 k = kindOf(a);
			if (k == 0)
				continue;
			Obj o;
			std::memset(&o, 0, sizeof(o));
			o.a    = (THideObjBase*)a;
			o.id   = ((u32)mgr << 16) | (u32)i;
			o.kind = k;
			o.avail = holdsCoin(o);
			sById[o.id]  = (int)sObjs.size();
			sByActor[a]  = (int)sObjs.size();
			sObjs.push_back(o);
		}
	}
	std::fprintf(stderr, "[mp-react] stage %d/%d: %d map objects react on every screen\n", sVisitStage, sVisitScenario,
	             (int)sObjs.size());
}

void send(const Msg& m) { port_mp_send(kChannel, &m, (int)sizeof(m), 1, -1); }

Msg header(u8 type, u32 id)
{
	Msg m;
	std::memset(&m, 0, sizeof(m));
	m.type     = type;
	m.stage    = (u8)sVisitStage;
	m.scenario = (u8)sVisitScenario;
	m.id       = id;
	return m;
}

void removeCoin(void* coin, u32 id, const char* why)
{
	TItem* it = (TItem*)coin;
	sCoinLink.erase(coin);
	if (alive(it))
		it->kill();
	sStatRemoved++;
	std::fprintf(stderr, "[mp-react] coin of %08x removed here (%s)\n", id, why);
}

// The same water as the sender's: n hits through the object's own receiveMessage.
void applyWater(Obj& o, int n, const float* pos)
{
	auto& w = TModelWaterManager::mStaticHitActor;
	const JGeometry::TVec3<f32> saved = w.mPosition;
	sApplying = true;
	for (int k = 0; k < n; k++) {
		w.mPosition.set(pos[0], pos[1], pos[2]);
		o.a->receiveMessage(&w, HIT_MESSAGE_SPRAYED_BY_WATER);
	}
	sApplying  = false;
	w.mPosition = saved;
	sStatApplied += n;
}

void netPoll()
{
	Msg m;
	int from, n;
	while ((n = port_mp_recv(kChannel, &m, (int)sizeof(m), &from)) != 0) {
		if (n != (int)sizeof(m) || !sBuilt || m.stage != sVisitStage || m.scenario != sVisitScenario)
			continue;
		auto it = sById.find(m.id);
		if (it == sById.end())
			continue;
		Obj& o = sObjs[it->second];
		if (m.type == MSG_WATER) {
			applyWater(o, m.count, m.pos);
			if (logOn())
				std::fprintf(stderr, "[mp-react] %d water hits on %s %08x from slot %d applied\n", m.count,
				             kindName(o.kind), o.id, from);
		} else if (m.type == MSG_TAKEN) {
			o.takenRemote = true;
			std::fprintf(stderr, "[mp-react] coin of %s %08x taken by slot %d\n", kindName(o.kind), o.id, from);
			for (auto& l : sCoinLink)
				if (l.second == o.id) {
					removeCoin(l.first, o.id, "taken by another player");
					break;
				}
		}
	}
}

// Reveals this frame -> the pool coins that appeared this frame (nearest each).
void linkCoins()
{
	if (gpItemManager == nullptr)
		return;
	const int num = gpItemManager->getObjNum();
	if ((int)sCoinAlive.size() != num) {
		sCoinAlive.assign(num, 0);
		for (int i = 0; i < num; i++) {
			TMapObjBase* c = (TMapObjBase*)gpItemManager->getObj(i);
			sCoinAlive[i] = c != nullptr && c->isActorType(ACTOR_TYPE_COIN) && alive(c);
		}
	}
	std::vector<int> fresh;
	for (int i = 0; i < num; i++) {
		TMapObjBase* c = (TMapObjBase*)gpItemManager->getObj(i);
		const u8 al    = c != nullptr && c->isActorType(ACTOR_TYPE_COIN) && alive(c);
		if (al && !sCoinAlive[i])
			fresh.push_back(i);
		sCoinAlive[i] = al;
	}
	for (Obj& o : sObjs) {
		const bool now = holdsCoin(o);
		if (o.avail && !now) {
			int best = -1;
			float bestD = 2000.0f * 2000.0f;
			for (int i : fresh) {
				TMapObjBase* c = (TMapObjBase*)gpItemManager->getObj(i);
				if (sCoinLink.count(c))
					continue;
				const float dx = c->mPosition.x - o.a->mPosition.x, dy = c->mPosition.y - o.a->mPosition.y,
				            dz = c->mPosition.z - o.a->mPosition.z;
				const float d = dx * dx + dy * dy + dz * dz;
				if (d < bestD)
					bestD = d, best = i;
			}
			if (best >= 0) {
				void* c       = gpItemManager->getObj(best);
				sCoinLink[c]  = o.id;
				sStatLinked++;
				std::fprintf(stderr, "[mp-react] %s %08x dropped its coin (item %d)\n", kindName(o.kind), o.id, best);
				if (o.takenRemote)
					removeCoin(c, o.id, "already taken by another player");
			}
		}
		o.avail = now;
	}
	// forget links of coins that are gone (timed out)
	for (auto it = sCoinLink.begin(); it != sCoinLink.end();)
		it = alive((TLiveActor*)it->first) ? std::next(it) : sCoinLink.erase(it);
}

void flushWater()
{
	for (Obj& o : sObjs) {
		while (o.pending > 0) {
			const u32 n = o.pending > 255 ? 255 : o.pending;
			Msg m       = header(MSG_WATER, o.id);
			m.count     = (u8)n;
			std::memcpy(m.pos, o.pendPos, sizeof(m.pos));
			send(m);
			o.pending -= n;
			sStatSent += n;
			if (logOn())
				std::fprintf(stderr, "[mp-react] %u water hits on %s %08x sent\n", n, kindName(o.kind), o.id);
		}
	}
}

int othersHere()
{
	int n = 0;
	for (int i = 0; i < PORT_MP_MAX_PLAYERS; i++)
		if (i != port_mp_local_slot() && port_mp_same_stage(i))
			n++;
	return n;
}

Obj* nearest(u8 kind)
{
	if (gpMarioOriginal == nullptr)
		return nullptr;
	Obj* best   = nullptr;
	float bestD = 1e30f;
	for (Obj& o : sObjs) {
		if (o.kind != kind)
			continue;
		const float dx = o.a->mPosition.x - gpMarioOriginal->mPosition.x,
		            dz = o.a->mPosition.z - gpMarioOriginal->mPosition.z;
		if (dx * dx + dz * dz < bestD)
			bestD = dx * dx + dz * dz, best = &o;
	}
	return best;
}

void testKnob()
{
	static const char* sSpec = std::getenv("SMS_PORT_MP_MAPREACT_TEST");
	static std::string sDone;
	if (sSpec == nullptr)
		return;
	const unsigned f = port_frame_count();
	const char* me   = port_mp_get_name();
	std::string spec = sSpec;
	size_t p         = 0;
	int idx          = 0;
	while (p < spec.size()) {
		size_t q = spec.find(';', p);
		if (q == std::string::npos)
			q = spec.size();
		const std::string item = spec.substr(p, q - p);
		p                      = q + 1;
		const int myIdx        = idx++;
		const size_t c = item.find(':'), at = item.find('@');
		if (c == std::string::npos || at == std::string::npos || item.substr(0, c) != me)
			continue;
		if ((unsigned)std::atoi(item.c_str() + at + 1) > f || sDone.find("," + std::to_string(myIdx) + ",") != std::string::npos)
			continue;
		sDone += "," + std::to_string(myIdx) + ",";
		std::string act = item.substr(c + 1, at - c - 1);
		int n           = 1;
		const size_t sp = act.find(' ');
		if (sp != std::string::npos) {
			n   = std::atoi(act.c_str() + sp + 1);
			act = act.substr(0, sp);
		}
		const u8 k = act == "sign" ? K_SIGN : act == "vane" ? K_VANE : act == "spot" ? K_SPOT : 0;
		if (k != 0) {
			Obj* o = nearest(k);
			if (o == nullptr) {
				std::fprintf(stderr, "[mp-react] test: no %s here\n", act.c_str());
				continue;
			}
			float pos[3] = { o->a->mPosition.x, o->a->mPosition.y + 100.0f, o->a->mPosition.z + 100.0f };
			for (int i = 0; i < n; i++) {
				port_mp_obj_hit(o->a, pos, HIT_MESSAGE_SPRAYED_BY_WATER, 1); // as the water hook
				auto& w = TModelWaterManager::mStaticHitActor;
				w.mPosition.set(pos[0], pos[1], pos[2]);
				o->a->receiveMessage(&w, HIT_MESSAGE_SPRAYED_BY_WATER);
			}
			std::fprintf(stderr, "[mp-react] test: %d water hits on %s %08x here\n", n, kindName(k), o->id);
		} else if (act == "take") {
			for (auto& l : sCoinLink) {
				TCoin* coin = (TCoin*)l.first;
				std::fprintf(stderr, "[mp-react] test: our Mario takes the coin of %08x\n", l.second);
				coin->taken(gpMarioOriginal); // the game's own take (count, sound, the hooks)
				break;
			}
		}
	}
}

void gameFrame()
{
	TApplication* app  = SMSGetApplication();
	const int stage    = app->mCurrArea.getStage();
	const int scenario = app->mCurrArea.getScenario();
	if ((gpMarDirector != sVisitDir || gPortMpStageSerial != sVisitDirSerial) || stage != sVisitStage || scenario != sVisitScenario) {
		sVisitDir      = gpMarDirector;
		sVisitDirSerial = gPortMpStageSerial;
		sVisitStage    = stage;
		sVisitScenario = scenario;
		sFrames        = 0;
		reset();
	} else {
		sFrames++;
	}
	if (!sBuilt && sFrames >= 2 && gpMarDirector != nullptr && gpMarDirector->mState == TMarDirector::STATE_UNK4)
		buildList();
	gPortMpMapReact = sBuilt && !sObjs.empty() && othersHere() > 0;
	netPoll();
	if (!sBuilt)
		return;
	testKnob();
	linkCoins();
	if (sFrames % 3 == 0)
		flushWater();
}

struct Register {
	Register() { port_mp_register_game_frame(gameFrame); }
} sRegister;

} // namespace

// port_mp_objects.cpp, port_mp_obj_hit (the water / Mario hit hooks, port_mp_obj.h): our water
// landed on `target`. Counted here when it is one of our reacting map objects.
extern "C" void port_mp_mapreact_hit(void* target, const float* senderPos, unsigned message, int water)
{
	if (!gPortMpMapReact || sApplying || !water || message != HIT_MESSAGE_SPRAYED_BY_WATER
	    || (gpMarDirector != sVisitDir || gPortMpStageSerial != sVisitDirSerial))
		return;
	auto it = sByActor.find(target);
	if (it == sByActor.end())
		return;
	Obj& o = sObjs[it->second];
	o.pending++;
	std::memcpy(o.pendPos, senderPos, sizeof(o.pendPos));
}

// Item.cpp, TCoin::taken: the local Mario took `coin`. One that an object of ours dropped is
// removed on the other screens.
extern "C" void port_mp_mapreact_coin_taken(void* coin)
{
	if (!sBuilt || (gpMarDirector != sVisitDir || gPortMpStageSerial != sVisitDirSerial))
		return;
	auto it = sCoinLink.find(coin);
	if (it == sCoinLink.end())
		return;
	const u32 id = it->second;
	sCoinLink.erase(it);
	send(header(MSG_TAKEN, id));
	std::fprintf(stderr, "[mp-react] coin of %08x taken here: sent\n", id);
}

#endif // SMS_PORT_AURORA
