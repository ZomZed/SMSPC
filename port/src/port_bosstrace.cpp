// port_bosstrace.cpp (w5, debug): SMS_PORT_BOSSTRACE=1 prints one line whenever
// a boss's nerve (state) changes, with its hit points and Mario's position, so a
// scripted boss fight (SMS_PORT_PADSCRIPT) can be followed without watching it.
// Called from TSpineBase::update (Strategic/Spine.hpp, under SMS_PORT). Names
// come from the PDB: the owner's vftable symbol gives the class, the nerve
// object's symbol gives the state. Read-only.
#include <Enemy/Enemy.hpp>
#include <Player/Mario.hpp>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" unsigned port_frame_count(void);
extern "C" int port_sym_name(const void* addr, char* out, size_t n); // port_symname.cpp

namespace {

bool symName(const void* addr, char* out, size_t n) { return port_sym_name(addr, out, n) != 0; }
struct VtEntry {
	const void* vt;
	bool boss;
	char name[64];
};
VtEntry gVt[256];
int gVtCount;

const VtEntry* classOf(const void* body)
{
	const void* vt = *(const void* const*)body;
	for (int i = 0; i < gVtCount; ++i)
		if (gVt[i].vt == vt)
			return &gVt[i];
	if (gVtCount >= 256)
		return nullptr;
	VtEntry& e = gVt[gVtCount++];
	e.vt       = vt;
	char full[256];
	symName(vt, full, sizeof(full));
	// "TBossPakkun::`vftable'" -> "TBossPakkun"
	char* c = strstr(full, "::`vftable'");
	if (c)
		*c = 0;
	snprintf(e.name, sizeof(e.name), "%s", full);
	static const char* kBoss[] = { "Boss", "Koopa", "Gesso", "Manta", "Eel", "Telesa", "Hanachan", "Pakkun" };
	static const char* sFilter = getenv("SMS_PORT_BOSSTRACE");
	e.boss = false;
	if (sFilter && strlen(sFilter) > 1) {
		e.boss = strstr(e.name, sFilter) != nullptr; // SMS_PORT_BOSSTRACE=<class substring>
	} else {
		for (const char* k : kBoss)
			if (strstr(e.name, k))
				e.boss = true;
	}
	return &e;
}

struct SpineEntry {
	const void* spine;
	const void* nerve;
	int hp;
};
SpineEntry gSp[512];
int gSpCount;

} // namespace

// The boss PADSCRIPT's B/FB tokens steer by this (port_main.cpp): the spine whose
// class contains SMS_PORT_BOSS_TARGET, else the first boss spine seen. Updated on
// every spine update; stale after 10 frames.
extern "C" float gPortBossPos[3];
extern "C" float gPortBossRotY;
extern "C" unsigned gPortBossFrame;
float gPortBossPos[3];
float gPortBossRotY;
unsigned gPortBossFrame;
static const void* sPrimary;
// A boss's current weak spot to stomp (e.g. Petey's navel while he lies on his
// back), set by the boss code under SMS_PORT_BOSSTRACE; fresh for 10 frames.
extern "C" float gPortBossAux[3];
extern "C" unsigned gPortBossAuxFrame;
float gPortBossAux[3];
unsigned gPortBossAuxFrame;
// A goal to walk/jump to when no boss is active (a collectible Shine, Item.cpp).
extern "C" float gPortGoalPos[3];
extern "C" unsigned gPortGoalFrame;
float gPortGoalPos[3];
unsigned gPortGoalFrame;
// Which bot a combined script should run now (boss code decides): 0 = PULL, 1 = AIM.
extern "C" int gPortBossWant;
int gPortBossWant;
// 1 while gPortBossAux is a beak-like part that scores when released far enough.
extern "C" int gPortBossAuxIsBeak;
int gPortBossAuxIsBeak;
// Mecha-Bowser coaster (rocket.cpp): a bottle rocket sits on FLUDD's nozzle.
extern "C" int gPortRocketHeld;
extern "C" unsigned gPortRocketFrame;
int gPortRocketHeld;
unsigned gPortRocketFrame;
// Where the THROW bot throws (King Boo), or where the AIM bot stands to spray a point
// (gPortBossWant 5, Wiggler's sand sprouts).
extern "C" float gPortThrowAt[3];
float gPortThrowAt[3];
// Gelato's sand sprout bases (MapObjMamma.cpp, SMS_PORT_BOSSTRACE): x, y, z, height scale,
// state, refreshed every frame they run; the Wiggler bot raises one in his path.
extern "C" float gPortSand[48][5];
extern "C" unsigned gPortSandFrame[48];
float gPortSand[48][5];
unsigned gPortSandFrame[48];
// The pull length past which letting go scores (Gooper's mSLBeakLengthDamage; 0 = unknown).
extern "C" float gPortBossRelease;
float gPortBossRelease;

extern "C" void port_spine_trace(const void* spine, TSpineEnemy* body, const void* nerve)
{
	static const bool sOn = getenv("SMS_PORT_BOSSTRACE") != nullptr;
	if (!sOn || body == nullptr)
		return;
	const VtEntry* cls = classOf(body);
	if (cls == nullptr || !cls->boss)
		return;
	SpineEntry* e = nullptr;
	for (int i = 0; i < gSpCount; ++i)
		if (gSp[i].spine == spine)
			e = &gSp[i];
	if (e == nullptr) {
		if (gSpCount >= 512)
			return;
		e        = &gSp[gSpCount++];
		e->spine = spine;
		e->nerve = (const void*)1;
		e->hp    = -1;
	}
	{
		static const char* sTarget = getenv("SMS_PORT_BOSS_TARGET");
		const bool want = sTarget ? strstr(cls->name, sTarget) != nullptr : true;
		if (want && !body->checkLiveFlag(LIVE_FLAG_DEAD) && body->getHitPoints() > 0
		    && (sPrimary == nullptr || port_frame_count() - gPortBossFrame > 10))
			sPrimary = spine;
		// Several of the same boss (Phantamanta's splits): follow the one nearest Mario.
		if (want && spine != sPrimary && !body->checkLiveFlag(LIVE_FLAG_DEAD) && body->getHitPoints() > 0
		    && gpMarioOriginal != nullptr && port_frame_count() - gPortBossFrame <= 10) {
			const float bx = body->mPosition.x - gpMarioOriginal->mPosition.x;
			const float bz = body->mPosition.z - gpMarioOriginal->mPosition.z;
			const float px = gPortBossPos[0] - gpMarioOriginal->mPosition.x;
			const float pz = gPortBossPos[2] - gpMarioOriginal->mPosition.z;
			if (bx * bx + bz * bz < 0.5f * (px * px + pz * pz))
				sPrimary = spine;
		}
		// A beaten boss (no hit points left) is no longer tracked, so the bots go for the
		// Shine and the NB entries apply.
		if (spine == sPrimary && body->getHitPoints() > 0 && !body->checkLiveFlag(LIVE_FLAG_DEAD)) {
			gPortBossPos[0] = body->mPosition.x;
			gPortBossPos[1] = body->mPosition.y;
			gPortBossPos[2] = body->mPosition.z;
			gPortBossRotY   = body->mRotation.y;
			gPortBossFrame  = port_frame_count();
		}
	}
	int hp = body->getHitPoints();
	if (nerve == e->nerve && hp == e->hp)
		return;
	e->nerve = nerve;
	e->hp    = hp;
	char nn[256];
	if (nerve) {
		symName(*(const void* const*)nerve, nn, sizeof(nn)); // the nerve's vftable
		if (char* c = strstr(nn, "::`vftable'"))
			*c = 0;
	}
	else
		snprintf(nn, sizeof(nn), "(none)");
	const TMario* m = gpMarioOriginal;
	fprintf(stderr, "[boss] f%u %s %p nerve %s hp %d pos %.0f %.0f %.0f ry %.0f | mario %.0f %.0f %.0f\n",
	        port_frame_count(), cls->name, (const void*)body, nn, hp, body->mPosition.x, body->mPosition.y,
	        body->mPosition.z, body->mRotation.y, m ? m->mPosition.x : 0.0f, m ? m->mPosition.y : 0.0f, m ? m->mPosition.z : 0.0f);
}

// SMS_PORT_PADSCRIPT "AIM<d>;<deg>;<gun>" (debug, w5): a small spray bot for
// boss fights. Each frame it either walks Mario to the point <d> units from the
// boss at <deg> from the boss's facing, turns him toward the boss, or (in place
// and facing it) holds R to squat and spray, pumping the trigger every 100
// frames and tilting the stick until the nozzle pitch reaches <gun> (s16 units,
// 0x2000 = 45 degrees up). port_main.cpp applies the result.
#include <Player/WaterGun.hpp>
#include <Player/NozzleBase.hpp>
#include <Player/MarioStatus.hpp>
#include <math.h>
extern "C" int port_boss_bot(int d, int deg, int gun, unsigned now, int* mode, int* gx, int* gz, int* stickY)
{
	*mode   = 0;
	*stickY = 0;
	const TMario* m = gpMarioOriginal;
	if (m == nullptr)
		return 0;
	if (now - gPortBossFrame >= 10) {
		// No boss: go for the goal (a Shine), jumping when under it.
		if ((now % 300) == 0)
			fprintf(stderr, "[boss] f%u aim bot: no boss; goal %.0f %.0f %.0f seen %u frames ago, mario %.0f %.0f %.0f\n",
			        now, gPortGoalPos[0], gPortGoalPos[1], gPortGoalPos[2], now - gPortGoalFrame, m->mPosition.x,
			        m->mPosition.y, m->mPosition.z);
		if (now - gPortGoalFrame >= 10)
			return 0;
		const float gdx = gPortGoalPos[0] - m->mPosition.x, gdz = gPortGoalPos[2] - m->mPosition.z;
		*gx   = (int)gPortGoalPos[0];
		*gz   = (int)gPortGoalPos[2];
		*mode = 1;
		if (gdx * gdx + gdz * gdz < 150.0f * 150.0f && gPortGoalPos[1] > m->mPosition.y + 120.0f && (now % 40) < 8)
			*mode = 5;
		return 1;
	}
	double a  = (gPortBossRotY + deg) * 3.14159265358979 / 180.0;
	float px  = gPortBossPos[0] + d * (float)sin(a);
	float pz  = gPortBossPos[2] + d * (float)cos(a);
	// Spray-a-point mode (gPortBossWant 5, e.g. a sand sprout in Wiggler's way): stand at
	// gPortThrowAt, face gPortBossAux and spray it; the boss itself is ignored.
	// gPortBossWant 6: the same spot, but only wait there (the point needs no more water).
	const bool sprayPoint = now - gPortBossAuxFrame < 10 && (gPortBossWant == 5 || gPortBossWant == 6);
	float facePos[2]      = { gPortBossPos[0], gPortBossPos[2] };
	if (sprayPoint) {
		// Stand <d> from the point, on the side of gPortThrowAt.
		float sx = gPortThrowAt[0] - gPortBossAux[0], sz = gPortThrowAt[2] - gPortBossAux[2];
		const float sl = sqrtf(sx * sx + sz * sz) + 0.001f;
		px             = gPortBossAux[0] + sx / sl * (float)d;
		pz             = gPortBossAux[2] + sz / sl * (float)d;
		facePos[0] = gPortBossAux[0];
		facePos[1] = gPortBossAux[2];
	}
	const float dx  = px - m->mPosition.x, dz = pz - m->mPosition.z;
	static unsigned sSprayStart;
	static int sLastMode;
	static unsigned sJumpAt;
	if (now - gPortBossAuxFrame < 10 && gPortBossWant == 2) {
		// Stomp mode: walk onto the weak spot, jump, ground pound (L in the air).
		const float ax = gPortBossAux[0] - m->mPosition.x, az = gPortBossAux[2] - m->mPosition.z;
		static unsigned sPoundAt;
		*gx = (int)gPortBossAux[0];
		*gz = (int)gPortBossAux[2];
		if (sJumpAt != 0 && now - sJumpAt < 90) {
			// In the air: keep steering onto the spot, ground pound once over it.
			const unsigned t = now - sJumpAt;
			if (sPoundAt == 0 && t >= 6 && ((ax * ax + az * az < 60.0f * 60.0f && m->mVel.y < 5.0f) || t >= 40))
				sPoundAt = now;
			if (sPoundAt != 0)
				*mode = now - sPoundAt < 8 ? 6 : 0;
			else
				*mode = t < 8 ? 5 : 1; // 5 = A held (jump) while steering
			return 1;
		}
		sJumpAt   = 0;
		sPoundAt  = 0;
		sLastMode = 1;
		// Jump from up to 350 away (a lying boss body blocks walking closer) and steer on.
		if (ax * ax + az * az > 350.0f * 350.0f) {
			*mode = 1;
			return 1;
		}
		sJumpAt = now;
		*mode   = 5;
		return 1;
	}
	sJumpAt = 0;
	if (d <= 0 && !sprayPoint) {
		// AIM0 (fast bosses, e.g. Phantamanta): no stand point; close in to 1200 units and
		// spray from wherever Mario is, turning after it.
		const float ex = gPortBossPos[0] - m->mPosition.x, ez = gPortBossPos[2] - m->mPosition.z;
		const float ed = ex * ex + ez * ez;
		if (ed > 1200.0f * 1200.0f && !(sLastMode == 3 && ed < 1500.0f * 1500.0f)) {
			*mode     = 1;
			*gx       = (int)gPortBossPos[0];
			*gz       = (int)gPortBossPos[2];
			sLastMode = 1;
			return 1;
		}
		px = m->mPosition.x; // stand here
		pz = m->mPosition.z;
	}
	const float dx2 = px - m->mPosition.x, dz2 = pz - m->mPosition.z;
	if (dx2 * dx2 + dz2 * dz2 > 150.0f * 150.0f && !(sLastMode == 3 && dx2 * dx2 + dz2 * dz2 < 300.0f * 300.0f)) {
		*mode = 1;
		*gx   = (int)px;
		*gz   = (int)pz;
		sLastMode = 1;
		return 1;
	}
	const float bx  = facePos[0] - m->mPosition.x, bz = facePos[1] - m->mPosition.z;
	const s16 want  = (s16)(atan2f(bx, bz) * 32768.0f / 3.14159265f);
	const s16 diff  = (s16)(want - m->mFaceAngle.y);
	if ((diff > 0x0C00 || diff < -0x0C00) && sLastMode != 3) {
		*mode     = 2;
		*gx       = (int)facePos[0];
		*gz       = (int)facePos[1];
		sLastMode = 2;
		return 1;
	}
	if (sprayPoint && gPortBossWant == 6) {
		sLastMode = 2;
		*mode     = 0; // in place: wait
		return 1;
	}
	if (sLastMode != 3)
		sSprayStart = now;
	sLastMode = 3;
	*mode     = 3;
	if ((now - sSprayStart) % 20 >= 16)
		*mode = 4; // analog trigger released for a moment: pump (the nozzle's pressure
		           // is the trigger's rise, TNozzleBase::movement)
	if (m->mWaterGun != nullptr && m->mStatus == MARIO_STATUS_SQUAT) {
		const int cur = m->mWaterGun->getCurrentNozzle()->unk36E;
		if (cur < gun - 300)
			*stickY = 100;
		else if (cur > gun + 600)
			*stickY = -60;
	}
	if (diff > 0x1800 || diff < -0x1800)
		sLastMode = 2; // drifted off: turn again next frame
	return 1;
}

// SMS_PORT_PADSCRIPT "THROW" (debug, w5): carry-and-throw bot (King Boo's chilis and
// fruit). The boss code sets gPortBossWant = 4 with gPortBossAux = the item to fetch and
// gPortThrowAt = where to throw it. Without an item in hand: walk to it and press B; with
// one: stand 350-650 units from the target, face it, press B. Modes as port_boss_pull.
extern "C" float gPortItems[20][3];
extern "C" int gPortItemCount;
float gPortItems[20][3];
int gPortItemCount;
extern "C" int port_boss_throw(unsigned now, int* mode, int* gx, int* gz)
{
	*mode           = 0;
	const TMario* m = gpMarioOriginal;
	if (m == nullptr || now - gPortBossAuxFrame >= 10)
		return 0;
	static unsigned sPressAt;
	const bool canPress = now - sPressAt > 30;
	if (m->mHeldObject != nullptr) {
		const float tx = gPortThrowAt[0] - m->mPosition.x, tz = gPortThrowAt[2] - m->mPosition.z;
		const float d  = sqrtf(tx * tx + tz * tz);
		if (d > 650.0f || d < 350.0f) {
			const float k = d < 1.0f ? 0.0f : (d - 500.0f) / d;
			*gx   = (int)(m->mPosition.x + tx * k);
			*gz   = (int)(m->mPosition.z + tz * k);
			*mode = 1;
			return 1;
		}
		const s16 want = (s16)(atan2f(tx, tz) * 32768.0f / 3.14159265f);
		const s16 diff = (s16)(want - m->mFaceAngle.y);
		*gx            = (int)gPortThrowAt[0];
		*gz            = (int)gPortThrowAt[2];
		if (diff > 0x0800 || diff < -0x0800) {
			*mode = 2;
			return 1;
		}
		if (canPress)
			sPressAt = now;
		*mode = now - sPressAt < 4 ? 7 : 0;
		return 1;
	}
	// Pick the nearest item on Mario's level that has not been given up on (an item the
	// autopilot cannot reach, e.g. on the boss's platform, is skipped for 30 s).
	struct Skip {
		float x, z;
		unsigned until;
	};
	static Skip sSkip[8];
	static float sGoalX, sGoalZ, sBestDist;
	static unsigned sProgressAt;
	int best    = -1;
	float bestD = 1e30f;
	for (int i = 0; i < gPortItemCount; ++i) {
		const float* p = gPortItems[i];
		if (fabsf(p[1] - m->mPosition.y) > 200.0f)
			continue;
		bool skip = false;
		for (const Skip& s : sSkip)
			if (now < s.until && fabsf(s.x - p[0]) < 60.0f && fabsf(s.z - p[2]) < 60.0f)
				skip = true;
		const float dx = p[0] - m->mPosition.x, dz = p[2] - m->mPosition.z;
		if (!skip && dx * dx + dz * dz < bestD) {
			bestD = dx * dx + dz * dz;
			best  = i;
		}
	}
	if (best < 0)
		return 0;
	const float* p = gPortItems[best];
	const float d  = sqrtf(bestD);
	if (fabsf(p[0] - sGoalX) > 60.0f || fabsf(p[2] - sGoalZ) > 60.0f || d < sBestDist - 50.0f) {
		sGoalX      = p[0];
		sGoalZ      = p[2];
		sBestDist   = d;
		sProgressAt = now;
	} else if (now - sProgressAt > 120) {
		for (Skip& s : sSkip)
			if (now >= s.until) {
				s = { p[0], p[2], now + 900 };
				break;
			}
		sProgressAt = now;
		sBestDist   = 1e30f;
	}
	*gx = (int)p[0];
	*gz = (int)p[2];
	// Items often rest against something (the walk stops ~120 short): close in to 70,
	// or to 160 once the walk stalls, then face it and press B.
	if (d > 160.0f || (d > 70.0f && now - sProgressAt < 40)) {
		*mode = 1;
		return 1;
	}
	{
		const s16 want = (s16)(atan2f(p[0] - m->mPosition.x, p[2] - m->mPosition.z) * 32768.0f / 3.14159265f);
		const s16 diff = (s16)(want - m->mFaceAngle.y);
		if ((diff > 0x1000 || diff < -0x1000) && now - sPressAt >= 4) {
			*mode = 2;
			return 1;
		}
	}
	// B while running is a dive: stop first, then pick it up.
	if (m->mForwardVel > 1.0f && now - sPressAt >= 4) {
		*mode = 0;
		return 1;
	}
	if (canPress)
		sPressAt = now;
	*mode = now - sPressAt < 4 ? 7 : 0;
	return 1;
}

// SMS_PORT_PADSCRIPT "TURRET" (debug, w5): Mecha-Bowser coaster. The cart follows its
// rail by itself; the stick turns Mario on it (TMario::thinkTorocco). Turn toward the boss
// (gPortBossPos) and tap R (the FLUDD shot) while he is in front. port_main.cpp applies it.
#include <M3DUtil/MActor.hpp>
#include <JSystem/J3D/J3DGraphAnimator/J3DModel.hpp>
extern "C" int port_boss_turret(unsigned now, int* stickX, int* fire)
{
	*stickX         = 0;
	*fire           = 0;
	const TMario* m = gpMarioOriginal;
	if (m == nullptr || m->mTorocco == nullptr || now - gPortBossFrame >= 10)
		return 0;
	MtxPtr a      = m->mTorocco->getModel()->getAnmMtx(2);
	const float t = m->mToroccoAngle * (3.14159265f / 32768.0f);
	const float s = sinf(t), c = cosf(t);
	const float fx = a[0][0] * s + a[0][2] * c, fz = a[2][0] * s + a[2][2] * c;
	// A bottle rocket on the nozzle (rocket.cpp): turn to the boss and fire it with R.
	// None: turn to the nearest waiting rocket within 1500 and spray it to pick it up.
	const bool held = gPortRocketHeld && now - gPortRocketFrame < 10;
	float tx = gPortBossPos[0] - m->mPosition.x, tz = gPortBossPos[2] - m->mPosition.z;
	bool haveRocket = false;
	if (!held && now - gPortRocketFrame < 10) {
		float best = 1500.0f * 1500.0f;
		for (int i = 0; i < gPortItemCount; ++i) {
			const float rx = gPortItems[i][0] - m->mPosition.x, ry = gPortItems[i][1] - m->mPosition.y,
			            rz = gPortItems[i][2] - m->mPosition.z;
			if (rx * rx + ry * ry + rz * rz < best && rx * fx + rz * fz > 0.0f) {
				best       = rx * rx + ry * ry + rz * rz;
				tx         = rx;
				tz         = rz;
				haveRocket = true;
			}
		}
	}
	// Signed angle from Mario's facing to the target, degrees.
	const float diff = atan2f(fx * tz - fz * tx, fx * tx + fz * tz) * (180.0f / 3.14159265f);
	static const char* sSign = getenv("SMS_PORT_TURRET_SIGN");
	const int sign           = sSign != nullptr && sSign[0] == '-' ? -1 : 1;
	int sx                   = (int)(diff * 3.0f) * sign;
	*stickX                  = sx > 100 ? 100 : sx < -100 ? -100 : sx;
	if (held)
		*fire = (diff < 8.0f && diff > -8.0f && (now % 20) < 4) ? 1 : 0; // R press edge launches it
	else if (haveRocket)
		*fire = (diff < 25.0f && diff > -25.0f) ? 1 : 0; // spray
	if ((now % 60) == 0)
		fprintf(stderr, "[boss] f%u turret: %s, target %.0f %.0f, diff %.1f deg, stick %d, %d rockets waiting\n", now,
		        held ? "rocket on the nozzle -> boss" : haveRocket ? "spraying a rocket" : "no rocket near",
		        tx, tz, diff, *stickX, gPortItemCount);
	return 1;
}

// SMS_PORT_PADSCRIPT "ROCKET" (debug, w5): the final Bowser fight. Walk onto the nearest
// standing bathtub grip (MapObjCorona.cpp exports them), hold R until the rocket nozzle
// fires Mario up, and ground pound (L) at the top of the flight, so the long fall makes it
// a super hip drop. Modes: 1 walk to (gx, gz), 6 press L, 9 hold R, 0 nothing.
extern "C" unsigned gPortGripFrame;
unsigned gPortGripFrame;
extern "C" int port_boss_rocket(unsigned now, int* mode, int* gx, int* gz)
{
	*mode           = 0;
	const TMario* m = gpMarioOriginal;
	if (m == nullptr || now - gPortGripFrame >= 10 || gPortItemCount == 0)
		return 0; // (port_main then presses A now and then: ending demo, prompts)
	static unsigned sChargeAt, sLaunchAt, sPoundAt;
	// The tub tips toward a broken grip: go for the highest standing grip (the high side,
	// where Mario does not slide off), the nearest one when they are level.
	int best    = 0;
	float bestD = 1e30f;
	float topY  = -1e30f;
	for (int i = 0; i < gPortItemCount; ++i)
		if (gPortItems[i][1] > topY)
			topY = gPortItems[i][1];
	for (int i = 0; i < gPortItemCount; ++i) {
		if (gPortItems[i][1] < topY - 150.0f)
			continue;
		const float dx = gPortItems[i][0] - m->mPosition.x, dz = gPortItems[i][2] - m->mPosition.z;
		if (dx * dx + dz * dz < bestD) {
			bestD = dx * dx + dz * dz;
			best  = i;
		}
	}
	const float gxf = gPortItems[best][0], gzf = gPortItems[best][2];
	*gx = (int)gxf;
	*gz = (int)gzf;
	if (sLaunchAt != 0 && now - sLaunchAt < 240) {
		// In the air: drift out over the grip; ground pound once over it on the way down.
		if (sPoundAt == 0 && now - sLaunchAt > 10 && m->mVel.y < 5.0f
		    && (bestD < 30.0f * 30.0f || m->mVel.y < -25.0f))
			sPoundAt = now;
		if (sPoundAt != 0 && now - sPoundAt < 6)
			*mode = 6;
		else if (sPoundAt == 0)
			*mode = bestD > 15.0f * 15.0f ? 1 : 0;
		if (sPoundAt == now)
			fprintf(stderr, "[boss] f%u rocket bot: ground pound at %.0f %.0f %.0f, %.0f from grip %d\n", now,
			        m->mPosition.x, m->mPosition.y, m->mPosition.z, sqrtf(bestD), best);
		if (sPoundAt != 0 && now - sPoundAt > 90)
			sLaunchAt = 0;
		return 1;
	}
	sLaunchAt = 0;
	// A grip that took a super hip drop crumbles and sinks with Mario on it: jump straight
	// back in toward the rim (radius 3000 on his line) and keep going there for 2 s.
	static int sLastCount = -1;
	static unsigned sRetreatAt;
	static float sRetreatX, sRetreatZ;
	if (sLastCount >= 0 && gPortItemCount < sLastCount) {
		const float am = atan2f(m->mPosition.x, m->mPosition.z);
		sRetreatAt     = now;
		sRetreatX      = 3000.0f * sinf(am);
		sRetreatZ      = 3000.0f * cosf(am);
	}
	sLastCount = gPortItemCount;
	if (sRetreatAt != 0 && now - sRetreatAt < 45) {
		*gx   = (int)sRetreatX;
		*gz   = (int)sRetreatZ;
		*mode = (now - sRetreatAt) % 4 < 2 ? 5 : 1; // A pulses: jump the first frame Mario can
		return 1;
	}
	// Launch from the rim just inside the grip (150 units toward the tub's centre, the
	// origin) and drift onto it in the air.
	const float gl = sqrtf(gxf * gxf + gzf * gzf) + 0.001f;
	const float px = gxf - gxf / gl * 150.0f, pz = gzf - gzf / gl * 150.0f;
	*gx            = (int)px;
	*gz            = (int)pz;
	bestD          = (px - m->mPosition.x) * (px - m->mPosition.x) + (pz - m->mPosition.z) * (pz - m->mPosition.z);
	// The rim is a five-pointed shape with the grips at its points (radius ~5300; between
	// them it ends at ~3600, where Mario starts at 3311): go round at radius 3300, in steps
	// of ~17 degrees, then straight out along the point to the grip.
	{
		const float am = atan2f(m->mPosition.x, m->mPosition.z), ag = atan2f(px, pz);
		float da       = ag - am;
		while (da > 3.14159265f)
			da -= 6.2831853f;
		while (da < -3.14159265f)
			da += 6.2831853f;
		const float rm = sqrtf(m->mPosition.x * m->mPosition.x + m->mPosition.z * m->mPosition.z);
		if ((da > 0.3f || da < -0.3f) && rm > 3700.0f) {
			// Out on a point: back in along it first (its sides are narrow).
			*gx       = (int)(3300.0f * sinf(am));
			*gz       = (int)(3300.0f * cosf(am));
			sChargeAt = 0;
			*mode     = 1;
			return 1;
		}
		if (da > 0.3f || da < -0.3f) {
			const float a = am + (da > 0.0f ? 0.3f : -0.3f), r = 3300.0f;
			*gx           = (int)(r * sinf(a));
			*gz           = (int)(r * cosf(a));
			sChargeAt     = 0;
			*mode         = 1;
			return 1;
		}
	}
	if (bestD > 60.0f * 60.0f) {
		sChargeAt = 0;
		*mode     = 1;
		return 1;
	}
	if (m->mForwardVel > 1.0f && sChargeAt == 0) {
		*mode = 0; // stop on it first
		return 1;
	}
	if (sChargeAt == 0 || now - sChargeAt > 300)
		sChargeAt = now;
	*mode = 9;
	if (m->mVel.y > 30.0f) {
		sLaunchAt = now;
		sPoundAt  = 0;
		sChargeAt = 0;
		fprintf(stderr, "[boss] f%u rocket bot: launched at %.0f %.0f %.0f toward grip %d\n", now, m->mPosition.x,
		        m->mPosition.y, m->mPosition.z, best);
	}
	return 1;
}

// SMS_PORT_PADSCRIPT "EEL" (debug, w5): Eely-Mouth, under water. Mario is in the dive status
// there: he sinks when idle, the stick moves him level (the G autopilot works), and the
// hover nozzle (R) lifts him while its jets wash whatever is below. While the eel is down:
// hover over its hole at the depth that brings it up. While it is out: hover 400 above the
// nearest tooth that still has hit points. bosseel.cpp exports the teeth (gPortItems).
extern "C" unsigned gPortEelFrame;
extern "C" int gPortEelState;
unsigned gPortEelFrame;
int gPortEelState;
extern "C" int port_boss_eel(unsigned now, int* gotoOn, int* gx, int* gz, int* creep, int* holdB, int* fire)
{
	*gotoOn = *creep = *holdB = *fire = 0;
	const TMario* m = gpMarioOriginal;
	if (m == nullptr || now - gPortEelFrame >= 10)
		return 0;
	float tx = 0.0f, ty = -17000.0f, tz = 1500.0f, fx = 0.0f, fz = 0.0f;
	bool haveTooth = false;
	if (gPortEelState == 1 && gPortItemCount > 0) {
		int best    = 0;
		float bestD = 1e30f;
		for (int i = 0; i < gPortItemCount; ++i) {
			const float dx = gPortItems[i][0] - m->mPosition.x, dy = gPortItems[i][1] - m->mPosition.y,
			            dz = gPortItems[i][2] - m->mPosition.z;
			if (dx * dx + dy * dy + dz * dz < bestD) {
				bestD = dx * dx + dy * dy + dz * dz;
				best  = i;
			}
		}
		// Hover over the tooth: Mario dives here (MARIO_STATUS_DIVE, TMario::diving: the
		// stick moves him level, gravity sinks him) and the hover nozzle's jets both lift
		// him and wash what is below him.
		fx        = gPortItems[best][0];
		fz        = gPortItems[best][2];
		tx        = fx;
		tz        = fz;
		ty        = gPortItems[best][1] + 400.0f;
		haveTooth = true;
	} else if (gPortEelState == 2) {
		return 1; // being eaten: nothing to do
	} else if (gPortEelState == 3 || (gPortItemCount == 0 && now - gPortGoalFrame < 10)) {
		// Beaten: swim to the Shine (gPortGoalPos, Item.cpp), at its depth.
		if (now - gPortGoalFrame >= 10)
			return 1;
		tx = gPortGoalPos[0];
		ty = gPortGoalPos[1] - 50.0f;
		tz = gPortGoalPos[2];
	}
	(void)fx;
	(void)fz;
	// R (hover) while below the wanted depth, nothing (sink) while above.
	*fire = m->mPosition.y < ty ? 1 : 0;
	const float dx = tx - m->mPosition.x, dz = tz - m->mPosition.z;
	if (dx * dx + dz * dz > 80.0f * 80.0f) {
		*gotoOn = 1;
		*creep  = dx * dx + dz * dz < 400.0f * 400.0f ? 1 : 0;
		*gx     = (int)tx;
		*gz     = (int)tz;
	}
	if ((now % 120) == 0)
		fprintf(stderr, "[boss] f%u eel bot: state %d, target %.0f %.0f %.0f, mario %.0f %.0f %.0f, %s%s%s\n", now,
		        gPortEelState, tx, ty, tz, m->mPosition.x, m->mPosition.y, m->mPosition.z, *gotoOn ? "steer " : "",
		        *holdB ? "rise " : "", *fire ? "spray" : "");
	return 1;
}

// SMS_PORT_INFWATER=1 (debug test aid, w5, off by default): FLUDD's tank is kept full,
// for scripted fights in arenas where the bot cannot walk to water to refill. Like
// SMS_PORT_GODMODE it changes the game state, so it is only for test runs. Called
// once per PADRead from port_main.cpp while a PADSCRIPT is set.
extern "C" void port_boss_assist(void)
{
	static const bool sInf = getenv("SMS_PORT_INFWATER") != nullptr;
	static const bool sGod = getenv("SMS_PORT_GODMODE") != nullptr;
	TMario* m = gpMarioOriginal;
	if (sInf && m != nullptr && m->mWaterGun != nullptr)
		m->mWaterGun->mCurrentWater = m->mWaterGun->getMaxWater();
	// SMS_PORT_GODMODE also keeps the air meter full (under water it drains on its own,
	// TMario::checkAir -> loserExec, not through decHP).
	if (sGod && m != nullptr)
		m->mAir = m->mMaxAir;
}

// SMS_PORT_PADSCRIPT "PULL" (debug, w5): walk to the boss's grab point (gPortBossAux,
// e.g. Gooper Blooper's beak), press B to grab it, then hold the stick away from
// the boss until it lets go. port_main.cpp applies the result: mode 1 = walk to
// (gx, gz), 7 = press B (grab), 8 = walk away to (gx, gz) while holding.
extern "C" int port_boss_pull(unsigned now, int* mode, int* gx, int* gz)
{
	*mode           = 0;
	const TMario* m = gpMarioOriginal;
	if (m == nullptr || now - gPortBossFrame >= 10 || now - gPortBossAuxFrame >= 10)
		return 0;
	static unsigned sGrabAt;
	static unsigned sLetGoAt;
	if (now - sLetGoAt < 20)
		return 1; // let go: the release (THROWN) is what scores a beak pull past mSLBeakLengthDamage
	if (m->mHeldObject != nullptr || m->mStatus == MARIO_STATUS_PULLING) {
		float dx = m->mPosition.x - gPortBossPos[0], dz = m->mPosition.z - gPortBossPos[2];
		float l  = sqrtf(dx * dx + dz * dz);
		const float bdx = gPortBossAux[0] - gPortBossPos[0], bdz = gPortBossAux[2] - gPortBossPos[2];
		const float release = gPortBossRelease > 0.0f ? gPortBossRelease + 40.0f : 640.0f;
		if (gPortBossWant == 0 && gPortBossAuxIsBeak && sqrtf(bdx * bdx + bdz * bdz) > release) {
			sLetGoAt = now;
			return 1;
		}
		if (l < 1.0f) {
			dx = 0.0f;
			dz = 1.0f;
			l  = 1.0f;
		}
		// SMS_PORT_PULL_DIR=<deg> (world yaw, 0 = +z, 90 = +x): pull toward open floor
		// instead of straight away from the boss (an arena edge stops the pull short).
		static const char* sDir = getenv("SMS_PORT_PULL_DIR");
		if (sDir != nullptr && gPortBossAuxIsBeak) {
			const float r = (float)atof(sDir) * (3.14159265f / 180.0f);
			dx            = sinf(r);
			dz            = cosf(r);
			l             = 1.0f;
		}
		*mode = 8;
		*gx   = (int)(gPortBossPos[0] + dx / l * 4000.0f);
		*gz   = (int)(gPortBossPos[2] + dz / l * 4000.0f);
		return 1;
	}
	const float ax = gPortBossAux[0] - m->mPosition.x, az = gPortBossAux[2] - m->mPosition.z;
	*gx = (int)gPortBossAux[0];
	*gz = (int)gPortBossAux[2];
	if (ax * ax + az * az > 130.0f * 130.0f) {
		*mode = 1;
		return 1;
	}
	if (now - sGrabAt > 30)
		sGrabAt = now;
	*mode = (now - sGrabAt) < 6 ? 7 : 2; // B, then face it
	return 1;
}