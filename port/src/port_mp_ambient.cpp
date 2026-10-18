// Multiplayer ambient sync (w15): fruit and animals (birds) look the same for everyone in a stage.
//
// Each client keeps simulating its own copies; one client is the authority per object and sends
// its state on PORT_MP_CH_AMBIENT (22, unreliable, every 3 logic frames for what moved, a
// keyframe every second). The others steer their copy to it.
//  - Authority: the stage leader (the lowest player slot in this stage + scenario) for animals
//    and for fruit nobody holds; a player whose own Mario holds a fruit, or threw it less than
//    2 s ago, claims that fruit and sends it instead (the latest claim wins).
//  - Animals (hit category ANIMAL: the plaza's birds, butterflies...): position and yaw,
//    smoothed. Their own AI keeps running, so only where they are is shared.
//  - Fruit (TResetFruit family: coconut, papaya, pineapple, durian, banana): position and
//    velocity (copies not held here are steered, a held one is left to its holder); "waiting to
//    appear" (eaten by Yoshi, rotted, broken) is followed; a fruit another player carries is
//    hidden here (makeObjDead) while his puppet shows its copy in his hands, and appears again
//    where he throws or drops it.
// Ids as w10's/w11's: (manager index in TConductor's list) << 16 | object index. Nothing runs
// outside a session (registered game-frame callbacks only run during one).
// Not covered: fruit an NPC (Pianta) carries on another screen (NPC sync, w12, is the owner of
// that); fruit spawned at run time.
#ifdef SMS_PORT_AURORA

#include "port_mp.h"

#include <Enemy/Conductor.hpp>
#include <MoveBG/MapObjBall.hpp>
#include <MoveBG/MapObjGeneral.hpp>
#include <Player/Mario.hpp>
#include <Strategic/ActorTypes.hpp>
#include <Strategic/LiveManager.hpp>
#include <System/MarDirector.hpp>
#include <JSystem/JDrama/JDRNameRefGen.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

extern "C" int gPortMpNpcPuppets; // port_mp_npcs.cpp (w12): NPCs driven by the network now

namespace {

const uint8_t kChannel = PORT_MP_CH_AMBIENT; // 22
const uint8_t kVersion = 1;
enum { K_ANIMAL = 1, K_FRUIT = 2 };
enum { F_CLAIM = 1, F_HELD = 2, F_WAITING = 4, F_DEAD = 8, F_NPC = 16 /* an NPC carries it */ };

#pragma pack(push, 1)
struct Hdr {
	uint8_t version, stage, scenario, count;
};
struct Ent {
	uint32_t id;
	uint8_t kind, flags;
	int16_t yaw; // animals: mRotation.y in s16 units of 360 degrees
	float pos[3];
	float vel[3];
};
#pragma pack(pop)
const int kMaxPerMsg = (PORT_MP_APP_MAX - (int)sizeof(Hdr)) / (int)sizeof(Ent);

struct Obj {
	u32 id;
	TLiveActor* a;
	u8 kind;
	// what we last sent
	float sentPos[3];
	u32 sentFrame;
	// authority state received
	bool haveTarget;
	int fromSlot;
	u32 rxFrame;
	Ent target;
	// fruit claims
	int claimSlot;      // slot whose claim is active (-1 none)
	u32 claimFrame;     // last frame a claim from it arrived
	u32 myClaimUntil;   // we claim it until this frame (held, or just thrown)
	bool hiddenByUs;    // makeObjDead'ed because another player carries it
};

std::vector<Obj> sObjs;
TMarDirector* sVisitDir;
unsigned sVisitDirSerial; // gPortMpStageSerial of this visit
int sStage, sScenario;
u32 sFrames;
bool sBuilt;
bool sLog;

TConductor* conductor()
{
	JDrama::TNameRefGen* gen = JDrama::TNameRefGen::getInstance();
	if (gpConductor == nullptr || gen == nullptr || gen->getRootNameRef() == nullptr)
		return nullptr;
	JDrama::TNameRef* found = gen->getRootNameRef()->search("\x83\x52\x83\x93\x83\x5F\x83\x4E\x83\x5E\x81\x5B");
	return found == (JDrama::TNameRef*)gpConductor ? gpConductor : nullptr;
}

bool isFruit(const TLiveActor* a)
{
	const u32 t = ((TLiveActor*)a)->getActorType();
	return t == ACTOR_TYPE_FRUIT_COCONUT || t == ACTOR_TYPE_FRUIT_PAPAYA || t == ACTOR_TYPE_FRUIT_PINE
	       || t == ACTOR_TYPE_FRUIT_DURIAN || t == ACTOR_TYPE_FRUIT_BANANA;
}

void build()
{
	sObjs.clear();
	TConductor* c = conductor();
	if (c == nullptr)
		return;
	int mi = 0, animals = 0, fruits = 0;
	for (JGadget::TList<TLiveManager*>::iterator it = c->unk10.begin(), e = c->unk10.end(); it != e; ++it, ++mi) {
		TLiveManager* m = *it;
		for (int i = 0; m != nullptr && i < m->getObjNum(); i++) {
			TLiveActor* a = m->getObj(i);
			if (a == nullptr)
				continue;
			u8 kind = 0;
			if (a->isHitCategory(HIT_CATEGORY_ANIMAL))
				kind = K_ANIMAL, animals++;
			else if (isFruit(a))
				kind = K_FRUIT, fruits++;
			if (kind == 0)
				continue;
			Obj o;
			std::memset(&o, 0, sizeof(o));
			o.id        = ((u32)mi << 16) | (u32)i;
			o.a         = a;
			o.kind      = kind;
			o.claimSlot = -1;
			o.fromSlot  = -1;
			sObjs.push_back(o);
		}
	}
	std::fprintf(stderr, "[mp-amb] stage %d/%d: %d animals, %d fruit synced\n", sStage, sScenario, animals, fruits);
}

int leaderSlot()
{
	int best = port_mp_local_slot();
	for (int s = 0; s < PORT_MP_MAX_PLAYERS; s++)
		if (s < best && s != port_mp_local_slot() && port_mp_same_stage(s))
			best = s;
	return best;
}

bool othersHere()
{
	for (int s = 0; s < PORT_MP_MAX_PLAYERS; s++)
		if (s != port_mp_local_slot() && port_mp_same_stage(s))
			return true;
	return false;
}

TResetFruit* fruit(Obj& o) { return (TResetFruit*)o.a; }

bool waiting(Obj& o) { return fruit(o)->isState(TMapObjGeneral::STATE_WAITING_TO_APPEAR); }

Ent stateOf(Obj& o, u8 flags)
{
	Ent e;
	std::memset(&e, 0, sizeof(e));
	e.id    = o.id;
	e.kind  = o.kind;
	e.flags = flags;
	e.yaw   = (int16_t)(o.a->mRotation.y * (65536.0f / 360.0f));
	for (int k = 0; k < 3; k++) {
		e.pos[k] = (&o.a->mPosition.x)[k];
		e.vel[k] = (&o.a->mVelocity.x)[k];
	}
	if (o.kind == K_FRUIT) {
		if (waiting(o))
			e.flags |= F_WAITING;
		TTakeActor* h = fruit(o)->getHolder();
		if (h != nullptr && h != (TTakeActor*)gpMarioOriginal)
			e.flags |= F_NPC;
		if (o.a->checkLiveFlag(LIVE_FLAG_DEAD) && !o.hiddenByUs)
			e.flags |= F_DEAD;
	}
	return e;
}

void sendAll()
{
	const int me     = port_mp_local_slot();
	const bool lead  = leaderSlot() == me;
	std::vector<Ent> out;
	for (Obj& o : sObjs) {
		u8 flags = 0;
		if (o.kind == K_FRUIT) {
			TResetFruit* f = fruit(o);
			if (f->getHolder() == (TTakeActor*)gpMarioOriginal) {
				o.myClaimUntil = sFrames + 60; // 2 s after he lets go
				flags |= F_HELD;
			}
			const bool mine = sFrames < o.myClaimUntil;
			if (mine)
				flags |= F_CLAIM;
			else if (!lead || (o.claimSlot >= 0 && sFrames - o.claimFrame < 45) || o.hiddenByUs)
				continue; // someone else's
		} else if (!lead) {
			continue;
		}
		const float* p = &o.a->mPosition.x;
		const float d  = std::fabs(p[0] - o.sentPos[0]) + std::fabs(p[1] - o.sentPos[1]) + std::fabs(p[2] - o.sentPos[2]);
		if (d < 1.0f && sFrames - o.sentFrame < 30 && !(flags & F_CLAIM))
			continue;
		std::memcpy(o.sentPos, p, sizeof(o.sentPos));
		o.sentFrame = sFrames;
		out.push_back(stateOf(o, flags));
	}
	for (size_t i = 0; i < out.size(); i += kMaxPerMsg) {
		uint8_t buf[PORT_MP_APP_MAX];
		const int n = (int)(out.size() - i < (size_t)kMaxPerMsg ? out.size() - i : (size_t)kMaxPerMsg);
		Hdr h       = { kVersion, (uint8_t)sStage, (uint8_t)sScenario, (uint8_t)n };
		std::memcpy(buf, &h, sizeof(h));
		std::memcpy(buf + sizeof(h), &out[i], sizeof(Ent) * n);
		port_mp_send(kChannel, buf, (int)(sizeof(h) + sizeof(Ent) * n), 0, -1);
	}
}

Obj* find(u32 id)
{
	for (Obj& o : sObjs)
		if (o.id == id)
			return &o;
	return nullptr;
}

void receive()
{
	uint8_t buf[PORT_MP_APP_MAX];
	int from = -1, n;
	while ((n = port_mp_recv(kChannel, buf, sizeof(buf), &from)) != 0) {
		if (n < (int)sizeof(Hdr))
			continue;
		Hdr h;
		std::memcpy(&h, buf, sizeof(h));
		if (h.version != kVersion || h.stage != sStage || h.scenario != sScenario
		    || (int)(sizeof(h) + h.count * sizeof(Ent)) > n || !sBuilt)
			continue;
		for (int i = 0; i < h.count; i++) {
			Ent e;
			std::memcpy(&e, buf + sizeof(h) + i * sizeof(Ent), sizeof(e));
			Obj* o = find(e.id);
			if (o == nullptr || o->kind != e.kind)
				continue;
			if (e.flags & F_CLAIM) {
				o->claimSlot  = from;
				o->claimFrame = sFrames;
				if (sFrames < o->myClaimUntil && fruit(*o)->getHolder() != (TTakeActor*)gpMarioOriginal)
					o->myClaimUntil = 0; // his claim is newer than our throw
			} else if (o->claimSlot >= 0 && sFrames - o->claimFrame < 45 && from != o->claimSlot) {
				continue; // the leader's view of a fruit someone else has
			}
			o->target     = e;
			o->haveTarget = true;
			o->fromSlot   = from;
			o->rxFrame    = sFrames;
		}
	}
}

void steer(float* p, const float* t, float k, float snap)
{
	const float dx = t[0] - p[0], dy = t[1] - p[1], dz = t[2] - p[2];
	if (dx * dx + dy * dy + dz * dz > snap * snap)
		k = 1.0f;
	p[0] += dx * k, p[1] += dy * k, p[2] += dz * k;
}

void apply()
{
	for (Obj& o : sObjs) {
		if (!o.haveTarget || sFrames - o.rxFrame > 90) {
			if (o.hiddenByUs) { // its carrier went quiet (left the stage): give it back
				o.hiddenByUs = false;
				std::memcpy(&o.a->mPosition.x, o.target.pos, sizeof(o.target.pos));
				fruit(o)->makeObjAppeared();
			}
			continue;
		}
		const Ent& t = o.target;
		if (o.kind == K_ANIMAL) {
			steer(&o.a->mPosition.x, t.pos, 0.35f, 600.0f);
			o.a->mRotation.y = t.yaw * (360.0f / 65536.0f);
			continue;
		}
		TResetFruit* f = fruit(o);
		if (f->getHolder() == (TTakeActor*)gpMarioOriginal || sFrames < o.myClaimUntil)
			continue; // ours
		if (TTakeActor* h = f->getHolder()) {
			if (!(t.flags & F_NPC)) { // an NPC here carries it, nobody there: let go (as rotting does)
				h->receiveMessage(f, HIT_MESSAGE_UNK8);
				h->mHeldObject = nullptr;
				f->mHolder     = nullptr;
				if (sLog)
					std::fprintf(stderr, "[mp-amb] fruit %08x: our NPC lets go of it\n", o.id);
			}
		}
		// Carried by another player, or by an NPC there while NPCs are not synced (our NPC is
		// elsewhere): hidden here. With NPC sync on, the NPC puppet is where the owner's NPC is,
		// so the fruit is steered into its hands below.
		if ((t.flags & F_HELD) || ((t.flags & F_NPC) && gPortMpNpcPuppets == 0 && f->getHolder() == nullptr)) {
			if (!f->checkLiveFlag(LIVE_FLAG_DEAD) && f->getHolder() == nullptr) {
				f->makeObjDead();
				o.hiddenByUs = true;
				if (sLog)
					std::fprintf(stderr, "[mp-amb] fruit %08x carried by slot %d: hidden here\n", o.id, o.fromSlot);
			}
			continue;
		}
		if (o.hiddenByUs) { // he threw or dropped it
			o.hiddenByUs = false;
			std::memcpy(&f->mPosition.x, t.pos, sizeof(t.pos));
			f->makeObjAppeared();
			std::memcpy(&f->mVelocity.x, t.vel, sizeof(t.vel));
			if (sLog)
				std::fprintf(stderr, "[mp-amb] fruit %08x let go by slot %d at %.0f %.0f %.0f\n", o.id, o.fromSlot,
				             t.pos[0], t.pos[1], t.pos[2]);
			continue;
		}
		if (f->getHolder() != nullptr)
			continue; // an NPC here has it
		if ((t.flags & F_WAITING) && !waiting(o) && !f->checkLiveFlag(LIVE_FLAG_DEAD)) {
			f->makeObjWaitingToAppear(); // eaten / rotted / broken there
			continue;
		}
		if (f->checkLiveFlag(LIVE_FLAG_DEAD) || waiting(o) || (t.flags & (F_WAITING | F_DEAD)))
			continue;
		steer(&f->mPosition.x, t.pos, (t.flags & F_NPC) ? 1.0f : 0.4f, 300.0f);
		if (t.flags & F_NPC)
			f->mVelocity.zero(); // in an NPC puppet's hands
		else
			std::memcpy(&f->mVelocity.x, t.vel, sizeof(t.vel));
	}
}

void gameFrame()
{
	if (gpMarDirector == nullptr)
		return;
	if ((gpMarDirector != sVisitDir || gPortMpStageSerial != sVisitDirSerial)) {
		sVisitDir = gpMarDirector;
		sVisitDirSerial = gPortMpStageSerial;
		sStage    = gpMarDirector->getCurrentMap();
		sScenario = gpMarDirector->unk7D;
		sFrames   = 0;
		sBuilt    = false;
		sObjs.clear();
		sLog = std::getenv("SMS_PORT_MP_AMB_LOG") != nullptr;
	}
	if (sFrames == 0 && gpMarDirector->mState < TMarDirector::STATE_UNK4) {
		uint8_t buf[PORT_MP_APP_MAX];
		int from;
		while (port_mp_recv(kChannel, buf, sizeof(buf), &from) != 0) {
		} // not ready: drop
		return;
	}
	sFrames++;
	if (!sBuilt) {
		build();
		sBuilt = true;
	}
	receive();
	if (sObjs.empty())
		return;
	if (othersHere()) {
		apply();
		if (sFrames % 3 == 0)
			sendAll();
	}
	if (sLog && sFrames % 150 == 0) {
		int steered = 0;
		for (Obj& o : sObjs)
			steered += o.haveTarget && sFrames - o.rxFrame <= 90;
		std::fprintf(stderr, "[mp-amb] frame %u: leader slot %d, %d of %zu objects follow another player\n", sFrames,
		             leaderSlot(), steered, sObjs.size());
	}
}

struct Register {
	Register() { port_mp_register_game_frame(gameFrame); }
} sRegister;

} // namespace

#endif // SMS_PORT_AURORA
