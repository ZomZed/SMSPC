// Multiplayer player interaction (task board P2, w8): sm64coopdx-style bumping, stomping,
// ground pounds and FLUDD knockback between the local Mario and the other players.
// Design and rules: port/MULTIPLAYER.md, "Player interaction (P2)".
//
// Every client stays authoritative for its own Mario:
//  - Bumping needs no messages. Each client pushes only its own Mario out of the other
//    players' bodies with the game's own TMario::keepDistance. In Full mode that includes the
//    game's "clash": running into someone above the clash speed knocks you back. In Solid
//    mode the push is the same without the clash.
//  - Stomps, ground pounds and FLUDD hits are detected by the attacker against the other
//    player as drawn (the received snapshots interpolated 100 ms behind, like w15's
//    puppets). The attacker's own reaction is the game's: TMario::trampleExec bounces off a
//    proxy hit actor exactly as off an enemy. Then a "hit" message (attacker position, kind,
//    the attacker's logic tick) goes to the victim on PORT_MP_CH_INTERACT (the host relays it).
//  - The victim applies the knockback to its own Mario with the game's
//    TMario::damageExec and damage 0: knock-down animation, voice, particles, camera shake,
//    invincibility frames, no health loss.
// Host settings (PortMpSessionSettings.reserved, see port_mp.h / MULTIPLAYER.md):
//   reserved[0] interaction mode: 0 = Full (default), 1 = Solid only, 2 = Off
//   reserved[4] FLUDD pushes players: 0 = yes (default), 1 = no
// Inert unless a multiplayer session runs (the game-frame hook is only called then).
#include "port_mp.h"
#include <Player/Mario.hpp>
#include <Player/MarioFlags.hpp>
#include <Player/MarioStatus.hpp>
#include <Player/ModelWaterManager.hpp>
#include <Player/WaterGun.hpp>
#include <Strategic/HitActor.hpp>
#include <System/MarDirector.hpp>
#include <Map/MapCollisionData.hpp>
#include <JSystem/JMath.hpp>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" u32 port_frame_count(void); // port_main.cpp

// w15's last captured local state (port_mp_last_local_state): its seq is the snapshot tick
// the other players see, so a hit can carry it.
#include "port_mp_puppet.h"

extern "C" int port_mp_prop_running(void); // port_mp_prophunt.cpp
extern "C" int port_mp_manhunt_protect(int slot); // port_mp_manhunt.cpp
extern "C" void port_mp_shinerun_hit(int fromSlot); // port_mp_shinerun.cpp

namespace {

enum InteractMode { MODE_FULL = 0, MODE_SOLID = 1, MODE_OFF = 2 };
enum HitKind : u8 { HIT_STOMP = 1, HIT_POUND = 2, HIT_WATER = 3 };
const char* kindName(int k) { return k == HIT_STOMP ? "stomp" : k == HIT_POUND ? "ground pound" : k == HIT_WATER ? "FLUDD" : "?"; }

#pragma pack(push, 1)
struct HitMsg {
	u8 version; // 1
	u8 kind;    // HitKind
	u8 victim;  // slot the attacker aimed at
	u8 pad;
	u32 tick;   // attacker's snapshot tick when it hit
	f32 pos[3]; // attacker's Mario position
	s16 yaw;    // attacker's facing (fallback direction when right above the victim)
	s16 pad2;
};
#pragma pack(pop)
static_assert(sizeof(HitMsg) == 24, "HitMsg layout");

// Stand-in hit actor: lets the game's own trample code bounce off another player, and gives
// damageExec a position to be knocked away from. Never registered with the hit check.
class TMpInteractProxy : public THitActor {
public:
	TMpInteractProxy()
	    : THitActor("mp interaction proxy")
	    , mLastMessage(~0u)
	{
	}
	virtual BOOL receiveMessage(THitActor*, u32 message)
	{
		mLastMessage = message;
		return message == HIT_MESSAGE_TRAMPLE || message == HIT_MESSAGE_HIP_DROP;
	}
	u32 mLastMessage;
};
TMpInteractProxy sProxy;

const bool sLog = true; // hits are rare; always logged ("[mp-interact]")

struct Remote {
	bool valid;
	f32 pos[3];
	u16 flags;
	u32 tick;
	u32 action;
};

int sCooldown[PORT_MP_MAX_PLAYERS]; // attacker side: frames until this victim can be hit again
u32 sLatestTick[PORT_MP_MAX_PLAYERS];

void settings(int& mode, bool& fludd)
{
	PortMpSessionSettings s;
	port_mp_get_settings(&s);
	mode  = s.reserved[0] <= MODE_OFF ? s.reserved[0] : MODE_FULL;
	fludd = s.reserved[4] == 0;
}

// The other player's Mario as drawn: snapshots interpolated 100 ms behind the newest receive
// time (w15's puppets do the same), no extrapolation.
bool remotePos(int slot, Remote& out)
{
	out.valid = false;
	if (!port_mp_same_stage(slot))
		return false;
	PortMpSnapshotRx rx[8];
	const int n = port_mp_get_snapshots(slot, rx, 8);
	if (n <= 0)
		return false;
	const double now = port_mp_time_ms();
	if (now - rx[0].rxMs > 500.0)
		return false; // no fresh data: the puppet is about to hide
	const PortMpSnapshot& newest = rx[0].snap;
	sLatestTick[slot]            = newest.tick;
	out.flags                    = newest.flags;
	out.tick                     = newest.tick;
	out.action                   = newest.action;
	if (!(newest.flags & PORT_MP_SNAP_VALID) || (newest.flags & (PORT_MP_SNAP_HIDDEN | PORT_MP_SNAP_PAUSED)))
		return false;
	const double t = now - 100.0;
	int b          = -1; // newest index with rxMs <= t
	for (int i = 0; i < n; i++)
		if (rx[i].rxMs <= t) {
			b = i;
			break;
		}
	if (b <= 0) { // all older than t (use the newest) or all newer (use the oldest)
		const PortMpSnapshot& s = b == 0 ? newest : rx[n - 1].snap;
		std::memcpy(out.pos, s.pos, sizeof(out.pos));
	} else {
		const PortMpSnapshotRx& a = rx[b];     // older
		const PortMpSnapshotRx& c = rx[b - 1]; // newer
		const double span = c.rxMs - a.rxMs;
		const f32 f       = span > 0.0 ? (f32)((t - a.rxMs) / span) : 1.0f;
		f32 d2            = 0.0f;
		for (int k = 0; k < 3; k++)
			d2 += (c.snap.pos[k] - a.snap.pos[k]) * (c.snap.pos[k] - a.snap.pos[k]);
		for (int k = 0; k < 3; k++)
			out.pos[k] = d2 > 600.0f * 600.0f ? c.snap.pos[k] : a.snap.pos[k] + (c.snap.pos[k] - a.snap.pos[k]) * f;
	}
	out.valid = true;
	return true;
}

u32 localTick()
{
	PortMpPuppetState st;
	return port_mp_last_local_state(&st) != 0 ? st.seq : 0;
}

bool localPlayable(TMario* m)
{
	TMarDirector* d = gpMarDirector;
	if (m == nullptr || d == nullptr || !d->mSetupDone)
		return false;
	if (d->mState != TMarDirector::STATE_UNK4 || d->unk124 != 0)
		return false; // pause, talk, demo, save prompt
	if (m->checkFlag(MARIO_FLAG_GAME_OVER) || m->mHealth == 0)
		return false;
	return true;
}

void sendHit(int victim, HitKind kind, TMario* m)
{
	HitMsg h {};
	h.version = 1;
	h.kind    = kind;
	h.victim  = (u8)victim;
	h.tick    = localTick();
	h.pos[0]  = m->mPosition.x;
	h.pos[1]  = m->mPosition.y;
	h.pos[2]  = m->mPosition.z;
	h.yaw     = m->mFaceAngle.y;
	port_mp_send(PORT_MP_CH_INTERACT, &h, sizeof(h), 1, victim);
	if (sLog)
		std::fprintf(stderr, "[mp-interact] frame %u: %s on slot %d (tick %u, at %.0f %.0f %.0f)\n", port_frame_count(),
		             kindName(kind), victim, h.tick, h.pos[0], h.pos[1], h.pos[2]);
}

// Solid mode: keepDistance without the clash knock-back (the same push and wall/floor checks).
void pushOut(TMario* m, const f32* p, f32 radius)
{
	const f32 dx = m->mPosition.x - p[0], dz = m->mPosition.z - p[2];
	const f32 thresh = radius + m->unk15C;
	f32 dist         = std::sqrt(dx * dx + dz * dz);
	if (!(dist < thresh))
		return;
	f32 nx = 0.0f, nz = 1.0f;
	if (dist > 0.001f) {
		nx = dx / dist;
		nz = dz / dist;
	} else {
		nx = JMASSin(m->mFaceAngle.y);
		nz = JMASCos(m->mFaceAngle.y);
	}
	Vec newPos = { p[0] + thresh * nx, m->mPosition.y, p[2] + thresh * nz };
	m->checkWallPlane(&newPos, 60.0f, m->unk15C);
	f32 floorY;
	const TBGCheckData* ground;
	m->checkGroundPlane(newPos.x, newPos.y, newPos.z, &floorY, &ground);
	if (!ground->isLegal())
		return;
	f32 sx = newPos.x - m->mPosition.x, sz = newPos.z - m->mPosition.z;
	const f32 step = std::sqrt(sx * sx + sz * sz);
	if (step > 50.0f) {
		sx *= 50.0f / step;
		sz *= 50.0f / step;
	}
	m->mPosition.x += sx;
	m->mPosition.z += sz;
}

// Victim side: our own Mario's reaction to another player's hit.
void applyHit(int from, const HitMsg& h, int mode, bool fludd)
{
	TMario* m = gpMarioOriginal;
	const char* why = nullptr;
	if (h.version != 1 || h.kind < HIT_STOMP || h.kind > HIT_WATER)
		why = "bad message";
	else if (mode != MODE_FULL)
		why = "interaction is not Full";
	else if (h.kind == HIT_WATER && !fludd)
		why = "FLUDD pushes are off";
	else if (!localPlayable(m))
		why = "not in control";
	else if (!port_mp_same_stage(from))
		why = "attacker not in this stage";
	else if (sLatestTick[from] != 0 && h.tick != 0 && sLatestTick[from] > h.tick + 15)
		why = "stale (over 0.5 s old)";
	else {
		const f32 dx = h.pos[0] - m->mPosition.x, dy = h.pos[1] - m->mPosition.y, dz = h.pos[2] - m->mPosition.z;
		const f32 range = h.kind == HIT_WATER ? 2000.0f : 600.0f;
		if (dx * dx + dy * dy + dz * dz > range * range)
			why = "too far away";
	}
	if (why != nullptr) {
		if (sLog)
			std::fprintf(stderr, "[mp-interact] frame %u: %s from slot %d ignored: %s\n", port_frame_count(),
			             kindName(h.kind), from, why);
		return;
	}
	// Knocked away from the attacker's horizontal position; right above us, from where it faces.
	f32 ax = h.pos[0], az = h.pos[2];
	const f32 hx = ax - m->mPosition.x, hz = az - m->mPosition.z;
	if (hx * hx + hz * hz < 25.0f) {
		ax = m->mPosition.x - JMASSin(h.yaw) * 10.0f;
		az = m->mPosition.z - JMASCos(h.yaw) * 10.0f;
	}
	sProxy.mPosition.x = ax;
	sProxy.mPosition.y = h.pos[1];
	sProxy.mPosition.z = az;
	// damageExec(actor, damage, anim type (0 light, 1 short, 2 full knock-down), FLUDD water
	// lost, knock-back speed, rumble, dirt, invincibility frames)
	const u32 before = m->mStatus;
	const bool inv   = m->isInvincible();
	// P3 emotes (w6) hold the local Mario in MARIO_STATUS_NOMOTION, a demo status that refuses
	// normal status changes, so damageExec's knock-down would be skipped: back to WAIT first.
	if (m->mStatus == MARIO_STATUS_NOMOTION)
		m->changePlayerStatus(MARIO_STATUS_WAIT, 0, true);
	switch (h.kind) {
	case HIT_STOMP: m->damageExec(&sProxy, 0, 1, 0, 16.0f, 10, 0.0f, 45); break;
	case HIT_POUND: m->damageExec(&sProxy, 0, 2, 0, 24.0f, 20, 0.0f, 60); break;
	case HIT_WATER: m->damageExec(&sProxy, 0, 0, 0, 10.0f, 4, 0.0f, 30); break;
	}
	if (!inv)
		port_mp_shinerun_hit(from); // Shine Runners: a hit knocks a Shine out
	if (sLog)
		std::fprintf(stderr, "[mp-interact] frame %u: %s from slot %d: status %08x -> %08x%s, health %d\n",
		             port_frame_count(), kindName(h.kind), from, before, m->mStatus,
		             inv ? " (invincible: no effect)" : "", (int)m->mHealth);
}

// SMS_PORT_MP_INTERACT_TEST=<frame>:<what>[,<frame>:<what>...] (tests): at that presented
// frame, put the local Mario next to the first other player in this stage:
//   stomp: 300 above it (falls on its head)   pound: 400 above (press Z with PADSCRIPT)
//   spray: 250 beside it, facing it (hold R)  bump: 30 beside it (overlapping)
void debugTeleport(TMario* m)
{
	static const char* sTest = std::getenv("SMS_PORT_MP_INTERACT_TEST");
	static unsigned sLast;
	if (sTest == nullptr)
		return;
	const unsigned now = port_frame_count();
	for (const char* p = sTest; *p != '\0';) {
		char* e;
		const unsigned f = (unsigned)std::strtoul(p, &e, 10);
		if (e == p || *e != ':')
			break;
		const char* what = e + 1;
		const char* next = std::strchr(what, ',');
		const size_t len = next != nullptr ? (size_t)(next - what) : std::strlen(what);
		if (f > sLast && now >= f) {
			sLast = f;
			for (int s = 0; s < PORT_MP_MAX_PLAYERS; s++) {
				if (s == port_mp_local_slot())
					continue;
				Remote r;
				if (!remotePos(s, r))
					continue;
				f32 ox = 0.0f, oy = 0.0f, oz = 0.0f;
				if (len == 5 && std::strncmp(what, "stomp", 5) == 0)
					oy = 300.0f;
				else if (len == 5 && std::strncmp(what, "pound", 5) == 0)
					oy = 400.0f;
				else if (len == 5 && std::strncmp(what, "spray", 5) == 0)
					ox = 250.0f;
				else
					ox = 30.0f;
				m->mPosition.x = r.pos[0] + ox;
				m->mPosition.y = r.pos[1] + oy;
				m->mPosition.z = r.pos[2] + oz;
				m->mVel.x = m->mVel.z = 0.0f;
				m->mForwardVel        = 0.0f;
				if (ox != 0.0f) // face the other player (+x side: look towards -x)
					m->mFaceAngle.y = m->mModelFaceAngle = (s16)0xC000;
				std::fprintf(stderr, "[mp-interact] TEST frame %u: %.*s next to slot %d at %.0f %.0f %.0f\n", now,
				             (int)len, what, s, r.pos[0], r.pos[1], r.pos[2]);
				break;
			}
		}
		if (next == nullptr)
			break;
		p = next + 1;
	}
}

// SMS_PORT_MP_INTERACT=full|solid|off, SMS_PORT_MP_FLUDD_PUSH=0|1 (host, tests): applied once.
void debugHostSettings()
{
	static bool sDone;
	if (sDone || !port_mp_is_host())
		return;
	sDone             = true;
	const char* mode  = std::getenv("SMS_PORT_MP_INTERACT");
	const char* fludd = std::getenv("SMS_PORT_MP_FLUDD_PUSH");
	if (mode == nullptr && fludd == nullptr)
		return;
	PortMpSessionSettings s;
	port_mp_get_settings(&s);
	if (mode != nullptr)
		s.reserved[0] = std::strcmp(mode, "off") == 0 ? MODE_OFF : std::strcmp(mode, "solid") == 0 ? MODE_SOLID : MODE_FULL;
	if (fludd != nullptr)
		s.reserved[4] = std::atoi(fludd) != 0 ? 0 : 1;
	port_mp_host_set_settings(&s);
	std::fprintf(stderr, "[mp-interact] host settings: interaction %d (0 full, 1 solid, 2 off), FLUDD pushes %d\n",
	             s.reserved[0], s.reserved[4] == 0);
}

// Game thread, once per logic frame while a session runs.
void interactFrame()
{
	debugHostSettings();
	int mode;
	bool fludd;
	settings(mode, fludd);

	// Hits aimed at us (applied, or ignored with a reason, even in Off mode so queues drain).
	HitMsg h;
	int from;
	int n;
	while ((n = port_mp_recv(PORT_MP_CH_INTERACT, &h, sizeof(h), &from)) != 0) {
		if (n == (int)sizeof(h) && from >= 0 && from < PORT_MP_MAX_PLAYERS && h.victim == port_mp_local_slot())
			applyHit(from, h, mode, fludd);
	}

	TMario* m = gpMarioOriginal;
	if (!localPlayable(m))
		return;
	debugTeleport(m);
	if (mode == MODE_OFF)
		return;

	const f32 radius  = m->unk15C;                       // another Mario's body radius
	const f32 height  = m->getDamageHeight() > 0.0f ? m->getDamageHeight() : 160.0f;
	const int self    = port_mp_local_slot();
	const bool canAct = mode == MODE_FULL;
	for (int s = 0; s < PORT_MP_MAX_PLAYERS; s++) {
		if (sCooldown[s] > 0)
			--sCooldown[s];
		if (s == self)
			continue;
		Remote r;
		if (!remotePos(s, r))
			continue;
		const f32 dx = m->mPosition.x - r.pos[0], dz = m->mPosition.z - r.pos[2];
		const f32 dy = m->mPosition.y - r.pos[1];
		const f32 h2 = dx * dx + dz * dz;
		const f32 reach = radius * 2.0f;
		const bool overlapXZ = h2 < reach * reach;

		// Landing on its head: stomp (bounce like off an enemy) or ground pound.
		if (canAct && overlapXZ && sCooldown[s] == 0 && !port_mp_manhunt_protect(s) && m->checkStatusType(MARIO_STATUS_FLAG_JUMPING)
		    && m->mVel.y < 0.0f && dy > height * 0.5f && dy < height + 60.0f && !(r.flags & PORT_MP_SNAP_IN_WATER)) {
			if (m->mStatus == MARIO_STATUS_HIP_DROP) {
				sProxy.mPosition.set(r.pos[0], r.pos[1], r.pos[2]);
				sProxy.receiveMessage(m, HIT_MESSAGE_HIP_DROP);
				sendHit(s, HIT_POUND, m);
				sCooldown[s] = 20;
				continue;
			}
			sProxy.mPosition.set(r.pos[0], r.pos[1], r.pos[2]);
			if (m->trampleExec(&sProxy)) {
				sendHit(s, HIT_STOMP, m);
				sCooldown[s] = 20;
				continue;
			}
		}

		// Bodies overlap at the same height: push our own Mario out.
		if (overlapXZ && dy > -height && dy < height * 0.5f) {
			if (canAct)
				m->keepDistance(JGeometry::TVec3<f32>(r.pos[0], m->mPosition.y, r.pos[2]), radius, 0.0f);
			else
				pushOut(m, r.pos, radius);
		}

		// Our FLUDD water reaching it (the other players' water in the water manager is
		// flagged remote and skipped; w15, port_mp_water.cpp).
		// (Prop Hunt: port_mp_prophunt.cpp handles water itself: health, not a push)
		if (canAct && fludd && sCooldown[s] == 0 && gpModelWaterManager != nullptr && !port_mp_prop_running()) {
			const JGeometry::TVec3<f32>* pp = gpModelWaterManager->getParticlePositions();
			const f32 wr = TModelWaterManager::mStaticHitActor.getAttackRadius();
			const f32 wh = TModelWaterManager::mStaticHitActor.getAttackHeight();
			const int count = gpModelWaterManager->getParticleCount();
			for (int i = 0; i < count; i++) {
				if (!gpModelWaterManager->checkFlagBottom4Bits(i, 0x1)
				    || gpModelWaterManager->portIsRemote(i)) // w15: not ours
					continue;
				const JGeometry::TVec3<f32>& q = pp[i];
				if (q.y > r.pos[1] + height || q.y + wh < r.pos[1])
					continue;
				const f32 qx = q.x - r.pos[0], qz = q.z - r.pos[2];
				if (qx * qx + qz * qz >= (wr + radius) * (wr + radius))
					continue;
				sendHit(s, HIT_WATER, m);
				sCooldown[s] = 30;
				break;
			}
		}
	}
}

struct Register {
	Register() { port_mp_register_game_frame(interactFrame); }
} sRegister;

} // namespace
