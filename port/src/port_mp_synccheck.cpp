// Multiplayer sync check (w14, task board P7): with SMS_PORT_MP_SYNCCHECK=1, every instance in a
// session writes a sample of the world state it simulates once per wall-clock second, on the
// first logic frame after each whole second (all test instances run on one PC, so the seconds
// line up). port/tools/mp_test.py --sync compares the samples of the same second across the
// instances and writes the mismatch report (port/MULTIPLAYER.md "Automated tests (P7)").
// Read-only: nothing here changes game state; nothing runs without the knob or outside a session.
//
// Lines (stderr):
//   [sync] t=<unix ms> slot=<s> st=<stage>/<scenario>
//   [sync-me] t=<ms> pos=<x>,<y>,<z> spray=<0|1>                          our Mario
//   [sync-rp] t=<ms> slot=<s> pos=<x>,<y>,<z> spray=<0|1> st=<stage>/<sc> age=<ms>   a remote player, newest snapshot
//   [sync-e] t=<ms> n=<count> <id>,<actor type>,<alive>,<x>,<y>,<z>;...   every actor of TConductor's
//            managers (enemies, NPCs, map objects; id = manager index << 16 | object index, as P8)
//   [sync-g] t=<ms> degree=<getPollutionDegree> layers=<n> <polluted texels>:<crc32 of the map>;...
//   [sync-f] t=<ms> cb=<card bools, hex bitset> ci=<card ints> sb=<stage bools> si=<stage ints>
#ifdef SMS_PORT_AURORA

#include "port_mp.h"

#include <Enemy/Conductor.hpp>
#include <Map/PollutionLayer.hpp>
#include <Map/PollutionManager.hpp>
#include <Player/Mario.hpp>
#include <Player/MarioAccess.hpp>
#include <Player/WaterGun.hpp>
#include <Strategic/LiveActor.hpp>
#include <Strategic/LiveManager.hpp>
#include <System/Application.hpp>
#include <System/FlagManager.hpp>
#include <System/FlagManagerFlags.hpp>
#include <System/MarDirector.hpp>
#include <JSystem/JDrama/JDRNameRefGen.hpp>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

long long unixMs()
{
	FILETIME ft;
	GetSystemTimeAsFileTime(&ft);
	const unsigned long long t = ((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
	return (long long)(t / 10000ull) - 11644473600000ll;
}

u32 crc32(const u8* p, size_t n)
{
	u32 c = 0xFFFFFFFFu;
	for (size_t i = 0; i < n; i++) {
		c ^= p[i];
		for (int k = 0; k < 8; k++)
			c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
	}
	return ~c;
}

// The scene's conductor, or null (gpConductor outlives its scene; as port_mp_objects.cpp checks).
TConductor* conductor()
{
	JDrama::TNameRefGen* gen = JDrama::TNameRefGen::getInstance();
	if (gpConductor == nullptr || gen == nullptr || gen->getRootNameRef() == nullptr)
		return nullptr;
	JDrama::TNameRef* found = gen->getRootNameRef()->search("\x83\x52\x83\x93\x83\x5F\x83\x4E\x83\x5E\x81\x5B");
	return found == (JDrama::TNameRef*)gpConductor ? gpConductor : nullptr;
}

void sampleActors(long long t)
{
	std::string s;
	int n = 0;
	if (TConductor* c = conductor()) {
		int mgr = 0;
		char buf[96];
		for (JGadget::TList<TLiveManager*>::iterator it = c->unk10.begin(), e = c->unk10.end(); it != e; ++it, ++mgr) {
			TLiveManager* m = *it;
			if (m == nullptr)
				continue;
			for (int i = 0; i < m->getObjNum(); i++) {
				TLiveActor* a = m->getObj(i);
				if (a == nullptr)
					continue;
				std::snprintf(buf, sizeof(buf), "%x,%x,%d,%.0f,%.0f,%.0f;", ((u32)mgr << 16) | (u32)i,
				              (u32)a->getActorType(), a->checkLiveFlag(LIVE_FLAG_DEAD) ? 0 : 1, a->mPosition.x,
				              a->mPosition.y, a->mPosition.z);
				s += buf;
				n++;
			}
		}
	}
	std::fprintf(stderr, "[sync-e] t=%lld n=%d %s\n", t, n, s.c_str());
}

void samplePollution(long long t)
{
	// gpPollution, like gpConductor, outlives its scene: use it only if this scene's name tree
	// has it (its object name, Shift-JIS "rakugaki kanri" = graffiti manager).
	JDrama::TNameRefGen* gen = JDrama::TNameRefGen::getInstance();
	if (gpPollution == nullptr || gen == nullptr || gen->getRootNameRef() == nullptr
	    || gen->getRootNameRef()->search("\x97\x8E\x8F\x91\x82\xAB\x8A\xC7\x97\x9D") != (JDrama::TNameRef*)gpPollution) {
		std::fprintf(stderr, "[sync-g] t=%lld none\n", t);
		return;
	}
	std::string s;
	char buf[48];
	const int layers = gpPollution->getJointModelNum();
	for (int i = 0; i < layers; i++) {
		TPollutionLayer* l = gpPollution->getLayer(i);
		if (l == nullptr || l->mPollutionMap == nullptr) {
			s += "-;";
			continue;
		}
		const size_t sz = (size_t)l->getTexWidth() * (size_t)l->getTexHeight();
		u32 dirty = 0;
		for (size_t k = 0; k < sz; k++)
			dirty += l->mPollutionMap[k] != 0;
		std::snprintf(buf, sizeof(buf), "%u:%08x;", dirty, crc32(l->mPollutionMap, sz));
		s += buf;
	}
	std::fprintf(stderr, "[sync-g] t=%lld degree=%u layers=%d %s\n", t, gpPollution->getPollutionDegree(), layers,
	             s.c_str());
}

std::string bits(u32 from, u32 to)
{
	TFlagManager* f = TFlagManager::getInstance();
	std::string s;
	char buf[4];
	for (u32 i = from; i < to; i += 4) {
		int v = 0;
		for (u32 k = 0; k < 4 && i + k < to; k++)
			v |= (f->getBool(i + k) ? 1 : 0) << k;
		std::snprintf(buf, sizeof(buf), "%x", v);
		s += buf;
	}
	return s;
}

std::string ints(u32 from, u32 to)
{
	TFlagManager* f = TFlagManager::getInstance();
	std::string s;
	char buf[16];
	for (u32 i = from; i < to; i++) {
		std::snprintf(buf, sizeof(buf), "%s%x", i == from ? "" : ",", (u32)f->getFlag(i));
		s += buf;
	}
	return s;
}

void sampleFlags(long long t)
{
	if (TFlagManager::getInstance() == nullptr)
		return;
	std::fprintf(stderr, "[sync-f] t=%lld cb=%s ci=%s sb=%s si=%s\n", t, bits(0x10000, MSF_CARD_BOOL_END).c_str(),
	             ints(0x20000, MSF_CARD_INT_END).c_str(), bits(0x50000, MSF_STAGE_BOOL_END).c_str(),
	             ints(0x60000, MSF_STAGE_INT_END).c_str());
}

void samplePlayers(long long t)
{
	if (TMario* m = gpMarioOriginal) {
		const bool spray = m->checkFlag(MARIO_FLAG_HAS_FLUDD) && m->mWaterGun != nullptr && m->mWaterGun->isEmitting();
		std::fprintf(stderr, "[sync-me] t=%lld pos=%.0f,%.0f,%.0f spray=%d\n", t, m->mPosition.x, m->mPosition.y,
		             m->mPosition.z, spray ? 1 : 0);
	}
	const double now = port_mp_time_ms();
	for (int slot = 0; slot < PORT_MP_MAX_PLAYERS; slot++) {
		if (slot == port_mp_local_slot())
			continue;
		PortMpSnapshotRx rx;
		if (port_mp_get_snapshots(slot, &rx, 1) < 1)
			continue;
		std::fprintf(stderr, "[sync-rp] t=%lld slot=%d pos=%.0f,%.0f,%.0f spray=%d st=%d/%d age=%.0f\n", t, slot,
		             rx.snap.pos[0], rx.snap.pos[1], rx.snap.pos[2], (rx.snap.flags & PORT_MP_SNAP_SPRAYING) ? 1 : 0,
		             rx.snap.stage, rx.snap.scenario, now - rx.rxMs);
	}
}

void gameFrame()
{
	static const bool sOn = std::getenv("SMS_PORT_MP_SYNCCHECK") != nullptr;
	if (!sOn || !port_mp_active() || gpMarDirector == nullptr || !gpMarDirector->mSetupDone)
		return;
	static long long sLastSecond;
	const long long sec = unixMs() / 1000;
	if (sec == sLastSecond)
		return;
	sLastSecond      = sec;
	const long long t = sec * 1000;
	TApplication* app = SMSGetApplication();
	std::fprintf(stderr, "[sync] t=%lld slot=%d st=%d/%d\n", t, port_mp_local_slot(), app->mCurrArea.getStage(),
	             app->mCurrArea.getScenario());
	samplePlayers(t);
	sampleActors(t);
	samplePollution(t);
	sampleFlags(t);
}

struct Register {
	Register() { port_mp_register_game_frame(gameFrame); }
} sRegister;

} // namespace

#endif // SMS_PORT_AURORA
