// Multiplayer world events (w10, task board P8 "EVENTS"; design: port/MULTIPLAYER.md
// "World events (P8 EVENTS)"). Host setting "Sync world events" (reserved[6], off by default).
//
// One-off changes a player makes to a stage's objects (pound switches, the red-coin switch,
// Ricco's fruit switch, nozzle boxes, brick blocks, super-hip-drop slabs) are sent as
// events; the host keeps the first claim for each (object, kind, n-th time) per stage visit and
// broadcasts it; every other player in that stage + scenario replays it on its own copy of the
// object through the object's own reaction (the hit message it reacts to, with a stand-in
// Mario that ignores the replies, or the same kill), never through a reward path for the local
// Mario. Late joiners and players entering the stage get the visit's events from the host and
// replay them once the stage is loaded. Ids: w11's P8 scheme, (manager index in TConductor's
// list) << 16 | (object index in that manager).
//
// Game hooks (SMS_PORT_AURORA, inert unless a session is active and the setting is on):
// port_mp_event_local() where those objects react (Item.cpp, MapObjTown.cpp, MapObjRicco.cpp,
// MapObjBlock.cpp) and port_mp_event_applying() in TMarDirector::fireStartDemoCamera (a replayed
// switch does not take the other players' cameras).
#ifdef SMS_PORT_AURORA

#include "port_mp.h"

#include <Enemy/Conductor.hpp>
#include <MoveBG/Item.hpp>
#include <MoveBG/ItemManager.hpp>
#include <MoveBG/MapObjBase.hpp>
#include <Player/Mario.hpp>
#include <Strategic/ActorTypes.hpp>
#include <Strategic/HitActor.hpp>
#include <Strategic/LiveManager.hpp>
#include <System/MarDirector.hpp>
#include <System/MarioGamePad.hpp>
#include <JSystem/JDrama/JDRNameRefGen.hpp>

#define IMGUI_USER_CONFIG "aurora/imgui_config.h" // as Aurora builds imgui
#include <imgui.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <new>
#include <vector>

extern "C" void port_mp_toast(const char* text); // port_mp_menu.cpp
extern "C" void port_mp_event_local(void* obj, void* sender, int kind);
extern "C" void port_mp_manhole_open(void* obj); // src/MoveBG/MapObjTown.cpp (w15)
extern "C" void port_mp_picture_clear(void* obj); // src/MoveBG/MapObjHide.cpp (w15)

// Event kinds (also used by the game hooks: keep the numbers).
enum PortMpEventKind {
	EV_SWITCH        = 1, // TMapObjSwitch: hip drop (blue-coin / hidden-object switch)
	EV_REDCOIN       = 2, // TRedCoinSwitch: hip drop
	EV_FRUIT_SWITCH  = 3, // TFruitSwitch (Ricco): hip drop
	EV_NOZZLE_BOX    = 4, // TNozzleBox: trampled open
	EV_BRICK         = 5, // TBrickBlock: broken by a head attack
	EV_SUPER_HIPDROP = 6, // TSuperHipDropBlock: broken by a super hip drop
	EV_MANHOLE       = 7, // TManhole (plaza grates): opened by a hip drop or from below (w15)
	EV_PICTURE       = 8, // TWaterHitPictureHideObj (red X graffiti, posters): sprayed clear (w15)
};

namespace {

const int kResEvents = 6; // PortMpSessionSettings.reserved[6]: 1 = sync world events

enum Op : u8 { OP_CLAIM = 1, OP_DONE, OP_REQ, OP_SET };
#pragma pack(push, 1)
struct Ev {
	u32 id;   // manager << 16 | object
	u8 kind;  // PortMpEventKind
	u8 n;     // n-th time this player did it to that object (first claim per n wins)
	u8 slot;  // who did it (DONE / SET)
	u8 pad;
};
struct Msg {
	u8 op;
	u8 stage, scenario;
	u8 count;
	Ev ev[1];
};
#pragma pack(pop)
const int kMaxEv = (PORT_MP_APP_MAX - 4) / (int)sizeof(Ev);

// A stand-in "Mario" to replay hit messages: objects that answer the sender (nozzle box ->
// ATTACK) answer this one, which ignores everything.
struct TMpProxyActor : public THitActor {
	TMpProxyActor() : THitActor("mp event proxy") { mActorType = ACTOR_TYPE_MARIO; }
	virtual BOOL receiveMessage(THitActor*, u32) { return FALSE; }
};

bool sApplying;
TMarDirector* sVisitDir;
unsigned sVisitDirSerial; // gPortMpStageSerial of this visit
int sStage = -1, sScenario = -1;
bool sEnabled;        // latched for this visit
int sFrames;          // frames into this visit
std::map<std::pair<u32, u8>, u8> sLocalCount; // (id, kind) -> times done here this visit
std::vector<Ev> sDone; // applied or done here this visit
std::vector<Ev> sPending; // received, waiting for the stage to be loaded
// host: per stage visit (stage << 8 | scenario) the accepted events, in order
std::map<u32, std::vector<Ev>> sHostLists;
int sStatSent, sStatApplied;
struct Verify {
	u32 id;
	int frame;
};
std::vector<Verify> sVerify; // log each event's object state 3 s later (tests)

bool same(const Ev& a, const Ev& b) { return a.id == b.id && a.kind == b.kind && a.n == b.n; }
bool done(const Ev& e)
{
	for (const Ev& d : sDone)
		if (same(d, e))
			return true;
	return false;
}

int hostSlot()
{
	for (int s = 0; s < PORT_MP_MAX_PLAYERS; s++) {
		PortMpPlayerInfo p;
		if (port_mp_get_player(s, &p) && p.isHost)
			return s;
	}
	return 0;
}

bool settingOn()
{
	PortMpSessionSettings s;
	port_mp_get_settings(&s);
	return s.reserved[kResEvents] == 0; // 0 = on (default, w15), 1 = off
}

// The scene's conductor, if this stage has one (gpConductor is never cleared: see w11).
TConductor* conductor()
{
	JDrama::TNameRefGen* gen = JDrama::TNameRefGen::getInstance();
	if (gpConductor == nullptr || gen == nullptr || gen->getRootNameRef() == nullptr)
		return nullptr;
	JDrama::TNameRef* found = gen->getRootNameRef()->search("\x83\x52\x83\x93\x83\x5F\x83\x4E\x83\x5E\x81\x5B");
	return found == (JDrama::TNameRef*)gpConductor ? gpConductor : nullptr;
}

bool idOf(const void* obj, u32* id)
{
	TConductor* c = conductor();
	if (c == nullptr)
		return false;
	int mi = 0;
	for (JGadget::TList<TLiveManager*>::iterator it = c->unk10.begin(), e = c->unk10.end(); it != e; ++it, ++mi) {
		TLiveManager* m = *it;
		if (m == nullptr)
			continue;
		for (int i = 0; i < m->getObjNum(); i++)
			if ((const void*)m->getObj(i) == obj) {
				*id = ((u32)mi << 16) | (u32)i;
				return true;
			}
	}
	return false;
}

TLiveActor* actorOf(u32 id)
{
	TConductor* c = conductor();
	if (c == nullptr)
		return nullptr;
	int mi = 0;
	for (JGadget::TList<TLiveManager*>::iterator it = c->unk10.begin(), e = c->unk10.end(); it != e; ++it, ++mi)
		if (mi == (int)(id >> 16)) {
			TLiveManager* m = *it;
			const int i     = (int)(id & 0xFFFF);
			return m != nullptr && i < m->getObjNum() ? m->getObj(i) : nullptr;
		}
	return nullptr;
}

void send(u8 op, const Ev* ev, int n, int to)
{
	for (int i = 0; i < n || (n == 0 && i == 0); i += kMaxEv) {
		u8 buf[PORT_MP_APP_MAX];
		Msg* m      = (Msg*)buf;
		const int k = n - i < kMaxEv ? n - i : kMaxEv;
		m->op       = op;
		m->stage    = (u8)sStage;
		m->scenario = (u8)sScenario;
		m->count    = (u8)(k > 0 ? k : 0);
		if (k > 0)
			std::memcpy(m->ev, ev + i, sizeof(Ev) * k);
		port_mp_send(PORT_MP_CH_EVENTS, buf, 4 + (int)sizeof(Ev) * (k > 0 ? k : 0), 1, to);
		if (n == 0)
			break;
	}
}

// The game's own reaction, on our copy of the object.
void apply(const Ev& e)
{
	TLiveActor* a = actorOf(e.id);
	if (a == nullptr)
		return;
	// built once in static storage (never on a stage heap that is freed at the next stage)
	alignas(TMpProxyActor) static unsigned char sStore[sizeof(TMpProxyActor)];
	static TMpProxyActor* sProxy = new (sStore) TMpProxyActor;
	sProxy->mPosition            = a->mPosition;
	sApplying                    = true;
	switch (e.kind) {
	case EV_SWITCH:
		a->receiveMessage(sProxy, HIT_MESSAGE_HIP_DROP);
		break;
	case EV_REDCOIN: {
		// the switch freezes the presser's pad during its animation: not ours
		TMarioGamePad* pad = gpMarDirector->unk18[0];
		const s32 frames   = pad != nullptr ? pad->mDisabledFrames : 0;
		a->receiveMessage(sProxy, HIT_MESSAGE_HIP_DROP);
		if (pad != nullptr)
			pad->mDisabledFrames = frames;
		break;
	}
	case EV_FRUIT_SWITCH:
		a->receiveMessage(sProxy, HIT_MESSAGE_HIP_DROP);
		break;
	case EV_NOZZLE_BOX:
		a->receiveMessage(sProxy, HIT_MESSAGE_TRAMPLE);
		break;
	case EV_BRICK:
		if (!a->checkLiveFlag(LIVE_FLAG_DEAD))
			a->kill();
		break;
	case EV_SUPER_HIPDROP:
		a->receiveMessage(sProxy, HIT_MESSAGE_SUPER_HIP_DROP);
		break;
	case EV_MANHOLE:
		port_mp_manhole_open(a);
		break;
	case EV_PICTURE:
		port_mp_picture_clear(a);
		break;
	default:
		break;
	}
	sApplying = false;
	sStatApplied++;
	sVerify.push_back(Verify{ e.id, sFrames });
	std::fprintf(stderr, "[mp-ev] applied event kind %d on object %08x (by slot %d): %s\n", e.kind, e.id, e.slot,
	             a->checkLiveFlag(LIVE_FLAG_DEAD) ? "now dead" : "alive");
}

// Host: accept a claim (first one wins), broadcast it, and apply it here if it is our stage.
void hostAccept(Ev e, int slot, int stage, int scenario)
{
	std::vector<Ev>& list = sHostLists[(u32)stage << 8 | (u32)scenario];
	for (const Ev& x : list)
		if (same(x, e))
			return;
	e.slot = (u8)slot;
	list.push_back(e);
	const int saveStage = sStage, saveScen = sScenario;
	sStage = stage, sScenario = scenario;
	send(OP_DONE, &e, 1, -1);
	sStage = saveStage, sScenario = saveScen;
	if (slot != port_mp_local_slot() && stage == sStage && scenario == sScenario)
		sPending.push_back(e);
}

// Host: forget the lists of stages nobody is in (a revisit starts fresh, as on GameCube).
void hostCleanup()
{
	for (auto it = sHostLists.begin(); it != sHostLists.end();) {
		bool someone = false;
		for (int s = 0; s < PORT_MP_MAX_PLAYERS; s++) {
			PortMpPlayerInfo p;
			if (port_mp_get_player(s, &p) && ((u32)p.stage << 8 | p.scenario) == it->first)
				someone = true;
		}
		it = someone ? std::next(it) : sHostLists.erase(it);
	}
}

void receive()
{
	u8 buf[PORT_MP_APP_MAX];
	int from, n;
	while ((n = port_mp_recv(PORT_MP_CH_EVENTS, buf, sizeof(buf), &from)) > 0) {
		if (n < 4)
			continue;
		const Msg* m = (const Msg*)buf;
		const int k  = m->count;
		if (4 + k * (int)sizeof(Ev) > n)
			continue; // malformed
		const bool host = port_mp_is_host() != 0;
		switch (m->op) {
		case OP_CLAIM:
			if (host)
				for (int i = 0; i < k; i++)
					hostAccept(m->ev[i], from, m->stage, m->scenario);
			break;
		case OP_REQ:
			if (host) {
				hostCleanup();
				auto it = sHostLists.find((u32)m->stage << 8 | m->scenario);
				std::fprintf(stderr, "[mp-ev] slot %d asks for stage %d/%d: %d events\n", from, m->stage, m->scenario,
				             it != sHostLists.end() ? (int)it->second.size() : 0);
				if (it != sHostLists.end() && !it->second.empty()) {
					const int saveStage = sStage, saveScen = sScenario;
					sStage = m->stage, sScenario = m->scenario;
					send(OP_SET, it->second.data(), (int)it->second.size(), from);
					sStage = saveStage, sScenario = saveScen;
				}
			}
			break;
		case OP_DONE:
		case OP_SET:
			if (m->op == OP_SET)
				std::fprintf(stderr, "[mp-ev] %d events already done in stage %d/%d\n", k, m->stage, m->scenario);
			if (m->stage != sStage || m->scenario != sScenario)
				break; // another stage's (or an old visit's) event
			for (int i = 0; i < k; i++)
				if (m->ev[i].slot != port_mp_local_slot())
					sPending.push_back(m->ev[i]);
			break;
		}
	}
}

// SMS_PORT_MP_EV_TEST=<logic frame in the stage>[,...] (debug): at each, the local Mario
// "triggers" the next event-capable object of the stage (sends it its trigger message, as a
// hip drop / trample would), so the event path can be tested without walking there.
void debugTest()
{
	static const char* sList = std::getenv("SMS_PORT_MP_EV_TEST");
	static std::vector<TLiveActor*> sTried;
	if (sList == nullptr || gpMarioOriginal == nullptr)
		return;
	bool due = false;
	for (const char* p = sList; *p;) {
		char* e;
		const long f = std::strtol(p, &e, 10);
		if (e == p)
			break;
		if (f == sFrames)
			due = true;
		p = *e == ',' ? e + 1 : e;
	}
	TConductor* c = conductor();
	if (!due || c == nullptr)
		return;
	for (JGadget::TList<TLiveManager*>::iterator it = c->unk10.begin(), e = c->unk10.end(); it != e; ++it) {
		TLiveManager* m = *it;
		for (int i = 0; m != nullptr && i < m->getObjNum(); i++) {
			TLiveActor* a = m->getObj(i);
			if (a == nullptr)
				continue;
			if (a->checkLiveFlag(LIVE_FLAG_DEAD)) {
				static int sDeadLogged;
				if (a->isActorType(ACTOR_TYPE_NOZZLE_BOX) && sDeadLogged++ < 3)
					std::fprintf(stderr, "[mp-ev] test: nozzle box at %.0f %.0f %.0f is dead (live flags %08x)\n",
					             a->mPosition.x, a->mPosition.y, a->mPosition.z, (unsigned)a->mLiveFlag);
				continue;
			}
			bool tried = false;
			for (TLiveActor* t : sTried)
				tried |= t == a;
			if (tried)
				continue;
			u32 msg  = 0;
			bool hit = true; // (HIT_MESSAGE_TRAMPLE is 0)
			if (a->isActorType(ACTOR_TYPE_OBJ_SWITCH) || a->isActorType(ACTOR_TYPE_RED_COIN_SWITCH)
			    || a->isActorType(ACTOR_TYPE_RICCO_SWITCH))
				msg = HIT_MESSAGE_HIP_DROP;
			else if (a->isActorType(ACTOR_TYPE_NOZZLE_BOX))
				msg = HIT_MESSAGE_TRAMPLE;
			else if (a->isActorType(ACTOR_TYPE_SUPER_HIP_DROP_BLOCK))
				msg = HIT_MESSAGE_SUPER_HIP_DROP;
			else if (a->isActorType(ACTOR_TYPE_BRICK_BLOCK)) {
				sTried.push_back(a);
				std::fprintf(stderr, "[mp-ev] test: brick block %p\n", a);
				port_mp_event_local(a, gpMarioOriginal, EV_BRICK); // its head-attack check needs a real jump
				a->kill();
				return;
			}
			else if (a->isActorType(ACTOR_TYPE_MANHOLE)) {
				sTried.push_back(a);
				std::fprintf(stderr, "[mp-ev] test: grate at %.0f %.0f %.0f opens\n", a->mPosition.x, a->mPosition.y,
				             a->mPosition.z);
				port_mp_event_local(a, gpMarioOriginal, EV_MANHOLE); // opened by a real hip drop otherwise
				port_mp_manhole_open(a);
				return;
			}
			else
				hit = false;
			if (!hit)
				continue;
			sTried.push_back(a);
			std::fprintf(stderr, "[mp-ev] test: object type %08x at %.0f %.0f %.0f gets message %u\n",
			             (unsigned)a->getActorType(), a->mPosition.x, a->mPosition.y, a->mPosition.z, msg);
			a->receiveMessage(gpMarioOriginal, msg);
			return;
		}
	}
	std::fprintf(stderr, "[mp-ev] test: no (more) event-capable objects here\n");
}

void gameFrame()
{
	if ((gpMarDirector != sVisitDir || gPortMpStageSerial != sVisitDirSerial)) { // a new stage visit
		sVisitDir = gpMarDirector;
		sVisitDirSerial = gPortMpStageSerial;
		sStage    = gpMarDirector->getCurrentMap();
		sScenario = gpMarDirector->unk7D;
		sEnabled  = false;
		sFrames   = 0;
		sLocalCount.clear();
		sDone.clear();
		sPending.clear();
		sVerify.clear();
	}
	// The stage loads while the director already runs (states 0-3): start once it plays.
	if (sFrames == 0 && gpMarDirector->mState < TMarDirector::STATE_UNK4)
		return;
	sFrames++;
	if (sFrames == 1) { // the stage's objects exist from here on
		sEnabled = settingOn();
		if (sEnabled) {
			int counts[8] = {};
			bool items = false;
			if (TConductor* c = conductor())
				for (JGadget::TList<TLiveManager*>::iterator it = c->unk10.begin(), e = c->unk10.end(); it != e; ++it) {
					TLiveManager* m = *it;
					items |= (void*)m == (void*)gpItemManager;
					for (int i = 0; m != nullptr && i < m->getObjNum(); i++) {
						TLiveActor* a = m->getObj(i);
						if (a == nullptr)
							continue;
						counts[1] += a->isActorType(ACTOR_TYPE_OBJ_SWITCH);
						counts[2] += a->isActorType(ACTOR_TYPE_RED_COIN_SWITCH);
						counts[3] += a->isActorType(ACTOR_TYPE_RICCO_SWITCH);
						counts[4] += a->isActorType(ACTOR_TYPE_NOZZLE_BOX);
						counts[5] += a->isActorType(ACTOR_TYPE_BRICK_BLOCK);
						counts[6] += a->isActorType(ACTOR_TYPE_SUPER_HIP_DROP_BLOCK);
						counts[7] += a->isActorType(ACTOR_TYPE_MANHOLE);
					}
				}
			std::fprintf(stderr,
			             "[mp-ev] stage %d/%d: world events on (switches %d, red-coin switches %d, fruit switches %d, "
			             "nozzle boxes %d, bricks %d, super-hip-drop blocks %d, grates %d; item manager %s)\n",
			             sStage, sScenario, counts[1], counts[2], counts[3], counts[4], counts[5], counts[6], counts[7],
			             items ? "in the conductor" : "NOT in the conductor");
			if (!port_mp_is_host())
				send(OP_REQ, nullptr, 0, hostSlot()); // what was done here before we came
			else {
				hostCleanup();
				auto it = sHostLists.find((u32)sStage << 8 | (u32)sScenario);
				if (it != sHostLists.end())
					for (const Ev& e : it->second)
						if (e.slot != port_mp_local_slot())
							sPending.push_back(e);
			}
		}
	}
	receive();
	if (!sEnabled)
		return;
	debugTest();
	for (size_t i = 0; i < sVerify.size();) {
		if (sFrames - sVerify[i].frame >= 90) {
			TLiveActor* a = actorOf(sVerify[i].id);
			std::fprintf(stderr, "[mp-ev] object %08x 3 s later: %s\n", sVerify[i].id,
			             a == nullptr ? "?" : a->checkLiveFlag(LIVE_FLAG_DEAD) ? "dead" : "alive");
			sVerify.erase(sVerify.begin() + i);
		} else {
			i++;
		}
	}
	for (size_t i = 0; i < sPending.size(); i++)
		if (!done(sPending[i])) {
			sDone.push_back(sPending[i]);
			apply(sPending[i]);
		}
	sPending.clear();
}

void lobbySection(int isHost)
{
	PortMpSessionSettings s;
	port_mp_get_settings(&s);
	bool on = s.reserved[kResEvents] == 0;
	if (isHost) {
		if (ImGui::Checkbox("Sync world events (switches, nozzle boxes, blocks, grates)", &on)) {
			s.reserved[kResEvents] = on ? 0 : 1;
			port_mp_host_set_settings(&s);
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("On by default. A switch pressed or a box broken by one player happens for\n"
			                  "everyone in that level. Applies from each player's next level load.");
	} else {
		ImGui::Text("World events: %s", on ? "synced" : "off");
	}
	if (on)
		ImGui::TextDisabled("  this level: %d sent, %d applied", sStatSent, sStatApplied);
}

struct Register {
	Register()
	{
		port_mp_register_game_frame(gameFrame);
		port_mp_register_lobby_section("World events", lobbySection);
	}
} sRegister;

} // namespace

extern "C" {

// Game hooks: `obj` just reacted to the local player (`sender`) in a way listed above.
void port_mp_event_local(void* obj, void* sender, int kind)
{
	if (sApplying || !sEnabled || obj == nullptr || sender != (void*)gpMarioOriginal || !port_mp_active())
		return;
	Ev e;
	std::memset(&e, 0, sizeof(e));
	if (!idOf(obj, &e.id))
		return; // not a scene object of this stage's managers
	e.kind = (u8)kind;
	e.n    = sLocalCount[std::make_pair(e.id, e.kind)]++;
	e.slot = (u8)port_mp_local_slot();
	sDone.push_back(e);
	sStatSent++;
	sVerify.push_back(Verify{ e.id, sFrames });
	std::fprintf(stderr, "[mp-ev] event kind %d on object %08x (n %d)\n", kind, e.id, e.n);
	if (port_mp_is_host())
		hostAccept(e, port_mp_local_slot(), sStage, sScenario);
	else
		send(OP_CLAIM, &e, 1, hostSlot());
}

int port_mp_event_applying(void) { return sApplying ? 1 : 0; }

} // extern "C"

#endif // SMS_PORT_AURORA
