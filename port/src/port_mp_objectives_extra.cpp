// Multiplayer shared objectives, follow-ups (task board P1b, w8). Design: port/MULTIPLAYER.md,
// "Shared objectives follow-ups (P1b, w8)". Everything here is active only while a session
// runs with shared objectives on (PortMpSessionSettings.sharedObjectives); P1 (w10,
// port_mp_game.cpp) keeps the scene-placed coins, blue coins and Shines.
//
//  - Load-time items: coins that objects create while the stage loads (an enemy's red or
//    yellow coin made in its loadAfter, after TItemManager's scene items and pools) sit at
//    the same index of gpItemManager's list on every client (same scene, same load order).
//    They get deterministic ids (kind | stage | scenario | index) and are shared like P1's:
//    the host arbitrates the first claim (CLAIM -> COLLECTED), the others are credited and
//    the coin is removed.
//  - Run-time coins (the item manager's 20-coin pool reused for enemy drops, boxes, ...) have
//    no identity shared by the clients (their spawners are not synced): they stay local
//    objects, but with shared objectives every one anyone collects adds to everyone's coin
//    total in that stage (CREDIT). With P1's shared scene coins that makes the yellow coin
//    count a shared total, so the 100-coin Shine appears for everyone at 100.
//  - Red-coin switch episodes (TRedCoinSwitch, secret courses and a few stages): a switch press
//    is sent to everyone in the stage (SWITCH), who press their own switch the same way (no
//    pad lock), so the game's script makes the 8 red coins and the timer appear on every client
//    at once; the coins themselves are P1 objects and their timeout (killByTimer) ends the
//    challenge on every client within the latency. The host remembers each press; a player who
//    enters the stage while the challenge runs gets the press with the elapsed time and has its
//    coins' remaining time and the timer moved on by that much.
//  - Remote Shines: host option `reserved[7]`: 0 = toast only (P1's), 1 = also the "Shine Get"
//    banner for the players in the stage where it was collected (SHINE).
#include "port_mp.h"
#include <GC2D/ConsoleStr.hpp>
#include <GC2D/GCConsole2.hpp>
#include <GC2D/Talk2D2.hpp>
#include <JSystem/JDrama/JDRNameRefGen.hpp>
#include <MoveBG/Item.hpp>
#include <MoveBG/ItemManager.hpp>
#include <MoveBG/MapObjTown.hpp>
#include <Player/Mario.hpp>
#include <Strategic/ActorTypes.hpp>
#include <System/Application.hpp>
#include <System/FlagManager.hpp>
#include <System/MarDirector.hpp>
#include <System/StageUtil.hpp>
#include <dolphin/os.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

extern "C" u32 port_frame_count(void);           // port_main.cpp
extern "C" void port_mp_toast(const char* text); // port_mp_menu.cpp
extern "C" void port_mp_objx_taken(void* item);  // below

namespace {

// App channel of this feature (port_mp.h: "new features: take the next number"; listed in
// MULTIPLAYER.md, to be added to PortMpChannel by w10).
const u8 kChannel = PORT_MP_CH_OBJX; // 19

enum MsgType : u8 { M_CLAIM = 1, M_COLLECTED = 2, M_CREDIT = 3, M_SWITCH = 4, M_SHINE = 5, M_START = 6 };

#pragma pack(push, 1)
struct Msg {
	u8 type;
	u8 stage;
	u8 scenario;
	u8 kind;   // PortMpObjKind for CLAIM/COLLECTED
	u32 id;    // CLAIM/COLLECTED: objective id; SHINE: shine id
	s32 value; // COLLECTED: collector slot; CREDIT: coins; SWITCH: elapsed ms since the press; SHINE: slot
	s32 extra; // SWITCH: challenge length (frames, TRedCoinSwitch::unk138)
};
#pragma pack(pop)
static_assert(sizeof(Msg) == 16, "Msg layout");

// Shift-JIS object names (this file is UTF-8 and not mirrored to Shift-JIS).
const char kRedCoinSwitch[] = "\x90\xD4\x83\x52\x83\x43\x83\x93\x97\x70\x83\x58\x83\x43\x83\x62\x83\x60"; // 赤コイン用スイッチ
const char k100CoinShine[] = "\x83\x56\x83\x83\x83\x43\x83\x93\x81\x69\x82\x50\x82\x4F\x82\x4F\x96\x87\x83\x52\x83"
                             "\x43\x83\x93\x97\x70\x81\x6A";
const char k100CoinShineCamera[] = "\x83\x56\x83\x83\x83\x43\x83\x93\x81\x69\x82\x50\x82\x4F\x82\x4F\x96\x87\x83\x52"
                                   "\x83\x43\x83\x93\x97\x70\x81\x6A\x83\x4A\x83\x81\x83\x89";

const int kPoolEnd = 22; // TItemManager::loadAfter: 20 coins + 2 bottles after the scene items

// ---- stage visit (game thread) -------------------------------------------------------------
int sStatic = -1;        // scene items (TItemManager::loadAfter); -1 until a stage loads
int sLoadEnd = -1;       // items present at the first game frame of the visit
int sStage = -1, sScen = -1;
std::vector<u32> sDone;  // load-time ids collected or applied this visit
bool sApplyingSwitch;    // a remote press is being applied (do not send it back)
// a remote/late press waiting for the coins to appear (late join: move the timers on)
bool sAdjustPending;
double sStartRefMs;   // when the challenge started elsewhere, on our clock (port_mp_time_ms)
bool sStartSeen;        // the challenge's start was sent by us or received this visit
bool sAutoCloseTalk;    // close the switch's instruction talk (the challenge runs elsewhere)
double sAutoCloseUntil; // ... until then (port_mp_time_ms)
std::vector<Msg> sDeferred; // messages for this stage that arrived before the visit was ready

// ---- host bookkeeping ------------------------------------------------------------------------
struct Claim {
	u8 slot;
};
std::map<u32, Claim> sClaims; // load-time ids claimed, per (stage, scenario) in the id
struct SwitchRec {
	double pressMs;
	double startMs; // 0 until the presser's coins and timer appeared (START)
	s32 frames;
};
std::map<u16, SwitchRec> sSwitches; // (stage << 8 | scenario) -> press
u8 sPrevStage[PORT_MP_MAX_PLAYERS], sPrevScen[PORT_MP_MAX_PLAYERS];
double sLastSweepMs;

bool shared()
{
	PortMpSessionSettings s;
	port_mp_get_settings(&s);
	return port_mp_active() && s.sharedObjectives != 0;
}

bool bannerOption()
{
	PortMpSessionSettings s;
	port_mp_get_settings(&s);
	return s.reserved[7] == 1;
}

int hostSlot()
{
	PortMpPlayerInfo p;
	for (int i = 0; i < PORT_MP_MAX_PLAYERS; i++)
		if (port_mp_get_player(i, &p) && p.isHost)
			return i;
	return 0;
}

const char* playerName(int slot)
{
	static PortMpPlayerInfo p;
	if (slot >= 0 && slot < PORT_MP_MAX_PLAYERS && port_mp_get_player(slot, &p))
		return p.name;
	return "Someone";
}

bool visitReady() { return sLoadEnd >= 0 && sStage >= 0; }

bool done(u32 id)
{
	for (u32 x : sDone)
		if (x == id)
			return true;
	return false;
}

int indexOf(const void* item)
{
	if (gpItemManager == nullptr)
		return -1;
	for (int i = 0; i < gpItemManager->getObjNum(); i++)
		if ((const void*)gpItemManager->getObj(i) == item)
			return i;
	return -1;
}

void send(const Msg& m, int to) { port_mp_send(kChannel, &m, sizeof(m), 1, to); }

void make100CoinShineIfDue(const JGeometry::TVec3<f32>& at)
{
	if (TFlagManager::getInstance()->getFlag(MSF_GOLD_COIN_COUNT) == 100 && gpItemManager != nullptr) {
		std::fprintf(stderr, "[mp-objx] coin total reached 100: the 100-coin Shine appears\n");
		gpItemManager->makeShineAppearWithDemo(k100CoinShine, k100CoinShineCamera, at.x, at.y, at.z);
	}
}

// Removes a collected item as P1 does (TItem::taken without the touch path).
void removeItem(TItem* it)
{
	it->kill();
	if (it->checkMapObjFlag(TMapObjBase::MAP_OBJ_FLAG_RESPAWNING)) {
		it->makeObjDefault();
		it->appear();
	}
}

// Credit a load-time coin collected by another player (as P1's applyObjective).
void applyCollected(u32 id, int slot)
{
	if (done(id))
		return;
	sDone.push_back(id);
	const u32 kind  = id >> 29;
	const int index = id & 0x1FFF;
	if (gpItemManager == nullptr || index >= gpItemManager->getObjNum())
		return;
	TItem* it = (TItem*)gpItemManager->getObj(index);
	const bool red = kind == PORT_MP_OBJ_RED;
	if (it == nullptr || !(red ? it->isActorType(ACTOR_TYPE_COIN_RED) : it->isActorType(ACTOR_TYPE_COIN)))
		return;
	TFlagManager* f = TFlagManager::getInstance();
	if (red)
		f->incFlag(MSF_RED_COIN_COUNT, 1);
	else
		f->incGoldCoinFlag(SMS_getShineStage((u8)sStage), 1);
	if (it->mContainer != nullptr)
		it->mContainer->receiveMessage(it, HIT_MESSAGE_UNK8);
	if (!it->checkLiveFlag(LIVE_FLAG_DEAD))
		removeItem(it);
	else
		it->makeObjDead(); // not out yet (still held): it never appears here
	std::fprintf(stderr, "[mp-objx] load-time %s coin %d collected by %s: credited\n", red ? "red" : "yellow", index,
	             playerName(slot));
	if (!red)
		make100CoinShineIfDue(it->mPosition);
}

// Presses our red-coin switch for a press made by someone else (receiveMessage's
// HIP_DROP branch without the local pad lock).
bool pressSwitch(const char* why)
{
	TRedCoinSwitch* sw = (TRedCoinSwitch*)JDrama::TNameRefGen::search(kRedCoinSwitch);
	if (sw == nullptr || sw->checkLiveFlag(LIVE_FLAG_DEAD))
		return false;
	if (sw->mState >= 2 || TFlagManager::getInstance()->getBool(MSF_RED_COIN_SWITCH_PRESSED)) {
		std::fprintf(stderr, "[mp-objx] red-coin switch already pressed here (%s)\n", why);
		return false;
	}
	sApplyingSwitch = true;
	sw->startBck("redcoinswitch");
	sw->removeMapCollision();
	sw->onHitFilter(HIT_FILTER_NO_COLLISION);
	sw->mState = 2;
	sApplyingSwitch = false;
	std::fprintf(stderr, "[mp-objx] red-coin switch pressed here (%s)\n", why);
	return true;
}

void applySwitch(const Msg& m, int from)
{
	if (!pressSwitch(m.value > 0 ? "late join" : "remote press"))
		return;
	if (from >= 0) {
		char t[96];
		std::snprintf(t, sizeof(t), "%s pressed the red coin switch!", playerName(from));
		port_mp_toast(t);
	}
	if (m.value > 0) { // the challenge already runs elsewhere: skip the talk, move the timers on
		sStartSeen       = true;
		sAutoCloseTalk   = true;
		sAutoCloseUntil  = port_mp_time_ms() + 120000.0;
		sAdjustPending   = true;
		sStartRefMs      = port_mp_time_ms() - m.value;
	}
}

// The switch's script forces an instruction talk and starts the challenge when it closes.
// When the challenge already started on another client, our talk is closed for us, so the
// coins and the timer appear here at (nearly) the same time.
void autoCloseTalk()
{
	if (!sAutoCloseTalk || gpMarDirector == nullptr || gpTalk2D == nullptr)
		return;
	if (port_mp_time_ms() > sAutoCloseUntil) {
		sAutoCloseTalk = false;
		return;
	}
	if (gpMarDirector->isTalkModeNow()) {
		gpTalk2D->forceCloseTalk();
		std::fprintf(stderr, "[mp-objx] red-coin challenge already running: instruction talk closed\n");
	}
}

// After a late press: once the script has made the red coins appear, move their lifetime
// and the on-screen timer on by the time the challenge has already run elsewhere.
void adjustLatePress()
{
	if (!sAdjustPending || gpItemManager == nullptr || gpMarDirector == nullptr)
		return;
	if (!TFlagManager::getInstance()->getBool(MSF_RED_COIN_SWITCH_PRESSED))
		return;
	const s32 elapsedMs = (s32)(port_mp_time_ms() - sStartRefMs);
	if (elapsedMs < 300) { // in step with the others: nothing to move on
		sAdjustPending = false;
		return;
	}
	int out = 0;
	const u32 frames = (u32)(elapsedMs * 60 / 1000); // object timers: 60 Hz ticks
	for (int i = 0; i < gpItemManager->getObjNum(); i++) {
		TItem* it = (TItem*)gpItemManager->getObj(i);
		if (it == nullptr || !it->isActorType(ACTOR_TYPE_COIN_RED) || it->checkLiveFlag(LIVE_FLAG_DEAD))
			continue;
		it->unk14C = it->unk14C > frames + 1 ? it->unk14C - frames : 1;
		++out;
	}
	if (out == 0)
		return; // not out yet
	gpMarDirector->unkC8 -= (s64)OSMillisecondsToTicks((s64)elapsedMs);
	std::fprintf(stderr, "[mp-objx] joined late: %d red coins and the timer moved on by %d ms\n", out, (int)elapsedMs);
	sAdjustPending = false;
	sAutoCloseTalk = false;
}

void hostRecordPress(u8 stage, u8 scen, s32 frames)
{
	sSwitches[(u16)(stage << 8 | scen)] = SwitchRec { port_mp_time_ms(), 0.0, frames };
}

void hostRecordStart(u8 stage, u8 scen, s32 frames)
{
	SwitchRec& r = sSwitches[(u16)(stage << 8 | scen)];
	if (r.pressMs == 0.0)
		r.pressMs = port_mp_time_ms();
	if (r.startMs == 0.0)
		r.startMs = port_mp_time_ms();
	r.frames = frames;
}

void handle(const Msg& m, int from)
{
	const bool here = visitReady() && m.stage == sStage && m.scenario == sScen;
	switch (m.type) {
	case M_CLAIM: { // host only
		if (!port_mp_is_host())
			return;
		if (sClaims.count(m.id))
			return; // first claim wins
		sClaims[m.id] = Claim { (u8)from };
		Msg c         = m;
		c.type        = M_COLLECTED;
		c.value       = from;
		send(c, -1);
		if (from != port_mp_local_slot())
			handle(c, from); // the host applies it too
		return;
	}
	case M_COLLECTED:
		if (m.value == port_mp_local_slot())
			return; // ours
		if (!visitReady() && (m.stage == sStage || sStage < 0)) {
			sDeferred.push_back(m);
			return;
		}
		if (here)
			applyCollected(m.id, m.value);
		return;
	case M_CREDIT:
		if (!here || !shared())
			return;
		TFlagManager::getInstance()->incGoldCoinFlag(SMS_getShineStage((u8)sStage), m.value);
		std::fprintf(stderr, "[mp-objx] run-time coin collected by %s: +%d (total %d)\n", playerName(from), m.value,
		             (int)TFlagManager::getInstance()->getFlag(MSF_GOLD_COIN_COUNT));
		if (gpMarioOriginal != nullptr)
			make100CoinShineIfDue(gpMarioOriginal->mPosition);
		return;
	case M_SWITCH:
		if (port_mp_is_host() && from != port_mp_local_slot() && m.value == 0)
			hostRecordPress(m.stage, m.scenario, m.extra);
		if (!shared())
			return;
		if (!visitReady()) {
			if (m.stage == sStage || sStage < 0)
				sDeferred.push_back(m);
			return;
		}
		if (here)
			applySwitch(m, from);
		return;
	case M_START:
		if (port_mp_is_host() && from != port_mp_local_slot())
			hostRecordStart(m.stage, m.scenario, m.value);
		if (!shared() || !here || sStartSeen)
			return;
		sStartSeen      = true;
		sAutoCloseTalk  = true;
		sAutoCloseUntil = port_mp_time_ms() + 120000.0;
		// our talk only starts once our Mario stands on the ground: if that takes a while, the
		// coins and the timer are moved on when they appear here
		sAdjustPending = true;
		sStartRefMs    = port_mp_time_ms();
		std::fprintf(stderr, "[mp-objx] %s's red-coin challenge started\n", playerName(from));
		return;
	case M_SHINE:
		if (here && bannerOption() && from != port_mp_local_slot() && gpMarDirector != nullptr
		    && gpMarDirector->getConsole() != nullptr) {
			gpMarDirector->getConsole()->unk94->startAppearShineGet();
			std::fprintf(stderr, "[mp-objx] %s got Shine %u in this stage: banner\n", playerName(from), m.id);
		}
		return;
	}
}

// Elapsed time of a recorded challenge for a late joiner: since the start (coins and timer
// out), or 0 while only pressed (the joiner then runs its talk; START closes it).
s32 lateElapsed(const SwitchRec& r)
{
	if (r.startMs == 0.0)
		return 0;
	const s32 v = (s32)(port_mp_time_ms() - r.startMs);
	return v < 1 ? 1 : v;
}

// Host: a player entering a stage with a running red-coin challenge gets the press, with the
// time it has already run.
void hostLateJoins()
{
	if (!port_mp_is_host())
		return;
	const double now = port_mp_time_ms();
	for (auto it = sSwitches.begin(); it != sSwitches.end();) {
		const SwitchRec& r = it->second;
		const bool over    = r.startMs != 0.0 ? now - r.startMs > r.frames * 1000.0 / 60.0 + 5000.0
		                                      : now - r.pressMs > 60000.0;
		it = over ? sSwitches.erase(it) : std::next(it);
	}
	for (int s = 0; s < PORT_MP_MAX_PLAYERS; s++) {
		PortMpPlayerInfo p;
		const bool used = port_mp_get_player(s, &p) != 0;
		const u8 st = used ? p.stage : 0xFF, sc = used ? p.scenario : 0xFF;
		if (used && !p.isLocal && st != 0xFF && (st != sPrevStage[s] || sc != sPrevScen[s])) {
			auto r = sSwitches.find((u16)(st << 8 | sc));
			if (r != sSwitches.end()) {
				Msg m {};
				m.type     = M_SWITCH;
				m.stage    = st;
				m.scenario = sc;
				m.value    = lateElapsed(r->second);
				m.extra    = r->second.frames;
				send(m, s);
				std::fprintf(stderr, "[mp-objx] %s entered stage %d/%d during a red-coin challenge (%d ms in): press sent\n",
				             p.name, st, sc, (int)m.value);
			}
		}
		sPrevStage[s] = st;
		sPrevScen[s]  = sc;
	}
	// forget claims of stages nobody is in (a revisit respawns the coins, as P1)
	if (now - sLastSweepMs > 2000.0) {
		sLastSweepMs = now;
		for (auto it = sClaims.begin(); it != sClaims.end();) {
			const u8 st = (it->first >> 21) & 0xFF, sc = (it->first >> 13) & 0xFF;
			bool someone = false;
			for (int s = 0; s < PORT_MP_MAX_PLAYERS && !someone; s++) {
				PortMpPlayerInfo p;
				if (port_mp_get_player(s, &p) && p.stage == st && p.scenario == sc)
					someone = true;
			}
			it = someone ? std::next(it) : sClaims.erase(it);
		}
	}
}

// Host, own stage entry: a running challenge recorded for this stage.
void hostOwnLateJoin()
{
	if (!port_mp_is_host())
		return;
	auto r = sSwitches.find((u16)(sStage << 8 | sScen));
	if (r == sSwitches.end())
		return;
	Msg m {};
	m.type     = M_SWITCH;
	m.stage    = (u8)sStage;
	m.scenario = (u8)sScen;
	m.value    = lateElapsed(r->second);
	m.extra    = r->second.frames;
	applySwitch(m, -1);
}

// ---- debug (tests) ----------------------------------------------------------------------------
// SMS_PORT_MP_OBJX_TEST="<name>:<action>@<frame>[=<arg>];..." (names as mp_test.py gives them):
//   press            press the stage's red-coin switch through its own hit path
//   coins=<n>        set this stage's yellow coin count to n
//   spawncoin        make a pool coin (a run-time coin) appear on Mario, who takes it
//   shine100         put Mario on the 100-coin Shine if it is out
void debugFrame()
{
	static const char* sTest = std::getenv("SMS_PORT_MP_OBJX_TEST");
	static std::vector<bool> sFired;
	if (sTest == nullptr || gpMarioOriginal == nullptr || !visitReady())
		return;
	const char* me   = port_mp_get_name();
	const u32 now    = port_frame_count();
	int idx          = 0;
	for (const char* p = sTest; *p != '\0'; idx++) {
		const char* end = std::strchr(p, ';');
		std::string e(p, end != nullptr ? (size_t)(end - p) : std::strlen(p));
		p = end != nullptr ? end + 1 : p + std::strlen(p);
		if ((int)sFired.size() <= idx)
			sFired.resize(idx + 1, false);
		const size_t c = e.find(':'), a = e.find('@');
		if (c == std::string::npos || a == std::string::npos || sFired[idx])
			continue;
		if (e.substr(0, c) != me)
			continue;
		const std::string action = e.substr(c + 1, a - c - 1);
		const size_t eq          = e.find('=', a);
		const u32 frame          = (u32)std::atoi(e.substr(a + 1).c_str());
		const int arg            = eq != std::string::npos ? std::atoi(e.substr(eq + 1).c_str()) : 0;
		if (now < frame)
			continue;
		sFired[idx] = true;
		TMario* m   = gpMarioOriginal;
		if (action == "press") {
			TRedCoinSwitch* sw = (TRedCoinSwitch*)JDrama::TNameRefGen::search(kRedCoinSwitch);
			std::fprintf(stderr, "[mp-objx] TEST %s: press (switch %s)\n", me, sw ? "found" : "missing");
			if (sw != nullptr) { // stand on it first, as after a real ground pound
				m->mPosition = sw->mPosition;
				m->mPosition.y += 30.0f;
				sw->receiveMessage(m, HIT_MESSAGE_HIP_DROP);
			}
		} else if (action == "toswitch") { // stand next to the switch (on the ground, not swimming)
			TRedCoinSwitch* sw = (TRedCoinSwitch*)JDrama::TNameRefGen::search(kRedCoinSwitch);
			if (sw != nullptr) {
				m->mPosition = sw->mPosition;
				m->mPosition.x += 250.0f;
				m->mPosition.y += 50.0f;
			}
			std::fprintf(stderr, "[mp-objx] TEST %s: to the switch (%s)\n", me, sw ? "found" : "missing");
		} else if (action == "coins") {
			TFlagManager::getInstance()->setFlag(MSF_GOLD_COIN_COUNT, arg);
			std::fprintf(stderr, "[mp-objx] TEST %s: coin count set to %d\n", me, arg);
		} else if (action == "spawncoin") {
			for (int i = sStatic; i >= 0 && i < sStatic + kPoolEnd && i < gpItemManager->getObjNum(); i++) {
				TItem* it = (TItem*)gpItemManager->getObj(i);
				if (it != nullptr && it->isActorType(ACTOR_TYPE_COIN) && it->checkLiveFlag(LIVE_FLAG_DEAD)) {
					it->mPosition = m->mPosition;
					it->appear();
					std::fprintf(stderr, "[mp-objx] TEST %s: pool coin %d appears on Mario\n", me, i);
					break;
				}
			}
		} else if (action == "unshine") { // forget this stage's Shines (free mode owns them all, and
			// the game's scripts start no red-coin challenge whose Shine is already got)
			int n = 0;
			TFlagManager* f = TFlagManager::getInstance();
			for (int i = 0; arg <= 0 && i < gpItemManager->getObjNum(); i++) {
				THitActor* a = gpItemManager->getObj(i);
				if (a != nullptr && a->isActorType(ACTOR_TYPE_SHINE)) {
					const u32 id = ((TShine*)a)->getEventId();
					if (id < 120 && f->getShineFlag((u8)id)) {
						f->setFlag(MSF_SHINE_BASE + id, 0);
						f->incFlag(MSF_SHINE_COUNT, -1);
						std::fprintf(stderr, "[mp-objx] TEST %s: Shine %u forgotten\n", me, id);
						++n;
					}
				}
			}
			{ // the red-coin Shine of this stage ("シャイン（赤コイン用）")
				TShine* rs = (TShine*)JDrama::TNameRefGen::search(
				    "\x83\x56\x83\x83\x83\x43\x83\x93\x81\x69\x90\xD4\x83\x52\x83\x43\x83\x93\x97\x70\x81\x6A");
				const u32 id = rs != nullptr ? rs->getEventId() : 0xFFFF;
				std::fprintf(stderr, "[mp-objx] TEST %s: red-coin Shine %s (id %u, got %d)\n", me, rs ? "found" : "missing", id,
				             rs && id < 120 ? (int)f->getShineFlag((u8)id) : -1);
				if (arg <= 0 && rs != nullptr && id < 120 && f->getShineFlag((u8)id)) {
					f->setFlag(MSF_SHINE_BASE + id, 0);
					f->incFlag(MSF_SHINE_COUNT, -1);
					++n;
				}
			}
			if (arg > 0 && arg < 120 && f->getShineFlag((u8)arg)) { // unshine=<id>: that one, anywhere
				f->setFlag(MSF_SHINE_BASE + arg, 0);
				f->incFlag(MSF_SHINE_COUNT, -1);
				++n;
			}
			std::fprintf(stderr, "[mp-objx] TEST %s: %d Shines of this stage forgotten\n", me, n);
		} else if (action == "loadcoin") { // claim the first load-time coin (arbitration test)
			bool any = false;
			for (int i = sStatic + kPoolEnd; i >= 0 && i < sLoadEnd && i < gpItemManager->getObjNum(); i++) {
				TItem* it = (TItem*)gpItemManager->getObj(i);
				if (it != nullptr && (it->isActorType(ACTOR_TYPE_COIN) || it->isActorType(ACTOR_TYPE_COIN_RED))) {
					std::fprintf(stderr, "[mp-objx] TEST %s: claim load-time coin %d\n", me, i);
					port_mp_objx_taken(it);
					any = true;
					break;
				}
			}
			if (!any)
				std::fprintf(stderr, "[mp-objx] TEST %s: no load-time coin in this stage\n", me);
		} else if (action == "shine100") {
			TMapObjBase* sh = (TMapObjBase*)JDrama::TNameRefGen::search(k100CoinShine);
			if (sh != nullptr && !sh->checkLiveFlag(LIVE_FLAG_DEAD)) {
				m->mPosition = sh->mPosition;
				std::fprintf(stderr, "[mp-objx] TEST %s: Mario to the 100-coin Shine\n", me);
			} else {
				std::fprintf(stderr, "[mp-objx] TEST %s: no 100-coin Shine out\n", me);
			}
		}
	}
}

// Log line whenever the number of red coins out changes during a switch challenge (tests).
void watchRedCoins()
{
	static int sLast = -1;
	if (!TFlagManager::getInstance()->getBool(MSF_RED_COIN_SWITCH_PRESSED)) {
		sLast = -1;
		return;
	}
	int out = 0;
	for (int i = 0; i < gpItemManager->getObjNum(); i++) {
		TItem* it = (TItem*)gpItemManager->getObj(i);
		out += it != nullptr && it->isActorType(ACTOR_TYPE_COIN_RED) && !it->checkLiveFlag(LIVE_FLAG_DEAD);
	}
	if (out != sLast)
		std::fprintf(stderr, "[mp-objx] red-coin challenge: %d red coins out, %d collected (frame %u)\n", out,
		             (int)TFlagManager::getInstance()->getFlag(MSF_RED_COIN_COUNT), port_frame_count());
	sLast = out;
}

// SMS_PORT_MP_OBJX_BANNER=0|1 (host, tests): sets reserved[7] once.
void debugHostSettings()
{
	static bool sDone;
	const char* b = std::getenv("SMS_PORT_MP_OBJX_BANNER");
	if (sDone || b == nullptr || !port_mp_is_host())
		return;
	sDone = true;
	PortMpSessionSettings s;
	port_mp_get_settings(&s);
	s.reserved[7] = std::atoi(b) != 0 ? 1 : 0;
	port_mp_host_set_settings(&s);
	std::fprintf(stderr, "[mp-objx] host settings: remote Shine banner %d\n", s.reserved[7]);
}

// Game thread, once per logic frame while a session runs.
void frame()
{
	debugHostSettings();
	hostLateJoins();
	TMarDirector* d = gpMarDirector;
	if (d == nullptr || !d->mSetupDone || gpItemManager == nullptr || sStatic < 0)
		return;
	if (sLoadEnd < 0) { // first frame of this stage visit
		sLoadEnd = gpItemManager->getObjNum();
		sStage   = SMSGetApplication()->mCurrArea.getStage();
		sScen    = SMSGetApplication()->mCurrArea.getScenario();
		int loadGold = 0, loadRed = 0;
		for (int i = sStatic + kPoolEnd; i < sLoadEnd; i++) {
			THitActor* a = gpItemManager->getObj(i);
			loadGold += a != nullptr && a->isActorType(ACTOR_TYPE_COIN);
			loadRed += a != nullptr && a->isActorType(ACTOR_TYPE_COIN_RED);
		}
		std::fprintf(stderr,
		             "[mp-objx] stage %d/%d: %d scene items, %d load-time items (%d yellow, %d red coins), shared %d\n",
		             sStage, sScen, sStatic, sLoadEnd - sStatic - kPoolEnd, loadGold, loadRed, shared() ? 1 : 0);
		// blue coins of this stage already collected (by anyone, any scenario) must not be out
		int blueOut = 0, blueFlagged = 0;
		for (int i = 0; i < gpItemManager->getObjNum(); i++) {
			TCoin* c = (TCoin*)gpItemManager->getObj(i);
			if (c == nullptr || !c->isActorType(ACTOR_TYPE_COIN_BLUE))
				continue;
			const bool flag = TFlagManager::getInstance()->getBlueCoinFlag(d->getCurrentMap(), c->getEventId());
			const bool out  = !c->checkLiveFlag(LIVE_FLAG_DEAD);
			blueOut += out;
			blueFlagged += flag;
			if (out && flag)
				std::fprintf(stderr, "[mp-objx] blue coin %d is out although its flag is set\n", c->getEventId());
		}
		std::fprintf(stderr, "[mp-objx] blue coins here: %d out, %d already collected\n", blueOut, blueFlagged);
		std::vector<Msg> later;
		later.swap(sDeferred);
		for (const Msg& m : later)
			handle(m, m.type == M_COLLECTED ? m.value : -1);
		hostOwnLateJoin();
	}
	Msg m;
	int from, n;
	while ((n = port_mp_recv(kChannel, &m, sizeof(m), &from)) != 0)
		if (n == (int)sizeof(m))
			handle(m, from);
	autoCloseTalk();
	adjustLatePress();
	watchRedCoins();
	debugFrame();
}

struct Register {
	Register() { port_mp_register_game_frame(frame); }
} sRegister;

} // namespace

extern "C" {

// ItemManager.cpp, TItemManager::loadAfter (next to w10's port_mp_items_loaded): a stage loads.
void port_mp_objx_items_loaded(int count)
{
	sStatic  = count;
	sLoadEnd = -1;
	sStage = sScen = -1;
	sDone.clear();
	sAdjustPending = false;
	sStartSeen     = false;
	sAutoCloseTalk = false;
}

// Item.cpp, TCoin::taken and TCoinRed::taken (after w10's hook): the local Mario took it.
void port_mp_objx_taken(void* item)
{
	if (!shared() || !visitReady() || item == nullptr)
		return;
	TCoin* c        = (TCoin*)item;
	const bool red  = c->isActorType(ACTOR_TYPE_COIN_RED);
	const bool gold = c->isActorType(ACTOR_TYPE_COIN);
	if (!red && !gold)
		return;
	const int index = indexOf(c);
	if (index < 0 || index < sStatic)
		return; // scene-placed: P1 (port_mp_game.cpp)
	Msg m {};
	m.stage    = (u8)sStage;
	m.scenario = (u8)sScen;
	if (index >= sStatic + kPoolEnd && index < sLoadEnd) { // load-time item: deterministic id
		m.type = M_CLAIM;
		m.kind = red ? PORT_MP_OBJ_RED : PORT_MP_OBJ_COIN;
		m.id   = PORT_MP_OBJ_ID(m.kind, sStage, sScen, index);
		if (done(m.id))
			return;
		sDone.push_back(m.id);
		std::fprintf(stderr, "[mp-objx] load-time %s coin %d collected here\n", red ? "red" : "yellow", index);
		if (port_mp_is_host())
			handle(m, port_mp_local_slot());
		else
			send(m, hostSlot());
		return;
	}
	if (gold) { // run-time coin (pool): credit only
		m.type  = M_CREDIT;
		m.value = 1;
		send(m, -1);
		std::fprintf(stderr, "[mp-objx] run-time coin %d collected here: credit sent\n", index);
	}
}

// MapObjTown.cpp, TRedCoinSwitch::receiveMessage (HIP_DROP accepted): the local Mario pressed it.
void port_mp_objx_switch_pressed(void* sw)
{
	if (sApplyingSwitch || !shared() || !visitReady() || sw == nullptr)
		return;
	Msg m {};
	m.type     = M_SWITCH;
	m.stage    = (u8)sStage;
	m.scenario = (u8)sScen;
	m.extra    = ((TRedCoinSwitch*)sw)->unk138;
	send(m, -1);
	if (port_mp_is_host())
		hostRecordPress((u8)sStage, (u8)sScen, m.extra);
	std::fprintf(stderr, "[mp-objx] red-coin switch pressed here: sent (%d frames)\n", (int)m.extra);
}

// EventWatcher.cpp, evAppear8RedCoinsAndTimer: the script made the red coins and the timer
// appear here (after the instruction talk). The first client to get there starts the
// challenge for everyone in the stage (START closes their talks).
void port_mp_objx_red_started(int frames)
{
	if (!shared() || !visitReady() || sStartSeen)
		return;
	sStartSeen = true;
	Msg m {};
	m.type     = M_START;
	m.stage    = (u8)sStage;
	m.scenario = (u8)sScen;
	m.value    = frames;
	send(m, -1);
	if (port_mp_is_host())
		hostRecordStart((u8)sStage, (u8)sScen, frames);
	std::fprintf(stderr, "[mp-objx] red-coin challenge started here: sent (%d frames)\n", frames);
}

// MarDirectorDirect.cpp, where the Shine-get demo sets the flag (next to w10's port_mp_shine_got).
void port_mp_objx_shine_got(int shineId)
{
	if (!shared() || !visitReady())
		return;
	Msg m {};
	m.type     = M_SHINE;
	m.stage    = (u8)sStage;
	m.scenario = (u8)sScen;
	m.id       = (u32)shineId;
	m.value    = port_mp_local_slot();
	send(m, -1);
}

} // extern "C"
