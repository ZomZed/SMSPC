#include <System/Application.hpp>
#include <System/PositionHolder.hpp>
#include <stdio.h>
#include <stdlib.h>
#include <dolphin/types.h>
#include <dolphin/os.h>
#include <dolphin/vi.h>
#include <dolphin/gx.h>
#include <dolphin/card.h>
#include <THPPlayer/THPPlayer.h>
#include <JSystem/JKernel/JKRDvdRipper.hpp>
#include <JSystem/JKernel/JKRDvdAramRipper.hpp>
#include <JSystem/JKernel/JKRExpHeap.hpp>
#include <JSystem/JKernel/JKRAram.hpp>
#include <JSystem/JKernel/JKRAramStream.hpp>
#include <JSystem/JKernel/JKRMemArchive.hpp>
#include <JSystem/JUtility/JUTGamePad.hpp>
#include <JSystem/JUtility/JUTRomFont.hpp>
#include <JSystem/JUtility/JUTDirectPrint.hpp>
#include <JSystem/JUtility/JUTAssert.hpp>
#include <JSystem/ResTIMG.hpp>
#include <JSystem/JMath.hpp>
#include <JSystem/JDrama/JDRDisplay.hpp>
#include <JSystem/JDrama/JDRNameRefGen.hpp>
#include <JSystem/JDrama/JDRVideo.hpp>
#include <JSystem/JAudio/JAInterface/JAIGlobalParameter.hpp>
#include <JSystem/J2D/J2DPrint.hpp>
#include <MarioUtil/RumbleMgr.hpp>
#include <MarioUtil/DrawUtil.hpp>
#include <GC2D/ScrnFader.hpp>
#include <System/MarDirector.hpp>
#include <System/RenderModeObj.hpp>
#include <System/TexCache.hpp>
#include <System/DrawSyncManager.hpp>
#include <System/MarioGamePad.hpp>
#include <System/Resolution.hpp>
#include <System/FlagManager.hpp>
#include <System/TimeRec.hpp>
#include <System/ProcessMeter.hpp>
#include <System/CardManager.hpp>
#include <System/ScenarioArchiveName.hpp>
#include <System/MarNameRefGen.hpp>
#include <System/GCLogoDir.hpp>
#include <System/MovieDirector.hpp>
#include <System/SelectDir.hpp>
#include <System/MenuDir.hpp>
#include <MSound/MSound.hpp>

// rogue includes needed for matching sinit & bss
#include <MSound/MSSetSound.hpp>
#include <MSound/MSoundBGM.hpp>
#include <System/DummyStrings.hpp>

#ifdef SMS_PORT
#include "port_profile.h" // w14: SMS_PORT_PROFILE=1 frame/load profiler
#include <Player/Mario.hpp>
#include <Player/WaterGun.hpp>
#include <Camera/Camera.hpp>
#include <MarioUtil/MathUtil.hpp>
#include <MoveBG/MapObjCorona.hpp>
#ifdef SMS_PORT
#include <MoveBG/Item.hpp>        // SMS_PORT_DEBUG_CALL=hatch
#include <Player/Yoshi.hpp>       // SMS_PORT_DEBUG_CALL=hatch, SMS_PORT_TRACE=act
#include <Player/MarioAccess.hpp> // SMS_GetYoshi
#include <Enemy/EnemyMario.hpp>   // PADSCRIPT "G*"/"F*", SMS_PORT_TRACE (W7-J)
#include <Enemy/Emario.hpp>
#include <Enemy/Graph.hpp>
#include <Map/Map.hpp>            // SMS_PORT_DEBUG_CALL=hmap
#include <Map/MapData.hpp>
#include <NPC/NpcBase.hpp>        // SMS_PORT_DEBUG_CALL=npcs
#endif
#include <math.h>
#include <stdlib.h>
// port/src/sdk_os_misc.cpp: SMS_PORT_WARP debug warp target
extern "C" int port_warp_target(unsigned char* area, unsigned char* scenario);
extern "C" void* port_isle_stage_archive(int stage, int scenario); // port/src/port_isle.cpp
// port/src/port_main.cpp: frames presented (the SMS_PORT_PADSCRIPT/SHOT clock)
extern "C" u32 port_frame_count(void);
// port/src/port_crash.cpp: PADSCRIPT "G<x>;<z>" target (active, x, z) and its stick
extern "C" int gPortGoto[3];
extern "C" int gPortGotoStick[2];
extern "C" unsigned gPortStageFrame;
extern "C" int gPortStageCount;
extern "C" unsigned gPortDemoEndFrame; // PADSCRIPT "+d<frame>" origin (port_crash.cpp)
extern "C" int gPortDemoEndCount;
extern "C" volatile unsigned gPortCond; // PADSCRIPT conditions (port_crash.cpp)
static bool sPortStageArmed = true;
// The stage's Shadow Mario (emario.cpp TEMario::load), for PADSCRIPT "G*"/"F*" and the
// trace (W7-J). Cleared on every app state change, before the next stage loads.
TEnemyMario* gPortEnemyMario;
static const int kPortTargetShadow = 0x7FFFFFFF; // gPortGoto x: follow Shadow Mario
// PADSCRIPT "W<x>;<z>|<x>;<z>|..." (W7-J): a route walked point by point (gPortGoto[0] == 3),
// set by port_main.cpp while the entry is active; the next point is taken within 150 units.
extern "C" int gPortRoute[16][2];
extern "C" int gPortRouteN;
static int sRouteIdx, sRouteKey[3] = { -1 };
int gPortRoute[16][2];
int gPortRouteN;

// SMS_PORT_TRACE=1 (debug, off by default): one line per change of the gameplay
// state that matters for driving the game blind (--nodraw): director state, talk/demo
// mode, director flags, Mario's action and health, lives, shines and the next area,
// plus Mario's position every 2 s. Read-only.
static void portTraceGameplay(TApplication* app)
{
	static const bool sOn = getenv("SMS_PORT_TRACE") != nullptr;
	if (!sOn)
		return;
	static unsigned sLastCond;
	if (gPortCond != sLastCond) {
		OSReport("[trace] f%u cond %04x (boss hp %u%s%s)\n", port_frame_count(), gPortCond,
		         (gPortCond >> 8) & 0xFF, (gPortCond & 1) ? ", head vulnerable" : "",
		         (gPortCond & 2) ? ", asleep" : "");
		sLastCond = gPortCond;
	}
	static u32 sLastBeat;
	if (port_frame_count() / 600 != sLastBeat) {
		sLastBeat = port_frame_count() / 600;
		OSReport("[trace] f%u heartbeat: app state %d, director %p, mar director %p, "
		         "goto %d (%d,%d) stick %d,%d camera %p pad flags %08x\n",
		         port_frame_count(), app->mAppState, app->mDirector, gpMarDirector,
		         gPortGoto[0], gPortGoto[1], gPortGoto[2], gPortGotoStick[0],
		         gPortGotoStick[1], gpCamera,
		         (gpMarioOriginal && gpMarioOriginal->mGamePad) ? (u32)gpMarioOriginal->mGamePad->mFlags : 0u);
	}
	if (gpMarDirector == nullptr || app->mDirector != (JDrama::TDirector*)gpMarDirector)
		return;
	static u32 sKey[9];
	TMarDirector* d = gpMarDirector;
	TMario* m       = gpMarioOriginal;
	TFlagManager* f = TFlagManager::getInstance();
	u32 key[9]      = { d->mState,
		                d->unk124,
		                d->mFlags & ~(TMarDirector::DIRECTOR_FLAG_FIRST_SIMULATION_TICK
		                              | TMarDirector::DIRECTOR_FLAG_LAST_SIMULATION_TICK),
		                m != nullptr,
		                m ? (u32)m->mHealth : 0,
		                (u32)f->getFlag(MSF_LIFE_COUNT),
		                (u32)f->getFlag(MSF_SHINE_COUNT),
		                (u32)(app->mNextArea.getStage() << 8 | app->mNextArea.getScenario()),
		                d->mDemoQueueHead != d->mDemoQueueTail };
	const u32 frame = port_frame_count();
	if (memcmp(key, sKey, sizeof(key)) != 0) {
		OSReport("[trace] f%u dir state %u mode %u flags %04x | mario act %08x hp %u | "
		         "lives %u shines %u next %u/%u demoq %u\n",
		         frame, key[0], key[1], key[2], m ? m->mStatus : 0, key[4], key[5],
		         key[6], key[7] >> 8, key[7] & 0xFF, key[8]);
		memcpy(sKey, key, sizeof(key));
	}
	// SMS_PORT_TRACE=act (w12 mechanics checklist): also one line per change of
	// Mario's action, nozzle or Yoshi state (with position, speeds, HP and water),
	// and per change of the gold/red/blue coin counters.
	static const bool sAct = getenv("SMS_PORT_TRACE")[0] == 'a';
	static u32 sLastAct;
	static int sLastNoz = -1, sLastYoshi = -1;
	const int noz = (m != nullptr && m->mWaterGun) ? (int)m->mWaterGun->mCurrentNozzle : -1;
	const int ys  = (m != nullptr && m->mYoshi) ? (int)m->mYoshi->mState : -1;
	if (sAct && m != nullptr && (m->mStatus != sLastAct || noz != sLastNoz || ys != sLastYoshi)) {
		OSReport("[act] f%u %08x -> %08x pos %.0f %.0f %.0f fwd %.1f vy %.1f hp %u water %d "
		         "noz %d yoshi %d\n",
		         frame, sLastAct, m->mStatus, m->mPosition.x, m->mPosition.y,
		         m->mPosition.z, m->mForwardVel, m->mVel.y, (u32)m->mHealth,
		         m->mWaterGun ? (int)m->mWaterGun->mCurrentWater : -1, noz, ys);
		sLastAct   = m->mStatus;
		sLastNoz   = noz;
		sLastYoshi = ys;
	}
	static int sCoins[3] = { -1, -1, -1 };
	const int coins[3]   = { f->getFlag(MSF_GOLD_COIN_COUNT), f->getFlag(MSF_RED_COIN_COUNT),
		                     f->getFlag(MSF_BLUE_COIN_COUNT) };
	if (sAct && memcmp(coins, sCoins, sizeof(coins)) != 0) {
		OSReport("[act] f%u coins %d red %d blue %d\n", frame, coins[0], coins[1], coins[2]);
		memcpy(sCoins, coins, sizeof(coins));
	}
	// The game loop can run at 30 Hz (2 frames per call), so test the 120-frame bucket.
	// (w12) SMS_PORT_TRACE containing "pos": every game frame instead (route calibration).
	static const bool sEveryFrame = strstr(getenv("SMS_PORT_TRACE"), "pos") != nullptr;
	static u32 sLastPos;
	const u32 bucket = sEveryFrame ? frame : frame / 120;
	if (m != nullptr && bucket != sLastPos && (sLastPos = bucket, true))
		OSReport("[trace] f%u mario pos %.0f %.0f %.0f act %08x water %d stick %.2f,%.2f "
		         "mag %.1f vel %.1f face %d gun %d\n",
		         frame, m->mPosition.x, m->mPosition.y, m->mPosition.z, m->mStatus,
		         m->mWaterGun ? (int)m->mWaterGun->mCurrentWater : -1,
		         m->mGamePad ? m->mGamePad->mCompSPos[0] : 0.0f,
		         m->mGamePad ? m->mGamePad->mCompSPos[1] : 0.0f, m->mIntendedMag,
		         m->mForwardVel, (int)m->mFaceAngle.y,
		         m->mWaterGun ? (int)m->mWaterGun->getCurrentNozzle()->unk36E : 0);
	// Shadow Mario (W7-J): his behaviour state and the water hits he has left.
	if (TEnemyMario* em = gPortEnemyMario) {
		static TEnemyMario* sDumped;
		if (sDumped != em && em->mEMario != nullptr && em->mEMario->getTracer() != nullptr
		    && em->mEMario->getTracer()->getGraph() != nullptr) {
			sDumped                = em;
			const TGraphWeb* g     = em->mEMario->getTracer()->getGraph();
			for (int i = 0; i < g->getNodeNum(); i++) {
				const TRailNode* rn = g->getGraphNode(i).getRailNode();
				OSReport("[trace] shadow graph node %d: %d %d %d links %d %d %d %d\n", i,
				         rn->mPosition.x, rn->mPosition.y, rn->mPosition.z,
				         rn->mConnectionNum > 0 ? rn->mConnections[0] : -1,
				         rn->mConnectionNum > 1 ? rn->mConnections[1] : -1,
				         rn->mConnectionNum > 2 ? rn->mConnections[2] : -1,
				         rn->mConnectionNum > 3 ? rn->mConnections[3] : -1);
			}
		}
		static u32 sEmKey = 0xFFFFFFFF;
		const u32 key = (u32)em->mEMDoing << 16 | (u16)em->mWaterCounter;
		if (key != sEmKey || frame % 240 == 0) {
			OSReport("[trace] f%u shadow mario doing %u water %d pos %.0f %.0f %.0f act %08x\n",
			         frame, em->mEMDoing, em->mWaterCounter, em->mPosition.x, em->mPosition.y,
			         em->mPosition.z, em->mStatus);
			sEmKey = key;
		}
	}
	// SMS_PORT_TRACE=aim (W7-J): FLUDD's pitch every 10 frames while spraying planted.
	static const bool sAim = getenv("SMS_PORT_TRACE")[1] == 'i';
	if (sAim && m != nullptr && m->mWaterGun != nullptr && m->mStatus == 0x0C008220
	    && frame % 10 == 0)
		OSReport("[aim] f%u gun %d face %d stick %.2f\n", frame,
		         (int)m->mWaterGun->getCurrentNozzle()->unk36E, (int)m->mFaceAngle.y,
		         m->mGamePad ? m->mGamePad->mCompSPos[1] : 0.0f);
}

// PADSCRIPT "G*" (W7-J, debug): a small navigation graph for the autopilot, either
// SMS_PORT_NAV="x,y,z x,y,z ...;a-b a-b ..." (walkable links, both ways) or, without it,
// Shadow Mario's own graph (leaving out links that climb more than a ramp does).
struct TPortNav {
	int n;
	f32 p[64][3];
	short deg[64];
	short adj[64][8];
};

static void portNavLink(TPortNav& g, int a, int b, int jump = 0)
{
	if (a < 0 || b < 0 || a >= g.n || b >= g.n || g.deg[a] >= 8)
		return;
	g.adj[a][g.deg[a]++] = (short)(b | (jump ? 0x4000 : 0));
}

static bool portNavFromEnv(TPortNav& g)
{
	static int sParsed = -1;
	static TPortNav sNav;
	if (sParsed < 0) {
		sParsed     = 0;
		const char* s = getenv("SMS_PORT_NAV");
		if (s != nullptr) {
			sNav.n = 0;
			while (*s != '\0' && *s != ';' && sNav.n < 64) {
				f32 x, y, z;
				int used = 0;
				if (sscanf(s, " %f,%f,%f%n", &x, &y, &z, &used) != 3)
					break;
				sNav.p[sNav.n][0] = x;
				sNav.p[sNav.n][1] = y;
				sNav.p[sNav.n][2] = z;
				sNav.deg[sNav.n]  = 0;
				sNav.n++;
				s += used;
				while (*s == ' ')
					++s;
			}
			if (*s == ';')
				++s;
			// "a-b" both ways, "a>b" one way (a drop Mario cannot climb back up), "a^b" both
			// ways with a jump (a low step)
			int a, b, used;
			char op;
			while (sscanf(s, " %d%c%d%n", &a, &op, &b, &used) == 3 && (op == '-' || op == '>' || op == '^')) {
				portNavLink(sNav, a, b, op == '^');
				if (op != '>')
					portNavLink(sNav, b, a, op == '^');
				s += used;
			}
			sParsed = sNav.n > 1;
			OSReport("[port] SMS_PORT_NAV: %d nodes\n", sNav.n);
		}
	}
	if (sParsed > 0)
		g = sNav;
	return sParsed > 0;
}

static bool portNavFromGraph(TPortNav& g, const TGraphWeb* w)
{
	const int n = w != nullptr ? w->getNodeNum() : 0;
	if (n <= 1 || n > 64)
		return false;
	g.n = n;
	for (int i = 0; i < n; i++) {
		const JGeometry::TVec3<f32> p = w->getGraphNode(i).getPoint();
		g.p[i][0] = p.x, g.p[i][1] = p.y, g.p[i][2] = p.z;
		g.deg[i]  = 0;
	}
	for (int i = 0; i < n; i++) {
		const TRailNode* rn = w->getGraphNode(i).getRailNode();
		for (int k = 0; k < rn->mConnectionNum && k < 8; k++) {
			const int d = rn->mConnections[k];
			if (d >= 0 && d < n && g.p[d][1] - g.p[i][1] <= 350.0f)
				portNavLink(g, i, d); // a jump up (a ramp climbs less) the autopilot cannot make
		}
	}
	return true;
}

// The next point on the way from `from` to (tx, ty, tz), breadth-first over the graph.
static bool portNavStep(const TPortNav& g, const JGeometry::TVec3<f32>& from, f32 tx, f32 ty,
                        f32 tz, f32* ox, f32* oz)
{
	static short sPrev[64], sQueue[64];
	const int n = g.n;
	int a = -1, b = -1;
	f32 da = 1e30f, db = 1e30f;
	for (int i = 0; i < n; i++) {
		f32 x = g.p[i][0] - from.x, y = (g.p[i][1] - from.y) * 3.0f, z = g.p[i][2] - from.z;
		if (x * x + y * y + z * z < da)
			da = x * x + y * y + z * z, a = i;
		x = g.p[i][0] - tx, y = (g.p[i][1] - ty) * 3.0f, z = g.p[i][2] - tz;
		if (x * x + y * y + z * z < db)
			db = x * x + y * y + z * z, b = i;
	}
	if (a < 0 || b < 0 || a == b)
		return false;
	for (int i = 0; i < n; i++)
		sPrev[i] = -1;
	int head = 0, tail = 0;
	sQueue[tail++] = (short)a;
	sPrev[a]       = (short)a;
	while (head < tail && sPrev[b] < 0) {
		const int c = sQueue[head++];
		for (int k = 0; k < g.deg[c]; k++) {
			const int d = g.adj[c][k] & 0x3FFF;
			if (sPrev[d] >= 0)
				continue;
			sPrev[d]       = (short)c;
			sQueue[tail++] = (short)d;
		}
	}
	if (sPrev[b] < 0)
		return false;
	int step = b;
	while (sPrev[step] != a)
		step = sPrev[step];
	// Go to the start node first unless Mario is already past it toward the next one.
	const f32 ax = g.p[a][0] - from.x, az = g.p[a][2] - from.z;
	const f32 mx = g.p[step][0] - from.x, mz = g.p[step][2] - from.z;
	const f32 nx = g.p[step][0] - g.p[a][0], nz = g.p[step][2] - g.p[a][2];
	const int go = (ax * ax + az * az > 250.0f * 250.0f && mx * mx + mz * mz > nx * nx + nz * nz)
	                   ? a
	                   : step;
	*ox = g.p[go][0];
	*oz = g.p[go][2];
	// "?25": heading down a one-way link ("a>b", a drop over a railing): jump now.
	bool oneWay = go == step;
	for (int k = 0; k < g.deg[step] && oneWay; k++)
		if ((g.adj[step][k] & 0x3FFF) == a)
			oneWay = false;
	for (int k = 0; k < g.deg[a]; k++)
		if (g.adj[a][k] == (step | 0x4000) && go == step)
			oneWay = true; // "a^b": a jump either way
	if (oneWay && ax * ax + az * az < 400.0f * 400.0f)
		gPortCond |= 1u << 25;
	else
		gPortCond &= ~(1u << 25);
	static const bool sTrace = getenv("SMS_PORT_TRACE") != nullptr;
	static u32 sLast;
	if (sTrace && port_frame_count() / 60 != sLast) {
		sLast = port_frame_count() / 60;
		OSReport("[trace] f%u nav %d nodes: at %d, goal %d, next %d (%.0f %.0f %.0f)\n",
		         port_frame_count(), n, a, b, go, g.p[go][0], g.p[go][1], g.p[go][2]);
	}
	return true;
}

// SMS_PORT_PADSCRIPT "G<x>;<z>" (debug): port_main.cpp sets gPortGoto (active, x, z)
// while the entry is active and applies gPortGotoStick to pad 0. Here the stick that
// walks Mario toward (x, z) is worked out from his position and the camera angle,
// the inverse of TMario's mIntendedYaw = matan(-V, H) + gpCamera->getUnk258().
static void portAutopilot()
{
	// PADSCRIPT "+<frame>" clock: restarts when a stage first hands Mario control (armed
	// by TApplication::proc on every app state change). It keeps running through movies
	// in between, so "+<n>@" entries can answer the prompts that follow them.
	if (sPortStageArmed && gpMarDirector->mState == 4) {
		sPortStageArmed = false;
		gPortStageFrame = port_frame_count();
		if (gPortStageFrame == 0)
			gPortStageFrame = 1;
		++gPortStageCount;
		OSReport("[port] PADSCRIPT: stage %d control at frame %u (+%d@0)\n",
		         gPortStageCount - 1, gPortStageFrame, gPortStageCount - 1);
	}
	// PADSCRIPT "+d<frame>" clock: the frame the last demo/talk ended (director
	// mode back to 0), e.g. a boss's appear demo.
	static u8 sLastMode;
	if (sLastMode != 0 && gpMarDirector->unk124 == 0) {
		gPortDemoEndFrame = port_frame_count();
		gPortDemoEndCount++;
		OSReport("[port] PADSCRIPT: demo/talk %d ended at frame %u (+d0)\n", gPortDemoEndCount,
		         gPortDemoEndFrame);
	}
	sLastMode = gpMarDirector->unk124;
	// PADSCRIPT "?16" / "!16": a talk or demo is running (director mode not 0), e.g. to
	// press A only while a dialogue waits for it (W7-J).
	gPortCond = (gPortCond & ~0x10000u) | (gpMarDirector->unk124 != 0 ? 0x10000u : 0u);
	// "?17": Mario is wading in goop (the OIL_* actions), e.g. to walk back out of it.
	{
		const u32 st     = gpMarioOriginal != nullptr ? gpMarioOriginal->mStatus : 0;
		const bool inOil = st == MARIO_STATUS_OIL_RUN || st == MARIO_STATUS_OIL_SLIP
		                   || st == MARIO_STATUS_OIL_SLOPE;
		gPortCond = (gPortCond & ~0x20000u) | (inOil ? 0x20000u : 0u);
	}
	gPortCond &= ~(1u << 25); // set again below by portNavStep for this frame
	// "?23" (W7-J): FLUDD needs a refill: set below 15% water, cleared again at 95%.
	if (gpMarioOriginal != nullptr && gpMarioOriginal->mWaterGun != nullptr) {
		TWaterGun* wg = gpMarioOriginal->mWaterGun;
		const s32 w         = wg->mCurrentWater;
		const s32 full      = wg->getCurrentNozzle()->mEmitParams.mAmountMax.get();
		if (w * 100 < full * 15)
			gPortCond |= 0x800000u;
		else if (w * 100 >= full * 95)
			gPortCond &= ~0x800000u;
	}
	// Shadow Mario (W7-J): "?18" waiting for Mario in his replay, "?19" within 1000
	// units of Mario (x/z), "?20" knocked down, "?21" water hits count now.
	{
		u32 bits = 0;
		if (TEnemyMario* em = gPortEnemyMario) {
			const u16 d = em->mEMDoing;
			if (d == TEnemyMario::EM_DOING_REPLAY_WAITING)
				bits |= 1u << 18;
			if (gpMarioOriginal != nullptr) {
				const f32 ex = em->mPosition.x - gpMarioOriginal->mPosition.x;
				const f32 ez = em->mPosition.z - gpMarioOriginal->mPosition.z;
				if (ex * ex + ez * ez < 1000.0f * 1000.0f)
					bits |= 1u << 19;
				if (em->mPosition.y > gpMarioOriginal->mPosition.y + 250.0f)
					bits |= 1u << 22; // "?22": he is up on a higher level
				if (em->mPosition.y < gpMarioOriginal->mPosition.y - 250.0f)
					bits |= 1u << 24; // "?24": he is down on a lower level
			}
			if (d == TEnemyMario::EM_DOING_PRE_DOWN_ANIMATION
			    || d == TEnemyMario::EM_DOING_DOWN_ANIMATION)
				bits |= 1u << 20;
			if (d == TEnemyMario::EM_DOING_REPLAY || d == TEnemyMario::EM_DOING_REPLAY_WAITING
			    || d == TEnemyMario::EM_DOING_REPLAY_JUMP_TO_NEAREST_NODE)
				bits |= 1u << 21;
		}
		gPortCond = (gPortCond & ~((0x1Fu << 18) | (1u << 24))) | bits;
	}

	TMario* m = gpMarioOriginal;
	if (!gPortGoto[0] || m == nullptr || gpCamera == nullptr) {
		gPortGotoStick[0] = gPortGotoStick[1] = 0;
		sRouteKey[0]      = -1;
		return;
	}
	f32 tx = gPortGoto[1], tz = gPortGoto[2];
	if (gPortGoto[0] == 3) { // "W" route
		const int key[3] = { gPortRouteN, gPortRoute[0][0], gPortRoute[0][1] };
		if (memcmp(key, sRouteKey, sizeof(key)) != 0) {
			memcpy(sRouteKey, key, sizeof(key));
			sRouteIdx = 0;
		}
		for (;;) {
			tx = gPortRoute[sRouteIdx][0];
			tz = gPortRoute[sRouteIdx][1];
			const f32 rx = tx - m->mPosition.x, rz = tz - m->mPosition.z;
			if (sRouteIdx + 1 >= gPortRouteN || rx * rx + rz * rz > 150.0f * 150.0f)
				break;
			++sRouteIdx;
		}
	} else {
		sRouteKey[0] = -1; // a route starts from its first point each time it becomes active
	}
	if (gPortGoto[1] == kPortTargetShadow && gPortGoto[0] != 3) { // "G*" / "F*": Shadow Mario
		if (gPortEnemyMario == nullptr) {
			gPortGotoStick[0] = gPortGotoStick[1] = 0;
			return;
		}
		tx = gPortEnemyMario->mPosition.x;
		tz = gPortEnemyMario->mPosition.z;
		// Walking ("G*"), not near him on the same level: follow his graph there.
		const f32 sx = tx - m->mPosition.x, sz = tz - m->mPosition.z;
		const f32 sy = gPortEnemyMario->mPosition.y - m->mPosition.y;
		TEMario* owner = gPortEnemyMario->mEMario;
		if (gPortGoto[0] == 1 && owner != nullptr && owner->getTracer() != nullptr
		    && (sx * sx + sz * sz > 800.0f * 800.0f || sy > 200.0f || sy < -200.0f))
		{
			static TPortNav sNav;
			if (portNavFromEnv(sNav) || portNavFromGraph(sNav, owner->getTracer()->getGraph()))
				portNavStep(sNav, m->mPosition, tx, gPortEnemyMario->mPosition.y, tz, &tx, &tz);
		}
	}
	if (gPortGoto[0] == 4) { // "GN<x>;<z>": along SMS_PORT_NAV
		static TPortNav sNavG;
		if (portNavFromEnv(sNavG))
			portNavStep(sNavG, m->mPosition, tx, m->mPosition.y, tz, &tx, &tz);
	}
	f32 dx = tx - m->mPosition.x;
	f32 dz = tz - m->mPosition.z;
	if (gPortGoto[0] == 2) {
		// PADSCRIPT "F<x>;<z>" (W7-J): turn Mario toward (x, z) in the FLUDD spray stance,
		// where stick X turns him (TMario::squating: right turns his face angle down)
		// and stick Y stays free for the aim. Nothing outside the stance.
		gPortGotoStick[1] = 0;
		if (m->mStatus != MARIO_STATUS_SQUAT) {
			gPortGotoStick[0] = 0;
			return;
		}
		s16 err = (s16)((s16)(atan2f(dx, dz) * (32768.0f / 3.14159265f)) - m->mFaceAngle.y);
		int sx  = -err / 40;
		if (err > -150 && err < 150)
			sx = 0;
		else if (sx > -40 && sx < 40)
			sx = sx < 0 ? -40 : 40; // past the stick's dead zone
		gPortGotoStick[0] = sx < -100 ? -100 : sx > 100 ? 100 : sx;
		return;
	}
	if (dx * dx + dz * dz < 60.0f * 60.0f) {
		gPortGotoStick[0] = gPortGotoStick[1] = 0;
		return;
	}
	s16 rel = (s16)(matan(dz, dx) - gpCamera->getUnk258());
	f32 rad = rel * (3.14159265f / 32768.0f);
	gPortGotoStick[0] = (int)(100.0f * sinf(rad));
	gPortGotoStick[1] = (int)(-100.0f * cosf(rad));
}

// SMS_PORT_WARP_FLAGS (debug, level sweep C): save-file flags for the warp's fresh
// save, so a stage can be started in a later story state. Comma-separated items:
//   <hex flag>          setBool(true) / setFlag(1)
//   <hex flag>=<dec>    setFlag(value)
//   S<hex>[-<hex>]      setShineFlag for one shine ID or a range (updates the count)
//   story<N>            preset for Delfino Plaza scenario N (1, 5, 6, 7, 8*, 9, 2),
//                       cumulative, the flags decideNextScenario() reads plus the
//                       shines/visits a player would have at that point.
static const char* const sPortStoryPresets[][2] = {
	{ "story1", "S56,S0,10384,103A5,103A6,103A7,10392,10393,10394,1039A,103A3,103A4,10397" },
	{ "story5", "story1,S1,S2,SA,S14,10385,103A8,103A9" },
	{ "story6", "story5,S1E,10386,10387,103AA" },
	{ "story7", "story6,S3,SB,S15,S28,S3C,103AB,103AC" },
	// Plaza scenario 8 sets MSF_SHADOW_MARIO_EVENT from these (setupObjects):
	// story8 no event, story8y 1 (Yoshi), story8t 2 (turbo nozzle), story8r 3 (rocket).
	{ "story8", "story7,S1F,S32,10389,103AD,10391" },
	{ "story8y", "story8,S21" },
	{ "story8t", "story8,S21,1038F,1038B,1038C,S4-7,SC-11,S16-1B" },
	{ "story8r", "story8t,10367" },
	{ "story9", "story8r,10366,S0-45,S56-76" },
	{ "story2", "story9,S77,103AE,10377" },
};

static void portWarpFlagItems(const char* p, int depth)
{
	TFlagManager* f = TFlagManager::getInstance();
	while (p != nullptr && *p != '\0' && depth < 8) {
		char item[32];
		size_t n = 0;
		while (*p != '\0' && *p != ',' && n < sizeof(item) - 1)
			item[n++] = *p++;
		item[n] = '\0';
		if (*p == ',')
			++p;
		bool preset = false;
		for (u32 i = 0; i < sizeof(sPortStoryPresets) / sizeof(sPortStoryPresets[0]); ++i)
			if (strcmp(item, sPortStoryPresets[i][0]) == 0) {
				portWarpFlagItems(sPortStoryPresets[i][1], depth + 1);
				preset = true;
			}
		if (preset || item[0] == '\0')
			continue;
		if (item[0] == 'S') {
			char* e;
			u32 a = strtoul(item + 1, &e, 16), b = a;
			if (*e == '-')
				b = strtoul(e + 1, nullptr, 16);
			for (u32 s = a; s <= b && s < 120; ++s)
				f->setShineFlag((u8)s);
			continue;
		}
		char* e;
		u32 flag = strtoul(item, &e, 16);
		if (*e == '=')
			f->setFlag(flag, (s32)strtol(e + 1, nullptr, 10));
		else if ((flag >> 16) == 1 || (flag >> 16) == 3 || (flag >> 16) == 5)
			f->setBool(true, flag);
		else
			f->setFlag(flag, 1);
	}
}

// w10 multiplayer (port_mp_game.cpp): the same items/presets for the free-mode save.
extern "C" void port_apply_flag_items(const char* items) { portWarpFlagItems(items, 0); }

#ifdef SMS_PORT_AURORA
// port/src/port_mp_game.cpp (w10): multiplayer session glue, inert outside multiplayer.
extern "C" void port_mp_note_area(int appState, int stage, int scenario);
extern "C" void port_mp_game_frame(void);
extern "C" int port_mp_debug_autostart(unsigned char* area, unsigned char* scenario);
extern "C" int port_mp_redirect_title(int appState, int reset, int* nextState, unsigned char* stage,
                                      unsigned char* scenario);
extern "C" int port_qol_skip_intro(void); // port/src/port_qol.cpp (w9)
#endif

static void portWarpFlags()
{
	const char* s = getenv("SMS_PORT_WARP_FLAGS");
	if (s == nullptr)
		return;
	portWarpFlagItems(s, 0);
	OSReport("[port] SMS_PORT_WARP_FLAGS: %s (shines %d)\n", s,
	         TFlagManager::getInstance()->getFlag(MSF_SHINE_COUNT));
}

// SMS_PORT_DEBUG_CALL=<what>@<frame> (debug, level sweep C): once per stage, at that
// SMS_PORT_SHOT/PADSCRIPT frame, calls a game function that scripted input cannot
// reach. "bathdemo": TBathtub::startDemo() in Corona's Bowser fight (what the last
// super hip drop on a grip does), to drive the ending demo and movies. "tpshine":
// moves Mario onto the object named "シャイン". (w12) "tp<x>;<y>;<z>": moves Mario
// there. "hatch": hatches the stage's Yoshi egg next to Mario.
static void portDebugCallOne(const char* sCall);
// (w12) Several calls can be given, separated by '|': each runs once per stage when
// its frame is reached ("tp...@900|hatch@1000").
static void portDebugCall()
{
	static const char* sAll = getenv("SMS_PORT_DEBUG_CALL");
	static void* sStage;
	static bool sDone[512];
	if (sAll == nullptr)
		return;
	if (sStage != gpMarDirector) {
		sStage = gpMarDirector;
		memset(sDone, 0, sizeof(sDone));
	}
	int idx = 0;
	for (const char* p = sAll; *p != '\0' && idx < 512; idx++) {
		const char* end = strchr(p, '|');
		size_t len      = end != nullptr ? (size_t)(end - p) : strlen(p);
		char buf[128];
		if (len >= sizeof(buf))
			len = sizeof(buf) - 1;
		memcpy(buf, p, len);
		buf[len]       = '\0';
		const char* at = strchr(buf, '@');
		if (!sDone[idx] && at != nullptr && port_frame_count() >= (u32)atoi(at + 1)) {
			sDone[idx] = true;
			portDebugCallOne(buf);
		}
		if (end == nullptr)
			break;
		p = end + 1;
	}
}

static void portDebugCallOne(const char* sCall)
{
	if (strncmp(sCall, "bathdemo", 8) == 0) {
		TBathtub* tub = (TBathtub*)JDrama::TNameRefGen::search("バスタブ");
		OSReport("[port] SMS_PORT_DEBUG_CALL: bathdemo, bathtub %p\n", tub);
		if (tub != nullptr)
			tub->startDemo();
	} else if (strncmp(sCall, "tpshine", 7) == 0 && gpMarioOriginal != nullptr) {
		// Put Mario on the stage's shine (the end of a secret course), so the
		// normal touch -> shine-get demo -> next stage path runs.
		JDrama::TActor* shine = (JDrama::TActor*)JDrama::TNameRefGen::search("シャイン");
		OSReport("[port] SMS_PORT_DEBUG_CALL: tpshine, shine %p\n", shine);
		if (shine != nullptr)
			gpMarioOriginal->mPosition = shine->mPosition;
	} else if (strncmp(sCall, "flag", 4) == 0) {
		// (w5 boss fights) "flag<hex>@<frame>": set a save/stage flag the way a stage
		// script does (e.g. 50007 = MSF_MANTA_SPAWNED, what sirena0's appearmanta.sb sets
		// to start the Phantamanta battle). A stage-local 0x5xxxx flag set by
		// SMS_PORT_WARP_FLAGS before the stage loads does not survive the stage setup.
		const u32 flag = (u32)strtoul(sCall + 4, nullptr, 16);
		TFlagManager::getInstance()->setBool(true, flag);
		OSReport("[port] SMS_PORT_DEBUG_CALL: flag %x set\n", flag);
	} else if (strncmp(sCall, "stage", 5) == 0 && gpMarDirector != nullptr) {
		// "stage<hex>@<frame>": take a level entrance the way a portal / pipe does
		// (TMarDirector::setNextStage with the entrance's value, e.g. stage200 = Bianco
		// Hills through the episode select).
		const u16 v = (u16)strtoul(sCall + 5, nullptr, 16);
		OSReport("[port] SMS_PORT_DEBUG_CALL: stage %x\n", v);
		gpMarDirector->setNextStage(v, nullptr);
	} else if (strncmp(sCall, "tp", 2) == 0 && gpMarioOriginal != nullptr) {
		// (w12 mechanics checklist) "tp<x>;<y>;<z>@<frame>": put Mario at a world
		// position, to reach an object a script cannot walk to.
		float x = 0.0f, y = 0.0f, z = 0.0f;
		if (sscanf(sCall + 2, "%f;%f;%f", &x, &y, &z) == 3) {
			gpMarioOriginal->mPosition.set(x, y, z);
			OSReport("[port] SMS_PORT_DEBUG_CALL: tp %.0f %.0f %.0f\n", x, y, z);
		}
	} else if (strncmp(sCall, "hmap", 4) == 0 && gpMap != nullptr) {
		// (w12 episode objectives) "hmap<step>[;<y>]@<frame>": ground heights of the
		// whole stage on a <step>-unit grid (x, z in -26000..26000), probed from y (default
		// 30000, i.e. the top surface), written to hmap_<area>_<scenario>_<y>_f<frame>.bin in the
		// working directory: "HMP2", x0, z0, step, w, h (s32), then w*h (f32 height, u16
		// BG type, u8 wall within 50 units at 60 above the ground) records, for planning routes.
		// (Map objects often add their collision only while Mario is near them.)
		// Read-only.
		int step = atoi(sCall + 4);
		if (step < 25)
			step = 100;
		const char* semi = strchr(sCall, ';');
		f32 fromY        = semi != nullptr ? (f32)atof(semi + 1) : 30000.0f;
		// optional ";<x0>;<z0>;<x1>;<z1>": only that rectangle (faster)
		int rx0 = -26000, rz0 = -26000, rx1 = 26000, rz1 = 26000;
		if (semi != nullptr)
			sscanf(semi + 1, "%*f;%d;%d;%d;%d", &rx0, &rz0, &rx1, &rz1);
		const s32 x0 = rx0, z0 = rz0, w = (rx1 - rx0) / step, h = (rz1 - rz0) / step;
		char path[64];
		snprintf(path, sizeof(path), "hmap_%d_%d_%d_f%u.bin", gpApplication.mCurrArea.getStage(),
		         gpApplication.mCurrArea.getScenario(), (int)fromY, port_frame_count());
		FILE* fp = fopen(path, "wb");
		if (fp != nullptr) {
			const s32 hdr[5] = { x0, z0, step, w, h };
			fwrite("HMP2", 1, 4, fp);
			fwrite(hdr, sizeof(hdr), 1, fp);
			for (s32 j = 0; j < h; j++)
				for (s32 i = 0; i < w; i++) {
					const TBGCheckData* plane = nullptr;
					f32 y = gpMap->checkGround((f32)(x0 + i * step), fromY, (f32)(z0 + j * step),
					                           &plane);
					u16 type = plane != nullptr ? plane->mBGType : 0xFFFF;
					// a wall within 50 units, 60 above that ground (what Mario would bump)
					u8 wall = y > -20000.0f
					              && gpMap->isTouchedOneWall((f32)(x0 + i * step), y + 60.0f,
					                                         (f32)(z0 + j * step), 50.0f);
					fwrite(&y, 4, 1, fp);
					fwrite(&type, 2, 1, fp);
					fwrite(&wall, 1, 1, fp);
				}
			fclose(fp);
		}
		OSReport("[port] SMS_PORT_DEBUG_CALL: hmap %s (%dx%d, step %d)\n", path, w, h, step);
	} else if (strncmp(sCall, "npcs", 4) == 0 && gpMarDirector != nullptr
	           && gpMarioOriginal != nullptr) {
		// (w12) list the talk candidates TMarDirector::findNearestTalkNPC looks at, with
		// their live flags (0x20000 = talkable, 0x100000 = excluded) and distance.
		JGadget::TVector_pointer<TBaseNPC>& v = gpMarDirector->unk88;
		OSReport("[npcs] %d NPCs, mario %.0f %.0f %.0f act %08x\n", (int)(v.end() - v.begin()),
		         gpMarioOriginal->mPosition.x, gpMarioOriginal->mPosition.y,
		         gpMarioOriginal->mPosition.z, gpMarioOriginal->mStatus);
		for (JGadget::TVector_pointer<TBaseNPC>::iterator it = v.begin(); it != v.end(); ++it) {
			TBaseNPC* n = *it;
			f32 dx = n->mPosition.x - gpMarioOriginal->mPosition.x;
			f32 dz = n->mPosition.z - gpMarioOriginal->mPosition.z;
			char hx[2 * 24 + 1] = {};
			for (int i = 0; n->getName()[i] != '\0' && i < 24; i++)
				sprintf(hx + 2 * i, "%02x", (unsigned char)n->getName()[i]);
			OSReport("[npcs]  %s at %.0f %.0f %.0f dist %.0f flags %08x\n", hx, n->mPosition.x,
			         n->mPosition.y, n->mPosition.z, sqrtf(dx * dx + dz * dz), n->mLiveFlag);
		}
		// and what Mario is touching (THitActor::mCollisions): type, damage radius
		TMario* m = gpMarioOriginal;
		for (int i = 0; i < (int)m->mColCount; i++) {
			THitActor* a = m->mCollisions[i];
			f32 dx = a->mPosition.x - m->mPosition.x, dz = a->mPosition.z - m->mPosition.z;
			char hx[2 * 24 + 1] = {};
			for (int j = 0; a->getName() != nullptr && a->getName()[j] != '\0' && j < 24; j++)
				sprintf(hx + 2 * j, "%02x", (unsigned char)a->getName()[j]);
			OSReport("[npcs]  touching %s type %08x at %.0f %.0f %.0f dist %.0f dmgR %.0f atkR %.0f\n",
			         hx, a->getActorType(), a->mPosition.x, a->mPosition.y, a->mPosition.z,
			         sqrtf(dx * dx + dz * dz), a->getDamageRadius(), a->getAttackRadius());
		}
	} else if (strncmp(sCall, "hatch", 5) == 0) {
		// (w12 mechanics checklist) Hatch the stage's Yoshi egg as if it had been
		// fed: what TEggYoshi::control's state 0xB does once the "happy" animation
		// ends (the egg's favourite fruit is random, the fruit is picked here),
		// then put Mario 300 units from it so a script can walk over and ride.
		TEggYoshi* egg = (TEggYoshi*)JDrama::TNameRefGen::search("ヨッシーの卵");
		TYoshi* yoshi  = SMS_GetYoshi();
		static const char* const kFruits[] = { "無限フルーツ", "やしの実（無限） 0" };
		THitActor* fruit = nullptr;
		for (int i = 0; i < 2 && fruit == nullptr; i++)
			fruit = (THitActor*)JDrama::TNameRefGen::search(kFruits[i]);
		OSReport("[port] SMS_PORT_DEBUG_CALL: hatch, egg %p yoshi %p fruit %p\n", egg,
		         yoshi, fruit);
		if (egg != nullptr && fruit != nullptr && yoshi != nullptr && !yoshi->isHatched()) {
			egg->unk150 = fruit; // appearFromEgg() eats the fruit the egg was fed
			JGeometry::TVec3<f32> pos = egg->mPosition;
			yoshi->appearFromEgg(pos, egg->mRotation.y, egg);
			yoshi->setEggYoshiPtr(egg);
			egg->mState = 0xC;
			if (gpMarioOriginal != nullptr) {
				gpMarioOriginal->mPosition = egg->mPosition;
				gpMarioOriginal->mPosition.z += 300.0f;
			}
		}
	}
}

// SMS_PORT_TRACE=obj (w12 episode objectives, read-only): one line per actor loaded
// from a scene (enemies/NPCs via TLiveActor::load, map objects via TMapObjBase::load):
// "[obj] <kind> <extra> at x y z name <SJIS name in hex>", to find routes and targets.
extern "C" void port_trace_actor(const char* kind, const char* extra, const char* name, float x,
                                 float y, float z)
{
	static const char* sTrace = getenv("SMS_PORT_TRACE");
	if (sTrace == nullptr || strstr(sTrace, "obj") == nullptr)
		return;
	char hx[2 * 48 + 1] = {};
	for (int i = 0; name != nullptr && name[i] != '\0' && i < 48; i++)
		sprintf(hx + 2 * i, "%02x", (unsigned char)name[i]);
	OSReport("[obj] %s %s at %.0f %.0f %.0f name %s\n", kind, extra ? extra : "-", x, y, z, hx);
}

// SMS_PORT_ROUTE=<file> (w12 episode objectives, debug): scripted input as a list of
// steps, each held until its condition holds instead of at fixed frames, so a route
// keeps working when loading or demo lengths change. One step per line ('#' starts a
// comment): <until> [args] [modifiers]
//   frames N         N presented frames
//   ctl              Mario has control (director state 4, no talk/demo)
//   talk             a talk or demo is running (director mode != 0)
//   go X Z [R]       the autopilot walks Mario until he is within R (default 80) of
//                    (X, Z); "go @<name> [R]" walks to the object with that (SJIS) name
//   near X Y Z R     Mario within R of (X, Y, Z) (3D)
//   stage A          area A is running and Mario has control
//   shine            the shine count went up (since the route started)
//   red N / coins N / blue N   that counter reached N
//   land             Mario is not airborne;  air: Mario is airborne
//   act HEX          Mario's action is HEX
//   y< V / y> V      Mario's height below / above V (also x<, x>, z<, z>)
//   cond B / ncond B gPortCond bit B (port_crash.cpp) set / clear
// Modifiers: +BTN (held, BTN = A B X Y Z L R START UP DOWN LEFT RIGHT), ^BTN/P (pressed
// for 4 frames every P frames), %BTN/P (held, but released for 4 frames every P frames:
// FLUDD's stream needs R pumped), S<deg> (stick fully tilted, 0 up, 90 right),
// T<x>;<z> (stick toward a world point, camera-compensated), Q<0-100> (stick strength),
// thru (go: steer straight through the target, done once past it), max N (give up after N
// frames, logged as TIMEOUT, and go on), stop (end the route
// if this step times out). Input is put on pad 0 by port_PADRead (port_main.cpp).
struct PortRouteStep {
	char kind[8];
	f32 a[4];
	int nargs;
	u32 u0; // first argument as an integer (act HEX)
	char name[64];
	u16 hold, tap, pump;
	int tapPeriod, pumpPeriod;
	int stickDeg;
	bool toward;
	f32 tx, tz;
	int strength;
	int maxFrames;
	bool stopOnTimeout;
	bool thru; // go: steer through the target (past it), done when passed
	int line;
};
static PortRouteStep* sRoute;
static int sRouteCount = -1;

static u16 portRouteButton(const char* s)
{
	static const struct {
		const char* name;
		u16 mask;
	} kNames[] = { { "START", PAD_BUTTON_START }, { "A", PAD_BUTTON_A },  { "B", PAD_BUTTON_B },
		           { "X", PAD_BUTTON_X },         { "Y", PAD_BUTTON_Y },  { "Z", PAD_TRIGGER_Z },
		           { "L", PAD_TRIGGER_L },        { "R", PAD_TRIGGER_R }, { "UP", PAD_BUTTON_UP },
		           { "DOWN", PAD_BUTTON_DOWN },   { "LEFT", PAD_BUTTON_LEFT },
		           { "RIGHT", PAD_BUTTON_RIGHT } };
	u16 mask = 0;
	char buf[32];
	strncpy(buf, s, sizeof(buf) - 1);
	buf[sizeof(buf) - 1] = '\0';
	for (char* tok = strtok(buf, "+"); tok != nullptr; tok = strtok(nullptr, "+")) {
		bool named = false;
		for (u32 i = 0; i < sizeof(kNames) / sizeof(kNames[0]); i++)
			if (strcmp(tok, kNames[i].name) == 0) {
				mask |= kNames[i].mask;
				named = true;
			}
		if (!named)
			mask |= (u16)strtoul(tok, nullptr, 16);
	}
	return mask;
}

static void portRouteLoad()
{
	sRouteCount     = 0;
	const char* env = getenv("SMS_PORT_ROUTE");
	if (env == nullptr)
		return;
	FILE* fp = fopen(env, "rb");
	if (fp == nullptr) {
		OSReport("[route] cannot open %s\n", env);
		return;
	}
	static PortRouteStep sSteps[1024];
	sRoute = sSteps;
	char line[512];
	int lineNo = 0;
	while (fgets(line, sizeof(line), fp) != nullptr) {
		lineNo++;
		char* hash = strchr(line, '#');
		if (hash != nullptr)
			*hash = '\0';
		char* save1 = nullptr;
		for (char* stmt = strtok_s(line, "\r\n", &save1); stmt != nullptr;
		     stmt       = strtok_s(nullptr, "\r\n", &save1)) {
			PortRouteStep st;
			memset(&st, 0, sizeof(st));
			st.stickDeg = -1;
			st.strength = 100;
			st.line     = lineNo;
			char* save2 = nullptr;
			char* tok   = strtok_s(stmt, " \t", &save2);
			if (tok == nullptr)
				continue;
			strncpy(st.kind, tok, sizeof(st.kind) - 1);
			while ((tok = strtok_s(nullptr, " \t", &save2)) != nullptr) {
				if (tok[0] == '+')
					st.hold |= portRouteButton(tok + 1);
				else if (tok[0] == '^') {
					char* slash = strchr(tok, '/');
					st.tapPeriod = slash != nullptr ? atoi(slash + 1) : 10;
					if (slash != nullptr)
						*slash = '\0';
					st.tap |= portRouteButton(tok + 1);
				} else if (tok[0] == '%') {
					char* slash = strchr(tok, '/');
					st.pumpPeriod = slash != nullptr ? atoi(slash + 1) : 60;
					if (slash != nullptr)
						*slash = '\0';
					st.pump |= portRouteButton(tok + 1);
				} else if (tok[0] == 'S' && tok[1] >= '0' && tok[1] <= '9')
					st.stickDeg = atoi(tok + 1);
				else if (tok[0] == 'T' && (tok[1] == '-' || (tok[1] >= '0' && tok[1] <= '9'))) {
					st.toward = sscanf(tok + 1, "%f;%f", &st.tx, &st.tz) == 2;
				} else if (tok[0] == 'Q' && tok[1] >= '0' && tok[1] <= '9')
					st.strength = atoi(tok + 1);
				else if (strcmp(tok, "max") == 0) {
					tok = strtok_s(nullptr, " \t", &save2);
					if (tok != nullptr)
						st.maxFrames = atoi(tok);
				} else if (strcmp(tok, "stop") == 0)
					st.stopOnTimeout = true;
				else if (strcmp(tok, "thru") == 0)
					st.thru = true;
				else if (tok[0] == '@')
					strncpy(st.name, tok + 1, sizeof(st.name) - 1);
				else if (st.nargs < 4) {
					if (st.nargs == 0)
						st.u0 = strtoul(tok, nullptr, 0);
					st.a[st.nargs++] = (f32)strtod(tok, nullptr);
				}
			}
			if (sRouteCount < 1024)
				sRoute[sRouteCount++] = st;
		}
	}
	fclose(fp);
	OSReport("[route] %d steps from %s\n", sRouteCount, env);
}

// Stick (x, y) in -100..100 that makes Mario head toward world (x, z): the inverse of
// TMario's mIntendedYaw = matan(-stickY, stickX) + camera angle (as portAutopilot).
static bool portRouteStickToward(f32 x, f32 z, int strength, int* sx, int* sy)
{
	TMario* m = gpMarioOriginal;
	if (m == nullptr || gpCamera == nullptr)
		return false;
	s16 rel = (s16)(matan(z - m->mPosition.z, x - m->mPosition.x) - gpCamera->getUnk258());
	f32 rad = rel * (3.14159265f / 32768.0f);
	*sx     = (int)(strength * sinf(rad));
	*sy     = (int)(-strength * cosf(rad));
	return true;
}

extern "C" void port_route_pad(PADStatus* status)
{
	if (sRouteCount < 0)
		portRouteLoad();
	static int sIdx;
	static bool sStarted;
	static u32 sStart;
	static int sBaseShines = -1;
	static f32 sGoX, sGoZ, sFromX, sFromZ;
	static u32 sLastFlush;
	if (sRouteCount > 0 && port_frame_count() / 60 != sLastFlush) {
		sLastFlush = port_frame_count() / 60;
		fflush(stdout); // test runs are killed at their time limit: keep the log
	}
	if (sIdx >= sRouteCount)
		return;
	const u32 now     = port_frame_count();
	TFlagManager* f   = TFlagManager::getInstance();
	TMarDirector* d   = gpMarDirector;
	TMario* m         = gpMarioOriginal;
	TApplication* app = &gpApplication;
	const bool inStage
	    = d != nullptr && app->mDirector == (JDrama::TDirector*)d && m != nullptr;
	if (sBaseShines < 0 && inStage)
		sBaseShines = f->getFlag(MSF_SHINE_COUNT);
	PortRouteStep& st = sRoute[sIdx];
	if (!sStarted) {
		sStarted = true;
		sStart   = now;
		sGoX     = st.a[0];
		sGoZ     = st.a[1];
		sFromX   = inStage ? m->mPosition.x : 0.0f;
		sFromZ   = inStage ? m->mPosition.z : 0.0f;
		if (st.name[0] != '\0') {
			JDrama::TActor* obj = (JDrama::TActor*)JDrama::TNameRefGen::search(st.name);
			if (obj != nullptr) {
				sGoX = obj->mPosition.x;
				sGoZ = obj->mPosition.z;
			}
			OSReport("[route] step %d: object %s at %.0f %.0f\n", sIdx, obj ? "found" : "NOT found",
			         sGoX, sGoZ);
		}
	}
	const u32 el  = now - sStart;
	const bool ctl = inStage && d->mState == 4 && d->unk124 == 0;
	bool done      = false;
	const char* k  = st.kind;
	if (strcmp(k, "frames") == 0)
		done = el >= (u32)st.a[0];
	else if (strcmp(k, "ctl") == 0)
		done = ctl;
	else if (strcmp(k, "talk") == 0)
		done = inStage && d->unk124 != 0;
	else if (strcmp(k, "go") == 0) {
		f32 r  = st.name[0] != '\0' ? (st.nargs > 0 ? st.a[0] : 80.0f)
		                            : (st.nargs > 2 ? st.a[2] : 80.0f);
		f32 dx = inStage ? sGoX - m->mPosition.x : 1e9f;
		f32 dz = inStage ? sGoZ - m->mPosition.z : 1e9f;
		done   = dx * dx + dz * dz < r * r;
		if (st.thru && inStage && !done) {
			// passed the target: Mario is beyond it along the line from the step's start
			f32 lx = sGoX - sFromX, lz = sGoZ - sFromZ;
			done   = (m->mPosition.x - sFromX) * lx + (m->mPosition.z - sFromZ) * lz >= lx * lx + lz * lz;
		}
	} else if (strcmp(k, "near") == 0 && inStage) {
		f32 dx = st.a[0] - m->mPosition.x, dy = st.a[1] - m->mPosition.y,
		    dz = st.a[2] - m->mPosition.z;
		done   = dx * dx + dy * dy + dz * dz < st.a[3] * st.a[3];
	} else if (strcmp(k, "stage") == 0)
		done = ctl && app->mCurrArea.getStage() == (u8)st.a[0];
	else if (strcmp(k, "shine") == 0)
		done = sBaseShines >= 0 && f->getFlag(MSF_SHINE_COUNT) > sBaseShines;
	else if (strcmp(k, "red") == 0)
		done = f->getFlag(MSF_RED_COIN_COUNT) >= (s32)st.a[0];
	else if (strcmp(k, "coins") == 0)
		done = f->getFlag(MSF_GOLD_COIN_COUNT) >= (s32)st.a[0];
	else if (strcmp(k, "blue") == 0)
		done = f->getFlag(MSF_BLUE_COIN_COUNT) >= (s32)st.a[0];
	else if (strcmp(k, "land") == 0)
		done = inStage && !(m->mStatus & MARIO_STATUS_FLAG_JUMPING);
	else if (strcmp(k, "air") == 0)
		done = inStage && (m->mStatus & MARIO_STATUS_FLAG_JUMPING);
	else if (strcmp(k, "act") == 0)
		done = inStage && m->mStatus == st.u0;
	else if (strcmp(k, "y<") == 0)
		done = inStage && m->mPosition.y < st.a[0];
	else if (strcmp(k, "y>") == 0)
		done = inStage && m->mPosition.y > st.a[0];
	else if (strcmp(k, "x<") == 0)
		done = inStage && m->mPosition.x < st.a[0];
	else if (strcmp(k, "x>") == 0)
		done = inStage && m->mPosition.x > st.a[0];
	else if (strcmp(k, "z<") == 0)
		done = inStage && m->mPosition.z < st.a[0];
	else if (strcmp(k, "z>") == 0)
		done = inStage && m->mPosition.z > st.a[0];
	else if (strcmp(k, "cond") == 0)
		done = (gPortCond >> ((u32)st.a[0] & 31)) & 1;
	else if (strcmp(k, "ncond") == 0)
		done = !((gPortCond >> ((u32)st.a[0] & 31)) & 1);
	const bool timeout = !done && st.maxFrames > 0 && el >= (u32)st.maxFrames;
	static const char* const kKinds[] = { "frames", "ctl",  "talk", "go",  "near", "stage",
		                                  "shine",  "red",  "coins", "blue", "land", "air",
		                                  "act",    "y<",   "y>",   "cond", "ncond",
		                                  "x<",     "x>",   "z<",   "z>" };
	bool known = false;
	for (u32 i = 0; i < sizeof(kKinds) / sizeof(kKinds[0]); i++)
		known = known || strcmp(k, kKinds[i]) == 0;
	if (!known) {
		OSReport("[route] step %d (line %d): unknown condition '%s', skipped\n", sIdx, st.line, k);
		sIdx++;
		sStarted = false;
		return;
	}
	if (done || timeout) {
		OSReport("[route] f%u step %d (line %d) %s %s after %u frames, pos %.0f %.0f %.0f\n", now,
		         sIdx, st.line, k, done ? "done" : "TIMEOUT", el, inStage ? m->mPosition.x : 0.0f,
		         inStage ? m->mPosition.y : 0.0f, inStage ? m->mPosition.z : 0.0f);
		sIdx     = (timeout && st.stopOnTimeout) ? sRouteCount : sIdx + 1;
		sStarted = false;
		if (sIdx >= sRouteCount)
			OSReport("[route] f%u route finished\n", now);
		fflush(stdout); // a killed test run keeps its route log
		return;
	}
	u16 add = st.hold;
	if (st.tap != 0 && st.tapPeriod > 0 && el % (u32)st.tapPeriod < 4)
		add |= st.tap;
	if (st.pump != 0 && st.pumpPeriod > 0 && el % (u32)st.pumpPeriod >= 4)
		add |= st.pump;
	int sx = 0, sy = 0;
	bool stick = false;
	if (strcmp(k, "go") == 0 && st.thru) {
		// aim 2000 units past the target on the line from the step's start, so a fast
		// vehicle (Blooper, Yoshi) runs straight through it instead of circling
		f32 lx = sGoX - sFromX, lz = sGoZ - sFromZ;
		f32 len = sqrtf(lx * lx + lz * lz);
		if (len < 1.0f)
			len = 1.0f;
		stick = portRouteStickToward(sGoX + lx / len * 2000.0f, sGoZ + lz / len * 2000.0f,
		                             st.strength, &sx, &sy);
	} else if (strcmp(k, "go") == 0)
		stick = portRouteStickToward(sGoX, sGoZ, st.strength, &sx, &sy);
	else if (st.toward)
		stick = portRouteStickToward(st.tx, st.tz, st.strength, &sx, &sy);
	else if (st.stickDeg >= 0) {
		const f32 rad = st.stickDeg * (3.14159265f / 180.0f);
		sx            = (int)(st.strength * sinf(rad));
		sy            = (int)(st.strength * cosf(rad));
		stick         = true;
	}
	if (stick) {
		status[0].stickX = (s8)sx;
		status[0].stickY = (s8)sy;
	}
	if (add != 0)
		status[0].button |= add;
	if (stick || add != 0)
		status[0].err = PAD_ERR_NONE;
	if ((add & PAD_TRIGGER_R) && status[0].triggerRight < 200)
		status[0].triggerRight = 255;
	if ((add & PAD_TRIGGER_L) && status[0].triggerLeft < 200)
		status[0].triggerLeft = 255;
}
#endif

TMarDirector* gpMarDirector;
// US splits place gpPositionHolder in Application's .sbss; the real owner
// (System/PositionHolder.cpp) stays original in link-all-source builds.
TNameRefAryT<TStagePositionInfo>* gpPositionHolder;
MSound* gpMSound;

OSThread gSetupThread;
u8* gpSetupThreadStack;

TCardManager* gpCardManager;
JUTResFont* gpSystemFont;
JUTRomFont* gpRomFont;

static int sGameInit;
static void* arcBufNLogo;
static void* arcBufMario;
static void* arcBufCmn;
static void* bufStageArcBin;
static void* spGameHeapBlock;
#if defined(VERSION_GMSP01) && !defined(VERSION_GMSE01) // PAL language loading
static JKRMemArchive* sCmn2DArc;
static void* sCmn2DArcBuf;
#endif

TARAMBlock gArBkConsole;
TARAMBlock gArBkGuide;

extern "C" void ReInitializeGX();
u8 SMS_getShineIDofExStage(u8);

#if defined(VERSION_GMSP01) && !defined(VERSION_GMSE01)
f32 SMSGetRealVSyncTimesPerSec()
{
	f32 result = 60.0f;
	switch (VIGetTvFormat()) {
	case VI_MPAL:
	case VI_NTSC:
	case VI_EURGB60:
		result = 60.0f;
		break;
	case VI_PAL:
		result = 50.0f;
		break;
	}
	return result;
}

f32 SMSGetVSyncTimesPerSec() { return SMSGetRealVSyncTimesPerSec() / 2.0f; }
#else

f32 SMSGetVSyncTimesPerSec()
{
	f32 result = 60.0f;
	switch (VIGetTvFormat()) {
	case VI_MPAL:
	case VI_NTSC:
	case VI_EURGB60:
		result = 60.0f;
		break;
	case VI_PAL:
		result = 50.0f;
		break;
	}
	return result / 2.0f;
}

#endif

f32 SMSGetAnmFrameRate() { return 60.0f / SMSGetVSyncTimesPerSec(); }

TApplication::TApplication()
    : mSelf(this)
    , mDirector(nullptr)
    , mDisplay(nullptr)
    , unk30(nullptr)
    , unk3C(0)
    , mHeap(nullptr)
    , unk44(0)
    , unk46(0)
{
}

void* SMSLoadArchive(const char* path, void* dst, u32 dstLength, JKRHeap* heap)
{
	if (heap == nullptr)
		heap = JKRGetCurrentHeap();

	void* result = nullptr;

	// Try to load a compressed version of the archive first
	char compressedArcPath[64];
	strcpy(compressedArcPath, path);
	char* loc = strstr(compressedArcPath, ".arc");
	if (loc != nullptr) {
		strcpy(loc, ".szs");
		s32 entryNum = DVDConvertPathToEntrynum(compressedArcPath);
		if (entryNum != -1) {
			result = JKRDvdToMainRam(compressedArcPath, (u8*)dst,
			                         EXPAND_SWITCH_DECOMPRESS, dstLength, heap,
			                         JKRDvdRipper::ALLOC_DIRECTION_FORWARD, 0,
			                         nullptr);
		}
	}

	// If that fails, then try to load the uncompressed version
	if (result == nullptr)
		result = JKRDvdToMainRam(
		    path, (u8*)dst, EXPAND_SWITCH_DEFAULT, dstLength, heap,
		    JKRDvdRipper::ALLOC_DIRECTION_FORWARD, 0, nullptr);

	return result;
}

void SMSLoadArchiveARAM(TARAMBlock* out_block, const char* path)
{
	// Try to load a compressed version of the archive first
	char compressedArcPath[64];
	strcpy(compressedArcPath, path);
#if defined(VERSION_GMSP01) && !defined(VERSION_GMSE01)
	out_block->mBlock = nullptr;
#endif
	char* loc = strstr(compressedArcPath, ".arc");
	if (loc != nullptr) {
		strcpy(loc, ".szs");
		s32 entryNum = DVDConvertPathToEntrynum(compressedArcPath);
		if (entryNum != -1) {
			out_block->mBlock = JKRDvdAramRipper::loadToAram(
			    compressedArcPath, 0, EXPAND_SWITCH_DEFAULT, 0, 0);
			out_block->mIsCompressed = true;
		}
	}

	// If that fails, then try to load the uncompressed version
	if (out_block->mBlock == nullptr) {
		out_block->mBlock = JKRDvdAramRipper::loadToAram(
		    (char*)path, 0, EXPAND_SWITCH_DEFAULT, 0, 0);
		out_block->mIsCompressed = false;
	}
}

void SMSMountAramArchive(JKRMemArchive* archive, TARAMBlock& block)
{
    // FAKEMATCH(frame): 16 bytes; real construct unknown
    volatile u32 pad[4]; (void)pad;
	if (block.mIsCompressed) {
		JKRAram::aramToMainRam(
		    block.mBlock, (u8*)SMSGetMarDirector()->getUnkD4(), 0, 0,
		    EXPAND_SWITCH_DECOMPRESS, 0x64000, nullptr, -1, nullptr);
	} else {
		JKRAram::aramToMainRam(block.mBlock,
		                       (u8*)SMSGetMarDirector()->getUnkD4(), 0, 0,
		                       EXPAND_SWITCH_DEFAULT, 0, nullptr, -1, nullptr);
	}
	archive->mountFixed(SMSGetMarDirector()->getUnkD4(), MBF_0);
}

JKRArchive* SMSSwitch2DArchive(const char* arc_path, TARAMBlock& block)
{
    // FAKEMATCH(frame): 16 bytes; real construct unknown
    volatile u32 pad[4]; (void)pad;
	JKRMemArchive* arch = (JKRMemArchive*)JKRFileLoader::getVolume(arc_path);
	arch->unmountFixed();
	SMSMountAramArchive(arch, block);
	return arch;
}

void* TApplication::setupThreadFuncBoot()
{
	arcBufNLogo = SMSLoadArchive("/data/nintendo.arc", nullptr, 0, nullptr);
	return nullptr;
}

static void* SetupThreadFuncBoot(void* param)
{
	return ((TApplication*)param)->setupThreadFuncBoot();
}

extern void MarErrInit();

void TApplication::initialize()
{
	OSProtectRange(0, nullptr, 0x80000000, 0);
	OSProtectRange(1, (void*)0x83000000, 0x7d000000, 0);
	OSInitFastCast();
	JKRDvdRipper::setSzpBufferSize(0x1000);
	JKRDvdAramRipper::setSzpBufferSize(0x1000);
	JKRExpHeap::createRoot(1, false);
	JKRExpHeap::create(0x20000, JKRGetRootHeap(), false)->becomeSystemHeap();
	JKRGetRootHeap()->becomeCurrentHeap();
	DVDInit();
	MarErrInit();
	VIInit();
	JUTGamePad::init();
	GXInit(JKRAllocFromHeap(nullptr, 0x80000, 0x20), 0x80000);
	SMS_ResetTexCacheRegion();
	GXPokeAlphaRead(GX_READ_NONE);
	void* pvVar3 = new (0x20) u8[0xa5000];
	GXRenderModeObj rmode;
	SMSSetupTitleRenderMode(&rmode);
	mDisplay = new JDrama::TDisplay(2, pvVar3, pvVar3, rmode);

	JKRHeap* prevHeap = JKRGetCurrentHeap();

	spGameHeapBlock = new (-0x20) u8[0x500000];
	mHeap = JKRExpHeap::create(spGameHeapBlock, 0x500000, JKRGetCurrentHeap(),
	                           true);

	gpRomFont = new JUTRomFont(JKRGetCurrentHeap());
	prevHeap->becomeCurrentHeap();

	JKRAram::create(0xf40000, -1, 8, 8, 8);
	JKRAramStream::setTransBuffer(new (0x20) u8[0x8000], 0x8000, nullptr);
	TDrawSyncManager::start(5, 0x28, 0xf);
	GXSetDrawSyncCallback(&TDrawSyncManager::drawSyncCallback);

	mGamePads[0] = new TMarioGamePad(JUTGamePad::EPort1);
	mGamePads[1] = new TMarioGamePad(JUTGamePad::EPort2);
	mGamePads[2] = new TMarioGamePad(JUTGamePad::EPort3);
	mGamePads[3] = new TMarioGamePad(JUTGamePad::EPort4);

	SMSRumbleMgr = new RumbleMgr(true, true, true, true);
	SMSRumbleMgr->init();
	mFader = new TSmplFader(JUtility::TColor(0, 0, 0, 0xff),
	                        SMSGetVSyncTimesPerSec(), "ルートフェーダー");
	mFader->setDisplaySize(SMSGetGCLogoRenderWidth(),
	                       SMSGetGCLogoRenderHeight());
	TFlagManager::start(JKRGetCurrentHeap());
	TTimeRec::start(0xDFC0);
	TTimeRec::instance()->mFlags.on(1);
	TDrawSyncManager::smInstance->setCallback(0, 0xDFC0, 0xDFFF,
	                                          TTimeRec::instance());
	mMeter = new TProcessMeter(2);
	srand(OSGetTick());
	gpSetupThreadStack = new (0x20) u8[0x10000];

	mAppState = APP_STATE_BOOT;

	mHeap->becomeCurrentHeap();
	OSCreateThread(&gSetupThread, &SetupThreadFuncBoot, this,
	               gpSetupThreadStack + 0x10000, 0x10000, 0x11, 0);
	OSResumeThread(&gSetupThread);
}

#if defined(VERSION_GMSP01) && !defined(VERSION_GMSE01) // PAL language loading
#pragma dont_inline on
void load2DResource2Aram()
{
	static u8 sLoadResourceLang     = -1;
	static const char* cmn2dNames[] = {
		"/data/cmn2d_en.arc", "/data/cmn2d_ge.arc", "/data/cmn2d_fr.arc",
		"/data/cmn2d_sp.arc", "/data/cmn2d_it.arc",
	};
	static const char* game_6Names[] = {
		"/data/game_6_en.arc", "/data/game_6_ge.arc", "/data/game_6_fr.arc",
		"/data/game_6_sp.arc", "/data/game_6_it.arc",
	};
	static const char* guideNames[] = {
		"/data/guide_en.arc", "/data/guide_ge.arc", "/data/guide_fr.arc",
		"/data/guide_sp.arc", "/data/guide_it.arc",
	};

	if (sLoadResourceLang
	    != (u8)TFlagManager::getInstance()->getFlag(MSF_LANGUAGE)) {
		sLoadResourceLang
		    = (u8)TFlagManager::getInstance()->getFlag(MSF_LANGUAGE);
		if (JKRFileLoader::getVolume("cmn2d") != nullptr)
			sCmn2DArc->unmountFixed();

		SMSLoadArchive(cmn2dNames[sLoadResourceLang], sCmn2DArcBuf, 0xA000,
		               nullptr);
		sCmn2DArc->mountFixed(sCmn2DArcBuf, MBF_0);
		JKRAram::getAramHeap()->freeAll();

		SMSLoadArchiveARAM(&gArBkConsole, game_6Names[sLoadResourceLang]);
		SMSLoadArchiveARAM(&gArBkGuide, guideNames[sLoadResourceLang]);
	}
}
#pragma dont_inline off
#endif

#pragma dont_inline on
void* TApplication::setupThreadFuncLogo()
{
	while (!SMSGetMSound()->checkWaveOnAram(MS_WAVE_UNK0))
		OSYieldThread();
	while (!SMSGetMSound()->checkWaveOnAram(MS_WAVE_UNK210))
		OSYieldThread();

	arcBufMario
	    = SMSLoadArchive("/data/mario.arc", nullptr, 0, JKRGetRootHeap());

	arcBufCmn
	    = SMSLoadArchive("/data/common.arc", nullptr, 0, JKRGetRootHeap());

	bufStageArcBin = JKRDvdToMainRam(
	    "/data/stageArc.bin", nullptr, EXPAND_SWITCH_DEFAULT, 0, mHeap,
	    JKRDvdRipper::ALLOC_DIRECTION_FORWARD, 0, nullptr);

#if defined(VERSION_GMSP01) && !defined(VERSION_GMSE01)
	load2DResource2Aram();
#else
	SMSLoadArchiveARAM(&gArBkConsole, "/data/game_6.arc");
	SMSLoadArchiveARAM(&gArBkGuide, "/data/guide.arc");
#endif

	return nullptr;
}
#pragma dont_inline off

static void* SetupThreadFuncLogo(void* param)
{
	return ((TApplication*)param)->setupThreadFuncLogo();
}

void TApplication::initialize_bootAfter()
{
	JKRGetRootHeap()->becomeCurrentHeap();
	JKRMemArchive* this_01 = new (JKRGetSystemHeap(), 0) JKRMemArchive;
	this_01->mountFixed(arcBufNLogo, MBF_0);

	this_01->becomeCurrent("/font");
	void* fontRes = this_01->getResource("standard_fontEx.bfn");
	u32 uVar1     = this_01->getResSize(fontRes);
	ResFONT* font = (ResFONT*)new (0x20) u8[uVar1];
	this_01->readResource(font, uVar1, "standard_fontEx.bfn");
	gpSystemFont = new JUTResFont(font, nullptr);

	this_01->becomeCurrent("/audi");
	void* aafRes = this_01->getResource("mSound.aaf");
	u32 uVar3    = this_01->getResSize(aafRes);
	u8* buf      = new u8[uVar3];
	this_01->readResource(buf, uVar3, "mSound.aaf");
	JKRHeap* prevHeap = JKRGetCurrentHeap();
	gpMSound = new MSound(prevHeap, nullptr, 0xF40000, buf, nullptr, 0xb00000);

	JAIGlobalParameter::setParamSoundOutputMode(!OSGetSoundMode() ? 0 : 1);
	gpMSound->loadWave(MS_WAVE_UNK0);
	gpMSound->loadWave(MS_WAVE_UNK210);
	prevHeap->becomeCurrentHeap();

	CARDInit();

	void* sectorWorkArea    = new (0x20) u8[0x2000];
	void* cardWorkArea      = new (0x20) u8[0xA000];
	void* workerThreadStack = new (0x20) u8[0x1000];
	gpCardManager = new TCardManager(sectorWorkArea, cardWorkArea, 0, 14,
	                                 workerThreadStack, 0x1000);
	gpCardManager->readOptionBlock();

#if defined(VERSION_GMSP01) && !defined(VERSION_GMSE01)
	sCmn2DArcBuf = new (0x20) u8[0xA000];
	sCmn2DArc    = new JKRMemArchive;
#endif

	mHeap->becomeCurrentHeap();

	OSCreateThread(&gSetupThread, SetupThreadFuncLogo, this,
	               gpSetupThreadStack + 0x10000, 0x10000, 0x11, 0);
	OSResumeThread(&gSetupThread);
}

void TApplication::initialize_nlogoAfter()
{
	JKRMemArchive* arch = (JKRMemArchive*)JKRFileLoader::getVolume("nintendo");
	arch->unmountFixed();
	delete arch;

	JKRGetRootHeap()->becomeCurrentHeap();

	JKRMemArchive* piVar2 = new JKRMemArchive(arcBufCmn, 0, MBF_0);

	{
		JDrama::TNameRefGen::instance
		    = new (JKRGetSystemHeap(), 0) TMarNameRefGen;

		u32 lVar3 = JKRGetRootHeap()->getSize(bufStageArcBin);
		JSUMemoryInputStream stream(bufStageArcBin, lVar3);
		JDrama::TNameRefGen::getInstance()->load(stream);
		unk30 = static_cast<
		    TNameRefPtrAryT<TNameRefAryT<TScenarioArchiveName> >*>(
		    JDrama::TNameRefGen::search("ステージ毎シナリオアーカイブ名群"));

		delete JDrama::TNameRefGen::instance;
		JDrama::TNameRefGen::instance = nullptr;
	}

	gpRomFont = nullptr;
	((JKRExpHeap*)mHeap)->destroy();
	JKRGetRootHeap()->free(spGameHeapBlock);

	JKRMemArchive* this_00 = new JKRMemArchive(arcBufMario, 0, MBF_0);
	gpCardManager->mIcons
	    = (ResTIMG*)piVar2->getResource("/card/mario_icon.bti") + 1;
	gpCardManager->mBanner
#ifdef VERSION_GMSP01
	    = (ResTIMG*)piVar2->getResource("/card/mariobnr.bti") + 1;
#else
	    = (ResTIMG*)piVar2->getResource("/card/mariobnr_jpn.bti") + 1;
#endif

#if !(defined(VERSION_GMSP01) && !defined(VERSION_GMSE01))
	int status;
	while ((status = gpCardManager->getLastStatus()) == -1)
		OSYieldThread();

	if (status == 0) {
		JSUMemoryInputStream stream;
		gpCardManager->getOptionReadStream(&stream);
		TFlagManager::getInstance()->loadOption(stream);
	}

#endif

	gpCardManager->unmount();

	int outputMode = 1;
	switch (TFlagManager::getInstance()->getFlag(MSF_SOUND_MODE)) {
	case 0:
		outputMode = 0;
		break;
	case 2:
		outputMode = 2;
		break;
	}

	JAIGlobalParameter::setParamSoundOutputMode(outputMode);

	JMANewSinTable(0xC);

	mHeap = JKRSolidHeap::create(JKRGetCurrentHeap()->getFreeSize(),
	                             JKRGetCurrentHeap(), true);
	mHeap->becomeCurrentHeap();
}

void TApplication::initialize_processMeter() { }

void TApplication::finalize()
{
	SMSRumbleMgr->reset();
	if (gpMSound != nullptr)
		gpMSound->resetAudioAll(0x3C);
	JUTGamePad::clearForReset();
	if (gpCardManager != nullptr)
		gpCardManager->~TCardManager();
	VISetBlack(TRUE);
	VIFlush();
	VIWaitForRetrace();
	if (gpMSound != nullptr)
		while (!gpMSound->resetAudioAll(0x3C))
			VIWaitForRetrace();
	OSResetSystem(unk44 & 2 ? 1 : 0, 0, FALSE);
}

bool TApplication::checkAdditionalMovie()
{
    // FAKEMATCH(frame): 48 bytes; real construct unknown
    volatile u32 pad[12]; (void)pad;
	bool result = false;

	const TGameSequence& currArea = SMSGetApplication()->mCurrArea;

	u8 uVar1 = SMS_getShineIDofExStage(currArea.getStage());
	if (uVar1 != 0xFF) {
		if (!TFlagManager::getInstance()->getShineFlag(uVar1)) {
			if (!TFlagManager::getInstance()->getBool(
			        MSF_FLUDD_THEFT_SCENARIO_WATCHED)) {
				mMovie = 5;
				TFlagManager::getInstance()->setBool(
				    true, MSF_FLUDD_THEFT_SCENARIO_WATCHED);
				result = true;
			}
		}
	} else {
		switch (currArea.getStage()) {
		case 0:
			if (currArea.getScenario() == 0) {
				if (!TFlagManager::getInstance()->getBool(
				        MSF_PLANE_CRASH_SCENARIO_WATCHED)) {
					mMovie = 1;
					TFlagManager::getInstance()->setBool(
					    true, MSF_PLANE_CRASH_SCENARIO_WATCHED);
					result = true;
				}
			}
			break;

		case 1:
			if (currArea.getScenario() == 0) {
				if (!TFlagManager::getInstance()->getBool(
				        MSF_COURT_SCENARIO_WATCHED)) {
					mMovie = 3;
					TFlagManager::getInstance()->setBool(
					    true, MSF_COURT_SCENARIO_WATCHED);
					result = true;
				}
			} else if (currArea.getScenario() == 1) {
				if (!TFlagManager::getInstance()->getBool(
				        MSF_PEACH_KIDNAP_SCENARIO_WATCHED)) {
					mMovie = 4;
					TFlagManager::getInstance()->setBool(
					    true, MSF_PEACH_KIDNAP_SCENARIO_WATCHED);
					result = true;
				}
			}
			break;

		case 8:
			if (currArea.getScenario() == 2) {
				if (!TFlagManager::getInstance()->getBool(
				        MSF_FLUDD_THEFT_SCENARIO_WATCHED)) {
					mMovie = 5;
					TFlagManager::getInstance()->setBool(
					    true, MSF_FLUDD_THEFT_SCENARIO_WATCHED);
					result = true;
				}
			}
			break;
		}
	}

	return result;
}

void TApplication::proc()
{
	while (mAppState != APP_STATE_QUIT) {
#if defined(VERSION_GMSP01) && !defined(VERSION_GMSE01)
		mDisplay->unk4C = 2;
#endif
		u8 nextState = APP_STATE_DEFAULT;
		int iVar9    = 0;
#ifdef SMS_PORT
		OSReport("[port] TApplication state %d (area %d/%d)\n", mAppState,
		         mCurrArea.getStage(), mCurrArea.getScenario());
		sPortStageArmed = true;
		gPortCond       = 0; // PADSCRIPT game-state bits belong to the stage that set them
		gPortEnemyMario = nullptr; // set again by the next stage's TEMario::load
		port_prof_stage(mAppState, mCurrArea.getStage(),
		                mCurrArea.getScenario()); // w14
#ifdef SMS_PORT_AURORA
		port_mp_note_area(mAppState, mCurrArea.getStage(), mCurrArea.getScenario()); // w10
#endif
#endif

		switch (mAppState) {
		case APP_STATE_BOOT:
			SMSSetupGCLogoRenderingInfo(mDisplay);
			break;

		case APP_STATE_NLOGO: {
			SMSSetupGCLogoRenderingInfo(mDisplay);
			TGCLogoDir* dir = new TGCLogoDir;
			mDirector       = dir;
			dir->setup(mDisplay, mGamePads[0]);
		} break;

		case APP_STATE_MENU: {
			SMSSetupTitleRenderingInfo(mDisplay);
			mFader->setDisplaySize(SMSGetTitleRenderWidth(),
			                       SMSGetTitleRenderHeight());
			TMenuDirector* dir = new TMenuDirector;
			mDirector          = dir;
			dir->setup(mDisplay, mGamePads[0]);
			TFlagManager::getInstance()->setFlag(MSF_LIFE_COUNT, 3);
			mCurrArea.set(1, 0, 0);
		} break;

		case APP_STATE_GAMEPLAY:
			if (checkAdditionalMovie()) {
				// Show a movie before entering a stage, e.g. the secret levels
				SMSSetupMovieRenderingInfo(mDisplay);
#if defined(VERSION_GMSP01) && !defined(VERSION_GMSE01)
				if ((mDisplay->getRenderMode().viTVmode >> 2) == VI_PAL)
					mDisplay->unk4C = 1;
#endif
				mFader->setDisplaySize((u16)SMSGetGameRenderWidth(),
				                       (u16)SMSGetGameRenderHeight());
				TMovieDirector* dir = new TMovieDirector;
				mDirector           = dir;
				dir->setup(mDisplay, mGamePads[0]);
			} else {
				SMSSetupGameRenderingInfo(mDisplay, (unk44 & 1) != 0);
				mFader->setDisplaySize((u16)SMSGetGameRenderWidth(),
				                       (u16)SMSGetGameRenderHeight());
				TMarDirector* dir = new TMarDirector;
				mDirector         = dir;
				iVar9 = dir->setup(mDisplay, mGamePads, mCurrArea.getStage(),
				                   mCurrArea.getScenario());
				if (iVar9)
					nextState = APP_STATE_DONE;
			}
			break;

		case APP_STATE_TITLE: {
			SMSSetupTitleRenderingInfo(mDisplay);
			mFader->setDisplaySize(SMSGetTitleRenderWidth(),
			                       SMSGetTitleRenderHeight());
			TSelectDir* selectDir = new TSelectDir;
			mDirector             = selectDir;
			selectDir->setup(mDisplay, mGamePads[0], mCurrArea.getStage());
		} break;

		case APP_STATE_DONE:
			SMSGetApplication()->setMovie(9);
			mNextArea.set(15, 0, 0);
			// FALLTHROUGH

		case APP_STATE_MOVIE: {
			SMSSetupMovieRenderingInfo(mDisplay);
#if defined(VERSION_GMSP01) && !defined(VERSION_GMSE01)
			if ((mDisplay->getRenderMode().viTVmode >> 2) == VI_PAL)
				mDisplay->unk4C = 1;
#endif
			mFader->setDisplaySize((u16)SMSGetGameRenderWidth(),
			                       (u16)SMSGetGameRenderHeight());
			TMovieDirector* dir = new TMovieDirector;
			mDirector           = dir;
			dir->setup(mDisplay, mGamePads[0]);
		} break;
		}

		if (!iVar9)
			nextState = gameLoop();

		if (mDirector != nullptr)
			mDirector->~TDirector();
		mDirector = nullptr;

		switch (mAppState) {
		case APP_STATE_BOOT:
			if (!mGamePads[0]->isSomethingPushed())
				initialize_bootAfter();
			break;

		case APP_STATE_NLOGO:
			if (!mGamePads[0]->isSomethingPushed())
				initialize_nlogoAfter();
			break;

		default:
			mHeap->freeAll();
			break;
		}

#ifdef SMS_PORT
		// Debug warp (SMS_PORT_WARP=<area>,<scenario>, off by default): after the
		// Nintendo logo, skip the intro movie, title and file select and start that
		// stage on a fresh save, the way picking a new file does (firstStart, save
		// slot 0, next area, then APP_STATE_GAMEPLAY).
		if (mAppState == APP_STATE_NLOGO && nextState == APP_STATE_DONE) {
			u8 warpArea, warpScenario;
			if (port_warp_target(&warpArea, &warpScenario)) {
				OSReport("[port] SMS_PORT_WARP: area %d scenario %d\n", warpArea,
				         warpScenario);
				TFlagManager::getInstance()->firstStart();
				portWarpFlags(); // SMS_PORT_WARP_FLAGS (level sweep C)
				mSaveFile = 0;
				mNextArea.set(warpArea, warpScenario, 0);
				nextState = APP_STATE_GAMEPLAY;
			}
#ifdef SMS_PORT_AURORA
			// w10: SMS_PORT_MP=host|join:<ip>:<port> (debug): host or join a multiplayer
			// session and start free mode on its in-memory save.
			else if (port_mp_debug_autostart(&warpArea, &warpScenario)) {
				mNextArea.set(warpArea, warpScenario, 0);
				nextState = APP_STATE_GAMEPLAY;
			}
			// w9 quality of life (port_qol.cpp, off by default): no intro movie
			// (APP_STATE_DONE plays Entrance.thp, movie 9), straight to the title
			// screen, the stage it leads to (area 15).
			else if (port_qol_skip_intro()) {
				mNextArea.set(15, 0, 0);
				nextState = APP_STATE_GAMEPLAY;
			}
#endif
		}
#endif

#ifdef SMS_PORT_AURORA
		const bool portMpReset = mGamePads[0]->isSomethingPushed(); // w10, see below
#endif
		SMSRumbleMgr->reset();
		if (mGamePads[0]->isSomethingPushed()) {
			TMarioGamePad::mResetFlag = 0;
			JUTGamePad::recalibrate(0xf0000000);
			if (!DVDCheckDisk()) {
				nextState = APP_STATE_QUIT;
				unk44 |= 2;
			} else if (mAppState == APP_STATE_BOOT
			           || mAppState == APP_STATE_NLOGO) {
				nextState = APP_STATE_QUIT;
			} else if (nextState != APP_STATE_QUIT) {
				nextState = APP_STATE_DONE;
				gpCardManager->unmount();
			}
		}

#ifdef SMS_PORT_AURORA
		// w10 multiplayer (port_mp_game.cpp): a session never lands on the title screen. The
		// ending, a game over or anything else heading for the title/file select sends the player
		// back to the session's Delfino Plaza; a reset leaves the lobby first. Inert outside one.
		{
			u8 portStage = mNextArea.getStage(), portScenario = mNextArea.getScenario();
			int portNext = nextState;
			if (port_mp_redirect_title(mAppState, portMpReset, &portNext, &portStage, &portScenario)) {
				nextState = (u8)portNext;
				mNextArea.set(portStage, portScenario, 0);
			}
		}
#endif

		mAppState = nextState;
		mPrevArea = mCurrArea;
		mCurrArea = mNextArea;
	}
}

#ifdef SMS_PORT
// PC frame-rate option (port/src/port_interp.cpp, w9): one extra display frame between two
// logic frames: the stage's draw lists again (MarDirectorDirect.cpp) and the screen fader as
// gameLoop() draws it (without TSMSFader::update, which advances the fade). Gameplay only.
extern "C" int port_mardirector_redraw(void);
extern "C" int port_app_redraw(void)
{
	TApplication* app = &gpApplication;
	if (gpMarDirector == nullptr || app->mDirector != (JDrama::TDirector*)gpMarDirector)
		return 0;
	if (!port_mardirector_redraw())
		return 0;
	const GXRenderModeObj& rmode = app->mDisplay->getVideo()->mNextRenderMode;
	GXSetViewport(0.0f, 0.0f, rmode.fbWidth, rmode.efbHeight, 0.0f, 1.0f);
	GXSetScissor(0, 0, rmode.fbWidth, rmode.efbHeight);
	Mtx44 ortho;
	C_MTXOrtho(ortho, 0.0f, (f32)rmode.efbHeight, 0.0f, (f32)rmode.fbWidth, -1.0f, 1.0f);
	GXSetProjection(ortho, GX_ORTHOGRAPHIC);
	app->mFader->draw(JDrama::TRect(0, 0, rmode.fbWidth, rmode.efbHeight));
	return 1;
}
#endif

int TApplication::gameLoop()
{
	u32 nextState = APP_STATE_DEFAULT;
	while (nextState <= APP_STATE_DEFAULT) {
		mDisplay->startRendering();

		TTimeRec::startFrameSt(mDisplay->unk60->mLastRetraceTime);
		TTimeRec::snapGXTimeSt(0);

		TMarioGamePad::read();
		for (int i = 0; i < 4; i++) {
			mGamePads[i]->updateMeaning();
			mGamePads[i]->onFlag(TMarioGamePad::PAD_FLAG_0x40);
		}

		if (int dvderr = drawDVDErr()) {
			SMSRumbleMgr->reset();
			if (dvderr != 'em_1') {
				if (mGamePads[0]->isSomethingPushed()) {
					nextState = APP_STATE_QUIT;
					unk44 |= 2;
				}
			}
		} else {
			if (mAppState == APP_STATE_BOOT) {
				nextState = APP_STATE_WAIT;
				if (OSIsThreadTerminated(&gSetupThread)) {
					void* val;
					OSJoinThread(&gSetupThread, &val);
					nextState = APP_STATE_NLOGO;
				}
			} else if (mAppState == APP_STATE_NLOGO) {
				nextState = APP_STATE_WAIT;
				if (!(sGameInit & 1)) {
					if (mDirector->direct() == APP_STATE_DONE)
						sGameInit |= 1;
				}
				if (!(sGameInit & 2) && OSIsThreadTerminated(&gSetupThread)) {
					void* val;
					OSJoinThread(&gSetupThread, &val);
					sGameInit |= 2;
				}

				if (sGameInit == 3)
					nextState = APP_STATE_DONE;
			} else {
#ifdef SMS_PORT
				{
					PORT_PROF_SCOPE(PP_DIRECT); // w14
					nextState = mDirector->direct();
				}
				// w14: a TMarDirector reports ready itself after setupObjects
				if (mDirector != (JDrama::TDirector*)gpMarDirector)
					port_prof_stage_ready();
#else
				nextState = mDirector->direct();
#endif
#ifdef SMS_PORT
				portTraceGameplay(this);
				if (gpMarDirector != nullptr
				    && mDirector == (JDrama::TDirector*)gpMarDirector) {
					portAutopilot();
					portDebugCall(); // SMS_PORT_DEBUG_CALL (level sweep C)
#ifdef SMS_PORT_AURORA
					port_mp_game_frame(); // w10: multiplayer leave/pause-menu glue
#endif
				}
#endif
			}

			JDrama::TGraphics graphics;
			graphics.unkFE = 0;

			const GXRenderModeObj& rmode
			    = mDisplay->getVideo()->mNextRenderMode;
			GXSetViewport(0.0f, 0.0f, rmode.fbWidth, rmode.efbHeight, 0.0f,
			              1.0f);
			GXSetScissor(0, 0, rmode.fbWidth, rmode.efbHeight);
			Mtx44 afStack_1ac;
			C_MTXOrtho(afStack_1ac, 0.0f, (f32)rmode.efbHeight, 0.0f,
			           (f32)rmode.fbWidth, -1.0f, 1.0f);
			GXSetProjection(afStack_1ac, GX_ORTHOGRAPHIC);
			mFader->update();
			mFader->draw(JDrama::TRect(0, 0, rmode.fbWidth, rmode.efbHeight));
			if (gpMSound != nullptr)
				gpMSound->mainLoop();
		}

		TTimeRec::snapCPUTime(0);

		THPPlayerDrawDone();
		mDisplay->endRendering();

		if (TTimeRec::_instance)
			TTimeRec::_instance->flip();

		JDrama::TVideo* video = mDisplay->unk60;
		if (video->mCurFrameBuffer) {
			JUTDirectPrint::getManager()->changeFrameBuffer(
			    (void*)video->mCurFrameBuffer, video->mCurRenderMode.fbWidth,
			    video->mCurRenderMode.xfbHeight);
			JUTAssertion::flushMessage();
		}
	}

	return nextState;
}

#if defined(VERSION_GMSP01) && !defined(VERSION_GMSE01)
static const char* sDvdErrMsgs[][6] = {
	{
	    "An error has occurred. Turn the\npower OFF and check the \n"
	    "NINTENDO GAMECUBE\x99"
	    "\nInstruction Booklet for further instructions.",
	    "The Disc could not be read.\nPlease read the NINTENDO GAMECUBE\x99"
	    "\nInstruction Booklet for\nmore information.",
	    "Reading Disc... ",
	    "The Disc Cover is open.\nTo continue playing,\nplease close the\nDisc "
	    "Cover.",
	    "Please insert a Super Mario Sunshine\nGame Disc.",
	    "This Disc is not\nSuper Mario Sunshine.\nPlease insert a Super\n"
	    "Mario Sunshine Game Disc.",
	},
	{
	    "Ein Fehler ist aufgetreten. Bitte schalte\nden NINTENDO GAMECUBE\x99"
	    " aus und\nlies die Bedienungsanleitung,\num weitere Informationen zu "
	    "erhalten.",
	    "Diese Disc kann nicht gelesen werden.\nBitte lies die "
	    "Bedienungsanleitung\n"
	    "des NINTENDO GAMECUBE\x99"
	    ",\num weitere Informationen zu erhalten.",
	    "Disc wird gelesen...",
	    "Disc-Deckel ist ge\xF6"
	    "ffnet. Bitte\nschlie\xDF"
	    "e den Disc-Deckel,\n"
	    "um weiterzuspielen.",
	    "Bitte lege eine Super Mario Sunshine\nGame Disc ein.",
	    "Diese Disc beinhaltet nicht Super Mario Sunshine.\n"
	    "Bitte lege eine Super Mario Sunshine\nGame Disc ein.",
	},
	{
	    "Une erreur est survenue. Eteignez\nla console et r\xE9"
	    "f\xE9"
	    "rez-vous au manuel\n"
	    "d'instructions NINTENDO GAMECUBE\x99"
	    "\npour de plus amples informations.",
	    "La lecture du disque a \xE9"
	    "chou\xE9"
	    ".\nVeuillez-vous r\xE9"
	    "f\xE9"
	    "rer au manuel\nd'instructions NINTENDO GAMECUBE\x99"
	    "\npour de plus amples informations.",
	    "Lecture du disque...",
	    "Le couvercle est ouvert.\nPour continuer \xE0"
	    " jouer,\n"
	    "veuillez fermer le couvercle.",
	    "Veuillez ins\xE9"
	    "rer\nle disque\nSuper Mario Sunshine.",
	    "Ce disque n'est pas\nle bon. Veuillez\nins\xE9"
	    "rer le disque\n"
	    "Super Mario Sunshine.",
	},
	{
	    "Se ha producido un error. Apaga la consola\ny consulta el manual de "
	    "instrucciones\n"
	    "de NINTENDO GAMECUBE\x99"
	    " para\nobtener m\xE1"
	    "s informaci\xF3"
	    "n.",
	    "No se puede leer el disco. Consulta\nel manual de instrucciones de \n"
	    "NINTENDO GAMECUBE\x99"
	    "\npara obtener m\xE1"
	    "s informaci\xF3"
	    "n.",
	    "Leyendo el disco...",
	    "La tapa est\xE1"
	    " abierta.\nCi\xE9"
	    "rrala para seguir jugando.",
	    "Coloca el disco de Super Mario Sunshine.",
	    "\xC9"
	    "ste no es el disco de\nSuper Mario Sunshine.\n"
	    "Coloca el disco apropiado.",
	},
	{
	    "Si \xE8"
	    " verificato un errore. Spegni (OFF) e\n"
	    "controlla il manuale d'istruzioni del\nNINTENDO GAMECUBE\x99"
	    "\nper ulteriori indicazioni.",
	    "Impossibile leggere il disco. Consulta il manuale \n"
	    "d'istruzioni del NINTENDO GAMECUBE\x99"
	    "\nper ulteriori indicazioni.",
	    "Lettura del disco in corso...",
	    "Il coperchio del disco \xE8"
	    "\naperto. Se vuoi proseguire\n"
	    "il gioco, chiudi il\ncoperchio del disco.",
	    "Inserisci il disco\nSuper Mario Sunshine.",
	    "Questo disco non \xE8"
	    " \nSuper Mario Sunshine.\n"
	    "Inserisci il disco \nSuper Mario Sunshine.",
	},
};
#endif

int TApplication::drawDVDErr()
{
	char message[512];
	u32 error = 0;
#if defined(VERSION_GMSP01) && !defined(VERSION_GMSE01)
	u32 language = TFlagManager::getInstance()->getFlag(MSF_LANGUAGE);
#endif

	switch (DVDGetDriveStatus()) {
	case -1:
#if defined(VERSION_GMSP01) && !defined(VERSION_GMSE01)
		snprintf(message, 512, sDvdErrMsgs[language][0]);
#elif defined(VERSION_GMSE01)
		snprintf(message, 512,
		         "An error has occurred. Turn the\npower OFF and check the\n"
		         "NINTENDO GAMECUBE\x99"
		         "\nInstruction Booklet for further instructions.");
#else
		snprintf(message, 512,
		         "エラーが発生しました。\n"
		         "本体のパワーボタンを押して電源をOFFにし\n"
		         "本体の取扱説明書の指示に従ってください。");
#endif
		error = 'em_1';
		break;

	case 11:
#if defined(VERSION_GMSP01) && !defined(VERSION_GMSE01)
		snprintf(message, 512, sDvdErrMsgs[language][1]);
#elif defined(VERSION_GMSE01)
		snprintf(message, 512,
		         "The Disc could not be read.\nPlease read the NINTENDO "
		         "GAMECUBE\x99"
		         "\nInstruction Booklet for\nmore information.\n");
#else
		snprintf(message, 512,
		         "ディスクを読めませんでした。\n"
		         "くわしくは、本体の取扱説明書を\n"
		         "お読みください。");
#endif
		error = 'em_2';
		break;

	case 1:
		if (DVDCheckDisk() == 0) {
#if defined(VERSION_GMSP01) && !defined(VERSION_GMSE01)
			snprintf(message, 512, sDvdErrMsgs[language][2]);
#elif defined(VERSION_GMSE01)
			snprintf(message, 512, "Reading Disc...");
#else
			snprintf(message, 512, "ディスクを読み込んでいます。");
#endif
			error = 'em_3';
		}
		break;

	case 5:
#if defined(VERSION_GMSP01) && !defined(VERSION_GMSE01)
		snprintf(message, 512, sDvdErrMsgs[language][3]);
#elif defined(VERSION_GMSE01)
		snprintf(message, 512,
		         "The Disc Cover is open.\nTo continue playing,\nplease close "
		         "the\nDisc Cover.");
#else
		snprintf(message, 512,
		         "ディスクカバーが開いています。\n"
		         "ゲームを続ける場合は\n"
		         "ディスクカバーを閉めてください。");
#endif
		error = 'em_4';
		break;

	case 4:
#if defined(VERSION_GMSP01) && !defined(VERSION_GMSE01)
		snprintf(message, 512, sDvdErrMsgs[language][4]);
#elif defined(VERSION_GMSE01)
		snprintf(message, 512,
		         "Please insert a Super Mario Sunshine\nGame Disc.");
#else
		snprintf(message, 512,
		         "「スーパーマリオサンシャイン」の\n"
		         "ディスクをセットしてください。");
#endif
		error = 'em_5';
		break;

	case 6:
#if defined(VERSION_GMSP01) && !defined(VERSION_GMSE01)
		snprintf(message, 512, sDvdErrMsgs[language][5]);
#elif defined(VERSION_GMSE01)
		snprintf(message, 512,
		         "This Disc is not\nSuper Mario Sunshine.\nPlease insert a "
		         "Super\nMario Sunshine Game Disc.");
#else
		snprintf(message, 512,
		         "このディスクは、「スーパーマリオサンシャイン」の\n"
		         "ディスクではありません。\n"
		         "「スーパーマリオサンシャイン」の\n"
		         "ディスクをセットしてください。 ");
#endif
		error = 'em_6';
		break;
	}

	if (error != 0) {
		ReInitializeGX();
		SMS_DrawInit();
		const GXRenderModeObj& rmode = mDisplay->getVideo()->mNextRenderMode;

		GXSetViewport(0.0f, 0.0f, rmode.fbWidth, rmode.efbHeight, 0.0f, 1.0f);
		Mtx44 afStack_260;
		C_MTXOrtho(afStack_260, 16.0f, 464.0f, 0.0f, 600.0f, -1.0f, 1.0f);
		GXSetProjection(afStack_260, GX_ORTHOGRAPHIC);
		MTXIdentity(afStack_260);
		GXLoadPosMtxImm(afStack_260, GX_PNMTX0);
		GXSetCullMode(GX_CULL_BACK);
		GXSetNumTexGens(1);
		GXSetNumTevStages(1);
		GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA,
		               GX_LO_NOOP);
		GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XY, GX_U16, 0);
		GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
		GXClearVtxDesc();
		GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
		GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
		GXSetNumChans(1);
		GXSetChanCtrl(GX_COLOR0A0, GX_TRUE, GX_SRC_REG, GX_SRC_VTX, 0,
		              GX_DF_NONE, GX_AF_NONE);
		GXSetChanAmbColor(GX_COLOR0A0, (GXColor) { 0xff, 0xff, 0xff, 0xff });
		GXSetChanCtrl(GX_COLOR1A1, GX_FALSE, GX_SRC_REG, GX_SRC_REG, 0,
		              GX_DF_NONE, GX_AF_NONE);
		GXBegin(GX_TRIANGLES, GX_VTXFMT0, 6);
		GXPosition2u16(0, 166);
		GXColor4u8(255, 0, 0, 200);
		GXPosition2u16(600, 166);
		GXColor4u8(255, 0, 0, 200);
		GXPosition2u16(600, 316);
		GXColor4u8(255, 0, 0, 200);
		GXPosition2u16(0, 166);
		GXColor4u8(255, 0, 0, 200);
		GXPosition2u16(600, 316);
		GXColor4u8(255, 0, 0, 200);
		GXPosition2u16(0, 316);
		GXColor4u8(255, 0, 0, 200);
		GXEnd();

		JUTFont* font = gpRomFont;
		if (gpSystemFont != nullptr)
			font = gpSystemFont;
		J2DPrint print(font, 0);
		print.unk44  = (GXColor) { 0xff, 0xff, 0, 0xff };
		print.unk48  = (GXColor) { 0xff, 0xff, 0, 0xff };
		f32 msgWidth = print.getWidth(message);
		print.print(0.5f * (600.0f - msgWidth), 230, message);
	}

	return error;
}

JKRMemArchive* TApplication::mountStageArchive()
{
	JKRMemArchive* result = nullptr;

	TNameRefPtrAryT<TNameRefAryT<TScenarioArchiveName> >& tmp = *unk30;
	if (mCurrArea.getStage() < tmp.getChildren().size()) {
		TNameRefAryT<TScenarioArchiveName>& scenarios
		    = tmp[mCurrArea.getStage()];
		if (mCurrArea.getScenario() < scenarios.size()) {
			const char* scenarioArcName
			    = scenarios.getChildren()[mCurrArea.getScenario()].mArcName;

#ifdef SMS_PORT_AURORA
			// port: the island test (port/src/port_isle.cpp) mounts its own archive
			if (void* isleBlob = port_isle_stage_archive(mCurrArea.getStage(), mCurrArea.getScenario())) {
				JKRMemArchive* arch = new JKRMemArchive;
				arch->mountFixed(isleBlob, MBF_0);
				return arch;
			}
#endif
			DVDChangeDir("/data/scene");
			if (void* archBlob
			    = SMSLoadArchive(scenarioArcName, nullptr, 0, nullptr)) {
				JKRMemArchive* arch = new JKRMemArchive;
				arch->mountFixed(archBlob, MBF_0);
				result = arch;
			}

			DVDChangeDir("/");
		}
	}

	return result;
}
