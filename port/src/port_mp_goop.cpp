// Multiplayer goop (pollution) cleaning sync, game side (w6; MULTIPLAYER.md "Enemy and object
// sync (P8)", item 4). Lobby-panel section: port_mp_goop_ui.cpp.
//
// How the game cleans: water hits call TPollutionManager::clean/stamp, which queue a "clean"
// stamp task (TPollutionTexStamp::pushTask, stamp type 0) per layer; once per frame
// TPollutionCounterLayer draws each layer's map, the queued stamps and the revival stamps into
// the EFB and copies the result back over the layer's map (GXCopyTex; w8's port_efb.cpp reads it
// back into memory), and the pixel counters give each layer's degree (mCounter, cleanedAll).
//
// Sync: every client keeps simulating its own goop. The clean stamps a client queues itself
// (its own Mario's water, its own brushes) are recorded through one SMS_PORT_AURORA hook in
// TPollutionTexStamp::pushTask and sent to the players in the same stage + scenario (reliable,
// batched every 3 game frames). Receivers queue the same tasks
// with the game's own TPollutionCounterLayer::pushStampTask, so they are drawn, copied, read
// back and counted exactly like local water hits: no CPU writes to the maps, no change to the
// EFB/readback path, and the layer counters (and so cleanedAll / the stage's clean event)
// converge. Polluting stamps are sent only for the goop that spreads by itself near a Mario
// (TPollutionLayer::spread, flagged by a second hook) and, for P8, while gPortMpGoopForward is
// set (an enemy this client owns); goop from enemies is otherwise each client's own world.
// Late join: a player who enters a stage where others already are asks the lowest such slot
// for a snapshot: per layer, row runs of "still polluted" texels (value above the layer's
// polluted threshold), varint run lengths. Texels clean there but polluted here are cleared in
// memory (the CPU write; port_efb.cpp notices a CPU-rewritten copy destination and draws
// from memory from then on, as on hardware).
// Reconciliation: when cleaning in our stage has been quiet for 2 s after a burst, every peer
// sends the same snapshot to the others, who clear what is clean there, but only inside the
// 8x8-texel tiles cleaned this visit (union of the cleaning; each client's enemy goop
// elsewhere is left alone). Timing differences (a stamp drawn in another frame relative to the
// decay of faint texels) leave a few dozen texels apart; this closes them.
//
// Host setting PortMpSessionSettings.reserved[5]: 0 = On (default; w15: was Auto, on only with
// "Sync enemies: Experimental"), 1 = On, 2 = Off. Nothing runs outside a session (registered
// game-frame callbacks are only called during one) and the hook flag stays 0.
//
// Wire (PORT_MP_CH_GOOP = 16, the first free channel; header {u8 version 1, u8 type, u8 stage,
// u8 scenario}):
//   type 1 STAMPS   + u16 count, then the pushTask arguments (stamp index, layer, size, x, z,
//                   depth) delta-coded per task (see encodeEntry), reliable, to everyone
//   type 2 SNAP_REQ reliable, to one slot
//   type 3 SNAP     + {u8 layer, u8 flags (bit 0: last chunk of the last layer, bit 1:
//                   reconciliation), u16 width, u16 height, u16 row0, u16 rows} + per row varint
//                   runs alternating clean / polluted, starting with clean; reliable, to the
//                   requester (late join) or to everyone (reconciliation)
#ifdef SMS_PORT_AURORA

#include "port_mp.h"

#include <Map/PollutionCount.hpp>
#include <Map/PollutionLayer.hpp>
#include <Map/PollutionManager.hpp>
#include <Player/Mario.hpp>
#include <System/MarDirector.hpp>
#include <JSystem/JDrama/JDRNameRefGen.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <vector>

extern "C" {
int gPortMpGoopHook   = 0; // read by TPollutionTexStamp::pushTask (PollutionCount.cpp)
int gPortMpGoopSpread = 0; // set by TPollutionLayer::spread around its pollute call (PollutionAction.cpp)
// For P8 (w11): increment around the control of an enemy this client owns (decrement after),
// so its goop (pollute stamps) is sent to the others too; puppets skip moveObject, stamp nothing.
int gPortMpGoopForward = 0;
u32 port_frame_count(void); // port_crash.cpp
}

namespace {

const uint8_t kChannel   = PORT_MP_CH_GOOP;
const uint8_t kVersion   = 1;
const int kResByte       = 5; // PortMpSessionSettings.reserved[5]
enum { T_STAMPS = 1, T_SNAP_REQ = 2, T_SNAP = 3 };

#pragma pack(push, 1)
struct Hdr {
	uint8_t version, type, stage, scenario;
};
struct Entry {
	uint8_t stamp, layer;
	uint16_t size, x, z;
	int16_t y;
	bool operator==(const Entry& o) const { return std::memcmp(this, &o, sizeof(Entry)) == 0; }
};
struct SnapHdr {
	uint8_t layer, last;
	uint16_t width, height, row0, rows;
};
#pragma pack(pop)
static_assert(sizeof(Entry) == 10, "entry");

// The pollution manager of the running stage, validated once per stage (gpPollution is set
// by a scene that has one and never cleared, like gpConductor).
TPollutionManager* sMgr;
TMarDirector* sDirector;
int sStage = -1, sScenario = -1;
u32 sStageFrames;
bool sInjecting;

std::vector<Entry> sOut;     // our clean stamps not sent yet
std::deque<Entry> sIn;       // received, not yet queued into the game
u32 sLastFlush;
// reconciliation: after a burst of cleaning in our stage (stamps sent or received) has been
// quiet for kQuietFrames, every peer sends its maps to the others once; each applies the texels
// clean there (union of clean), so leftovers from timing differences cannot keep one peer
// short of the stage's clean event.
const u32 kQuietFrames = 120;
u32 sLastActivity;
bool sReconPending;
unsigned sRecons;
// late join
int sSnapState; // 0 none, 1 requested, 2 received / nobody to ask
u32 sSnapAskedAt;
int sSnapTries;
// Areas cleaned this stage visit (own or received clean stamps), per layer in 8x8-texel tiles.
// A reconciliation snapshot only clears texels inside them: elsewhere the peers' maps may
// legitimately differ (goop from each client's own enemies), and that must not be erased.
std::vector<std::vector<uint8_t>> sTouched;
// stats
unsigned sSent, sRecv, sInjected, sDropped, sSnapCells, sSnapsServed, sBytes, sSpreadSent, sPolluteLocal;

void markTouched(int layer, int x, int z, int size)
{
	if (sMgr == nullptr || layer < 0 || layer >= sMgr->getJointModelNum())
		return;
	TPollutionLayer* L = sMgr->getLayer(layer);
	const int tw = (L->mPos.getWidth() + 7) / 8, th = (L->mPos.getHeight() + 7) / 8;
	if ((int)sTouched.size() <= layer)
		sTouched.resize(layer + 1);
	std::vector<uint8_t>& t = sTouched[layer];
	if ((int)t.size() != tw * th)
		t.assign(tw * th, 0);
	const int r = size + 8; // generous: the stamp's footprint plus a tile
	for (int ty = (z - r) / 8; ty <= (z + r) / 8; ty++)
		for (int tx = (x - r) / 8; tx <= (x + r) / 8; tx++)
			if (tx >= 0 && ty >= 0 && tx < tw && ty < th)
				t[ty * tw + tx] = 1;
}

bool touched(int layer, int s, int t)
{
	if (layer >= (int)sTouched.size() || sTouched[layer].empty())
		return false;
	const int tw = (sMgr->getLayer(layer)->mPos.getWidth() + 7) / 8;
	return sTouched[layer][(t / 8) * tw + s / 8] != 0;
}

bool envOn(const char* n)
{
	const char* v = std::getenv(n);
	return v != nullptr && *v != '\0' && *v != '0';
}

bool syncEnabled()
{
	PortMpSessionSettings s;
	port_mp_get_settings(&s);
	const uint8_t v = s.reserved[kResByte];
	return v != 2; // w15: 0 (default) and 1 = on, 2 = off (was: 0 = only with enemy sync)
}

void resetStage()
{
	sMgr      = nullptr;
	sOut.clear();
	sIn.clear();
	sSnapState = 0;
	sSnapTries = 0;
	sStageFrames = 0;
	sTouched.clear();
	sReconPending = false;
}

// Revalidates sMgr when the stage changes.
void trackStage()
{
	TMarDirector* d = gpMarDirector;
	if (d == nullptr || !d->mSetupDone) {
		if (sDirector != nullptr)
			resetStage();
		sDirector = nullptr;
		return;
	}
	if (d != sDirector || d->mMap != sStage || d->unk7D != sScenario) {
		resetStage();
		sDirector = d;
		sStage    = d->mMap;
		sScenario = d->unk7D;
		JDrama::TNameRefGen* gen = JDrama::TNameRefGen::getInstance();
		JDrama::TNameRef* found  = nullptr;
		if (gpPollution != nullptr && gen != nullptr && gen->getRootNameRef() != nullptr)
			found = gen->getRootNameRef()->search("\x97\x8E\x8F\x91\x82\xAB\x8A\xC7\x97\x9D"); // 落書き管理
		if (gpPollution != nullptr && found == (JDrama::TNameRef*)gpPollution && gpPollution->getJointModelNum() > 0
		    && gpPollution->getCounterLayer().mTexStamps != nullptr)
			sMgr = gpPollution;
		std::fprintf(stderr, "[mp] goop: stage %d/%d, %d pollution layers\n", sStage, sScenario,
		             sMgr != nullptr ? sMgr->getJointModelNum() : 0);
	}
	++sStageFrames;
}

void send(const void* data, int len, int to)
{
	port_mp_send(kChannel, data, len, 1, to);
}

void putVar(std::vector<uint8_t>& o, uint32_t v);
bool getVar(const uint8_t*& p, const uint8_t* e, uint32_t& v);
uint32_t zig(int32_t v) { return ((uint32_t)v << 1) ^ (uint32_t)(v >> 31); }
int32_t unzig(uint32_t v) { return (int32_t)(v >> 1) ^ -(int32_t)(v & 1); }

// STAMPS body: u16 count, then per task a flag byte (bit 0: stamp index follows as u8, bit 1:
// layer follows as u8, bit 2: size follows as varint) and zigzag-varint deltas of x, z and depth
// from the previous task (the first from 0). A spray stream is one stamp type on one layer with
// nearby positions, so a task takes about 4 bytes instead of 10.
void encodeEntry(std::vector<uint8_t>& o, const Entry& e, Entry& prev)
{
	const uint8_t f = (uint8_t)((e.stamp != prev.stamp ? 1 : 0) | (e.layer != prev.layer ? 2 : 0)
	                            | (e.size != prev.size ? 4 : 0));
	o.push_back(f);
	if (f & 1)
		o.push_back(e.stamp);
	if (f & 2)
		o.push_back(e.layer);
	if (f & 4)
		putVar(o, e.size);
	putVar(o, zig((int32_t)e.x - prev.x));
	putVar(o, zig((int32_t)e.z - prev.z));
	putVar(o, zig((int32_t)e.y - prev.y));
	prev = e;
}

bool decodeEntry(const uint8_t*& p, const uint8_t* end, Entry& e, Entry& prev)
{
	if (p >= end)
		return false;
	const uint8_t f = *p++;
	e               = prev;
	uint32_t v;
	if (f & 1) {
		if (p >= end)
			return false;
		e.stamp = *p++;
	}
	if (f & 2) {
		if (p >= end)
			return false;
		e.layer = *p++;
	}
	if ((f & 4) && getVar(p, end, v))
		e.size = (uint16_t)v;
	if (!getVar(p, end, v))
		return false;
	e.x = (uint16_t)(prev.x + unzig(v));
	if (!getVar(p, end, v))
		return false;
	e.z = (uint16_t)(prev.z + unzig(v));
	if (!getVar(p, end, v))
		return false;
	e.y  = (int16_t)(prev.y + unzig(v));
	prev = e;
	return true;
}

void flush()
{
	size_t i = 0;
	while (i < sOut.size()) {
		std::vector<uint8_t> body;
		Entry prev = {};
		uint16_t n = 0;
		const size_t room = PORT_MP_APP_MAX - sizeof(Hdr) - 2 - 16;
		while (i < sOut.size() && body.size() < room) {
			encodeEntry(body, sOut[i++], prev);
			++n;
		}
		uint8_t buf[PORT_MP_APP_MAX];
		Hdr h = { kVersion, T_STAMPS, (uint8_t)sStage, (uint8_t)sScenario };
		std::memcpy(buf, &h, sizeof(h));
		std::memcpy(buf + sizeof(h), &n, 2);
		std::memcpy(buf + sizeof(h) + 2, body.data(), body.size());
		send(buf, (int)(sizeof(h) + 2 + body.size()), -1);
		sSent += n;
		sLastActivity = port_frame_count();
		sReconPending = true;
		sBytes += (unsigned)(sizeof(h) + 2 + body.size());
	}
	sOut.clear();
}

// Queues received stamps into the game (the counter layer takes up to its capacity per frame).
void inject()
{
	if (sMgr == nullptr || sIn.empty())
		return;
	TPollutionCounterLayer& cl = sMgr->getCounterLayer();
	sInjecting = true;
	int n      = 0;
	while (!sIn.empty() && n < 96) {
		const Entry e = sIn.front();
		if (e.stamp >= cl.mTexStampNum || e.layer >= cl.mCounterNum) {
			sIn.pop_front();
			++sDropped;
			continue;
		}
		TPollutionTexStamp& ts = cl.mTexStamps[e.stamp];
		if (ts.mTaskNum >= ts.mTaskCapacity - 16) // leave room for our own water this frame
			break;
		cl.pushStampTask(e.stamp, e.layer, e.size, e.x, e.z, e.y);
		if (cl.stampIsCleanType(e.stamp))
			markTouched(e.layer, e.x, e.z, e.size);
		sIn.pop_front();
		++sInjected;
		++n;
	}
	sInjecting = false;
}

void putVar(std::vector<uint8_t>& o, uint32_t v)
{
	while (v >= 0x80) {
		o.push_back((uint8_t)(v | 0x80));
		v >>= 7;
	}
	o.push_back((uint8_t)v);
}

bool getVar(const uint8_t*& p, const uint8_t* e, uint32_t& v)
{
	v = 0;
	for (int s = 0; s < 35 && p < e; s += 7) {
		const uint8_t b = *p++;
		v |= (uint32_t)(b & 0x7F) << s;
		if ((b & 0x80) == 0)
			return true;
	}
	return false;
}

// Snapshot of every layer's "still polluted" texels to one slot.
// recon: a reconciliation snapshot (applied only inside the receivers' cleaned areas).
void serveSnapshot(int to, bool recon)
{
	if (sMgr == nullptr)
		return;
	const int layers = sMgr->getJointModelNum();
	for (int l = 0; l < layers; l++) {
		TPollutionLayer* L = sMgr->getLayer(l);
		if (L == nullptr || L->mPollutionMap == nullptr)
			continue;
		const TPollutionPos& pos = L->mPos;
		const int w = pos.getWidth(), h = pos.getHeight();
		const u8 thr = L->mPollutedThreshold;
		std::vector<uint8_t> rowsData;
		int row0 = 0;
		auto emit = [&](int rowEnd, bool last) {
			uint8_t buf[PORT_MP_APP_MAX];
			Hdr hd     = { kVersion, T_SNAP, (uint8_t)sStage, (uint8_t)sScenario };
			SnapHdr sh = { (uint8_t)l, (uint8_t)((last ? 1 : 0) | (recon ? 2 : 0)), (uint16_t)w, (uint16_t)h, (uint16_t)row0,
				           (uint16_t)(rowEnd - row0) };
			std::memcpy(buf, &hd, sizeof(hd));
			std::memcpy(buf + sizeof(hd), &sh, sizeof(sh));
			std::memcpy(buf + sizeof(hd) + sizeof(sh), rowsData.data(), rowsData.size());
			send(buf, (int)(sizeof(hd) + sizeof(sh) + rowsData.size()), to);
			rowsData.clear();
			row0 = rowEnd;
		};
		for (int t = 0; t < h; t++) {
			std::vector<uint8_t> row;
			bool polluted = false;
			int run       = 0;
			for (int s = 0; s < w; s++) {
				const bool p = L->mPollutionMap[pos.index(s, t)] > thr;
				if (p != polluted) {
					putVar(row, (uint32_t)run);
					polluted = p;
					run      = 0;
				}
				++run;
			}
			putVar(row, (uint32_t)run);
			if (rowsData.size() + row.size() > PORT_MP_APP_MAX - sizeof(Hdr) - sizeof(SnapHdr) - 8)
				emit(t, false);
			rowsData.insert(rowsData.end(), row.begin(), row.end());
		}
		emit(h, l == layers - 1);
	}
	++sSnapsServed;
	std::fprintf(stderr, "[mp] goop: %s of %d layers sent to slot %d\n", recon ? "reconciliation snapshot" : "snapshot",
	             layers, to);
}

void applySnapshot(const uint8_t* p, int n)
{
	if (sMgr == nullptr || n < (int)sizeof(SnapHdr))
		return;
	SnapHdr sh;
	std::memcpy(&sh, p, sizeof(sh));
	p += sizeof(sh);
	const uint8_t* e = p + (n - sizeof(sh));
	if (sh.layer >= sMgr->getJointModelNum())
		return;
	TPollutionLayer* L = sMgr->getLayer(sh.layer);
	if (L == nullptr || L->mPollutionMap == nullptr || sh.width != L->mPos.getWidth() || sh.height != L->mPos.getHeight())
		return;
	const TPollutionPos& pos = L->mPos;
	const u8 thr     = L->mPollutedThreshold;
	const bool recon = (sh.last & 2) != 0;
	unsigned cleared = 0;
	for (int t = sh.row0; t < sh.row0 + sh.rows && t < sh.height; t++) {
		int s         = 0;
		bool polluted = false;
		while (s < sh.width) {
			uint32_t run;
			if (!getVar(p, e, run))
				return;
			if (!polluted)
				for (uint32_t k = 0; k < run && s + (int)k < sh.width; k++) {
					if (recon && !touched(sh.layer, s + (int)k, t))
						continue;
					u8& v = L->mPollutionMap[pos.index(s + (int)k, t)];
					if (v > thr) {
						v = 0;
						++cleared;
					}
				}
			s += (int)run;
			polluted = !polluted;
		}
	}
	sSnapCells += cleared;
	if (sh.last & 1) {
		if (!recon)
			sSnapState = 2;
		std::fprintf(stderr, "[mp] goop: snapshot applied, %u texels cleaned so far\n", sSnapCells);
	}
}

void receive()
{
	uint8_t buf[PORT_MP_APP_MAX];
	int from, n;
	while ((n = port_mp_recv(kChannel, buf, sizeof(buf), &from)) != 0) {
		if (n < (int)sizeof(Hdr))
			continue;
		Hdr h;
		std::memcpy(&h, buf, sizeof(h));
		if (h.version != kVersion || sMgr == nullptr || !gPortMpGoopHook || h.stage != sStage
		    || h.scenario != sScenario)
			continue; // not for our stage visit (or sync off)
		const uint8_t* p = buf + sizeof(h);
		const int rest   = n - (int)sizeof(h);
		if (h.type == T_STAMPS && rest >= 2) {
			uint16_t cnt;
			std::memcpy(&cnt, p, 2);
			const uint8_t* q   = p + 2;
			const uint8_t* end = p + rest;
			Entry prev = {}, e;
			for (int i = 0; i < cnt && decodeEntry(q, end, e, prev); i++) {
				sIn.push_back(e);
				++sRecv;
				sLastActivity = port_frame_count();
				sReconPending = true;
			}
			while (sIn.size() > 20000) { // far behind (should not happen): keep the newest
				sIn.pop_front();
				++sDropped;
			}
		} else if (h.type == T_SNAP_REQ) {
			serveSnapshot(from, false);
		} else if (h.type == T_SNAP) {
			applySnapshot(p, rest);
		}
	}
}

// Late join: ask the lowest other slot in our stage for its maps, once per stage visit.
void lateJoin()
{
	if (sSnapState == 2 || sMgr == nullptr || sStageFrames < 90)
		return;
	if (sSnapState == 1 && port_frame_count() - sSnapAskedAt < 300)
		return;
	if (sSnapTries >= 3) {
		sSnapState = 2;
		return;
	}
	int leader = -1;
	for (int i = 0; i < PORT_MP_MAX_PLAYERS; i++)
		if (i != port_mp_local_slot() && port_mp_same_stage(i)) {
			leader = i;
			break;
		}
	if (leader < 0) {
		sSnapState = 2; // first one here: our map is the stage's
		return;
	}
	Hdr h = { kVersion, T_SNAP_REQ, (uint8_t)sStage, (uint8_t)sScenario };
	send(&h, sizeof(h), leader);
	sSnapState   = 1;
	sSnapAskedAt = port_frame_count();
	++sSnapTries;
	std::fprintf(stderr, "[mp] goop: asked slot %d for the stage's goop\n", leader);
}

void debugKnobs()
{
	// SMS_PORT_MP_GOOP=1|2|0 (host, debug): set reserved[5] once at the start (1 On, 2 Off, 0 Auto).
	static bool sDone;
	const char* v = std::getenv("SMS_PORT_MP_GOOP");
	if (sDone || v == nullptr || !port_mp_is_host())
		return;
	sDone = true;
	PortMpSessionSettings s;
	port_mp_get_settings(&s);
	s.reserved[kResByte] = (uint8_t)std::atoi(v);
	port_mp_host_set_settings(&s);
	std::fprintf(stderr, "[mp] goop: SMS_PORT_MP_GOOP: reserved[5] = %u\n", s.reserved[kResByte]);
}

void log()
{
	static const bool sLog = envOn("SMS_PORT_MP_GOOP_LOG");
	if (!sLog || sMgr == nullptr || (sStageFrames % 150) != 0)
		return;
	char t[512];
	int o = std::snprintf(t, sizeof(t), "[mp] goop: stage %d/%d frame %u, sync %d, sent %u recv %u injected %u dropped %u, "
	                      "snapshot cells %u, degrees", sStage, sScenario, port_frame_count(), gPortMpGoopHook, sSent,
	                      sRecv, sInjected, sDropped, sSnapCells);
	for (int l = 0; l < sMgr->getJointModelNum() && o < (int)sizeof(t) - 16; l++)
		o += std::snprintf(t + o, sizeof(t) - o, " %u", (unsigned)sMgr->getLayer(l)->mCounter);
	std::fprintf(stderr, "%s, total %u, bytes sent %u, spread sent %u, local pollute %u, revival stamps %u\n", t,
	             (unsigned)sMgr->getPollutionDegree(), sBytes, sSpreadSent, sPolluteLocal,
	             (unsigned)sMgr->getCounterLayer().mRevivalTexStampNum);
}

// SMS_PORT_MP_GOOP_DUMP=<frame> (debug): at that presented frame, write every layer's map
// (memory, untiled, grey = goop value) as goop_<stage>_<scenario>_<layer>.bmp in the working
// directory, to compare the peers' maps texel by texel.
void dumpMaps()
{
	static const char* sEnv = std::getenv("SMS_PORT_MP_GOOP_DUMP");
	static bool sDone;
	if (sEnv == nullptr || sDone || sMgr == nullptr || port_frame_count() < (u32)std::atoi(sEnv))
		return;
	sDone = true;
	for (int l = 0; l < sMgr->getJointModelNum(); l++) {
		TPollutionLayer* L = sMgr->getLayer(l);
		if (L == nullptr || L->mPollutionMap == nullptr)
			continue;
		const int w = L->mPos.getWidth(), h = L->mPos.getHeight();
		char name[64];
		std::snprintf(name, sizeof(name), "goop_%d_%d_%d.bmp", sStage, sScenario, l);
		FILE* f = std::fopen(name, "wb");
		if (f == nullptr)
			continue;
		const int stride = (w * 3 + 3) & ~3;
		const uint32_t size = 54 + stride * h;
		uint8_t hdr[54] = { 'B', 'M' };
		std::memcpy(hdr + 2, &size, 4);
		const uint32_t off = 54, dib = 40, planesBpp = 1 | (24 << 16), img = stride * h;
		std::memcpy(hdr + 10, &off, 4);
		std::memcpy(hdr + 14, &dib, 4);
		std::memcpy(hdr + 18, &w, 4);
		std::memcpy(hdr + 22, &h, 4);
		std::memcpy(hdr + 26, &planesBpp, 4);
		std::memcpy(hdr + 34, &img, 4);
		std::fwrite(hdr, 1, 54, f);
		std::vector<uint8_t> row(stride, 0);
		for (int t = h - 1; t >= 0; t--) { // bottom-up
			for (int s = 0; s < w; s++)
				row[s * 3] = row[s * 3 + 1] = row[s * 3 + 2] = L->mPollutionMap[L->mPos.index(s, t)];
			std::fwrite(row.data(), 1, stride, f);
		}
		std::fclose(f);
	}
	std::fprintf(stderr, "[mp] goop: maps dumped at frame %u\n", port_frame_count());
}

// SMS_PORT_MP_GOOP_TEST=<slot>@<frame> (debug, short checks): at that presented frame the
// instance with that local slot puts its Mario next to the goop: the polluted texel of the
// first ground layer nearest the layer's middle, 250 units to the side.
void debugTeleport()
{
	static const char* sEnv = std::getenv("SMS_PORT_MP_GOOP_TEST");
	static bool sDone;
	if (sEnv == nullptr || sDone || sMgr == nullptr || gpMarioOriginal == nullptr)
		return;
	char* end;
	const long slot = std::strtol(sEnv, &end, 10);
	if (*end != '@' || slot != port_mp_local_slot() || port_frame_count() < (u32)std::atol(end + 1))
		return;
	sDone = true;
	for (int l = 0; l < sMgr->getJointModelNum(); l++) {
		TPollutionLayer* L = sMgr->getLayer(l);
		if (L == nullptr || L->mPollutionMap == nullptr || L->getPlaneType() != 0)
			continue;
		const int w = L->mPos.getWidth(), h = L->mPos.getHeight();
		int best = -1, bs = 0, bt = 0;
		for (int t = 0; t < h; t += 2)
			for (int s = 0; s < w; s += 2)
				if (L->mPollutionMap[L->mPos.index(s, t)] > L->mPollutedThreshold) {
					const int d = (s - w / 2) * (s - w / 2) + (t - h / 2) * (t - h / 2);
					if (best < 0 || d < best)
						best = d, bs = s, bt = t;
				}
		if (best < 0)
			continue;
		JGeometry::TVec3<f32> p(L->getWorldPosX(bs) + 250.0f, L->mPos.getDepthWorld(bs, bt) + 50.0f,
		                        L->getWorldPosZ(bt));
		gpMarioOriginal->mPosition = p;
		std::fprintf(stderr, "[mp] goop: test: Mario put at %.0f %.0f %.0f (layer %d texel %d,%d)\n", p.x, p.y, p.z, l,
		             bs, bt);
		return;
	}
}

void goopFrame()
{
	debugKnobs();
	trackStage();
	const bool on   = sMgr != nullptr && syncEnabled();
	gPortMpGoopHook = on ? 1 : 0;
	receive(); // drains the channel even while off
	if (!on) {
		sOut.clear();
		sIn.clear();
		return;
	}
	inject();
	if (!sOut.empty() && port_frame_count() - sLastFlush >= 3) {
		flush();
		sLastFlush = port_frame_count();
	}
	lateJoin();
	if (sReconPending && port_frame_count() - sLastActivity >= kQuietFrames) {
		sReconPending = false;
		++sRecons;
		serveSnapshot(-1, true); // to every peer (others in other stages drop it)
	}
	log();
	dumpMaps();
	debugTeleport();
}

struct Register {
	Register() { port_mp_register_game_frame(goopFrame); }
} sRegister;

} // namespace

extern "C" {

// TPollutionTexStamp::pushTask (game thread): one queued stamp task.
void port_mp_goop_task(const void* stamp, u16 stamp_type, u8 layer_idx, u16 size, u16 x, u16 z, s16 y)
{
	if (sInjecting || sMgr == nullptr || !port_mp_active())
		return;
	if (stamp_type != 0 && !gPortMpGoopSpread && !gPortMpGoopForward) {
		++sPolluteLocal; // enemies, objects: from each client's own world, not sent
		return;
	}
	if (stamp_type != 0)
		++sSpreadSent;
	else
		markTouched(layer_idx, x, z, size);
	const TPollutionCounterLayer& cl = sMgr->getCounterLayer();
	const TPollutionTexStamp* base   = cl.mTexStamps;
	const ptrdiff_t idx              = (const TPollutionTexStamp*)stamp - base;
	if (idx < 0 || idx >= cl.mTexStampNum)
		return;
	// Every task, repeats included: a clean stamp lowers the goop by a step, it does not clear
	// it, so a stream hitting one spot many times cleans more (dropping repeats left receivers
	// about 20 % less clean in the first test).
	const Entry e = { (uint8_t)idx, layer_idx, size, x, z, y };
	if (sOut.size() < 4000)
		sOut.push_back(e);
}

// For the lobby section (port_mp_goop_ui.cpp).
void port_mp_goop_status(int* active, int* layers, unsigned* sent, unsigned* recv, unsigned* snapCells)
{
	*active    = gPortMpGoopHook;
	*layers    = sMgr != nullptr ? sMgr->getJointModelNum() : 0;
	*sent      = sSent;
	*recv      = sRecv;
	*snapCells = sSnapCells;
}

} // extern "C"

#endif // SMS_PORT_AURORA
