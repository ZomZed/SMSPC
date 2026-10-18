#include <Enemy/BossHanachan.hpp>
#include <Enemy/BossHanachanChangeSaveParams.hpp>
#include <Enemy/BossHanachanParts.hpp>
#include <Enemy/BossHanachanSub.hpp>
#include <Enemy/Conductor.hpp>
#include <Enemy/Graph.hpp>
#include <Camera/CameraShake.hpp>
#include <Camera/cameralib.hpp>
#include <GC2D/GCConsole2.hpp>
#include <JSystem/J3D/J3DGraphAnimator/J3DMaterialAttach.hpp>
#include <JSystem/J3D/J3DGraphAnimator/J3DModel.hpp>
#include <JSystem/J3D/J3DGraphBase/J3DTexture.hpp>
#include <JSystem/J3D/J3DGraphLoader/J3DModelLoaderFlags.hpp>
#include <JSystem/JDrama/JDRNameRefGen.hpp>
#include <JSystem/JUtility/JUTNameTab.hpp>
#include <M3DUtil/MActor.hpp>
#include <M3DUtil/SDLModel.hpp>
#include <Map/Map.hpp>
#include <Map/MapCollisionEntry.hpp>
#include <Map/MapData.hpp>
#include <MarioUtil/MapUtil.hpp>
#include <MarioUtil/MathUtil.hpp>
#include <MarioUtil/RandomUtil.hpp>
#include <MarioUtil/TexUtil.hpp>
#include <MarioUtil/RumbleMgr.hpp>
#include <MoveBG/ItemManager.hpp>
#include <MoveBG/MapObjManager.hpp>
#include <MSound/MSModBgm.hpp>
#include <MSound/MSound.hpp>
#include <MSound/SoundEffects.hpp>
#include <NPC/NpcInbetween.hpp>
#include <Player/MarioAccess.hpp>
#include <Strategic/Binder.hpp>
#include <Strategic/ObjManager.hpp>
#include <Strategic/ObjModel.hpp>
#include <Strategic/Spine.hpp>
#include <Strategic/Strategy.hpp>
#include <System/MarDirector.hpp>
#include <System/TargetArrow.hpp>
#include <macros.h>
#include <stdlib.h>

// rogue includes needed for matching sinit & bss
#include <MSound/MSSetSound.hpp>
#include <MSound/MSoundBGM.hpp>
#include <M3DUtil/InfectiousStrings.hpp>

const char* cSandPillarModelName = "sunabashira.bmd";
const char* cHitPoint1_RailName  = "bosshanachan2";
const char* cHitPoint2_RailName  = "bosshanachan1";
const char* cSandTextureName     = "suna";
const char* cDummyTextureName    = "M_dummy";

// Bit of a hit actor's filter that is set while its map collision is switched
// off.
// TODO: find out what this bit really means
static const u32 HIT_FILTER_COLLISION_REMOVED = 0x80000000;

TBossHanachanManager::TBossHanachanManager(const char* name)
    : TEnemyManager(name)
{
	static const char* sCommonSaveFileName
	    = "/enemy/bosshanachanCommon.prm";
	unk54 = new TBossHanachanCommonSaveParams(sCommonSaveFileName);

	static const char* sChangeSaveFileName[3] = {
		"/enemy/bosshanachan0.prm",
		"/enemy/bosshanachan1.prm",
		"/enemy/bosshanachan2.prm",
	};
	for (int i = 0; i < 3; ++i)
		unk58[i] = new TBossHanachanChangeSaveParams(sChangeSaveFileName[i]);
}

void TBossHanachanManager::createModelData()
{
	static TModelDataLoadEntry entry[] = {
		{ "hanabody_model.bmd", 0x10300000, 0 },
		{ "hanahead_model.bmd", 0x10100000, 0 },
		{ cSandPillarModelName, 0x10010000, 0 },
		{ nullptr, 0, 0 },
	};
	createModelDataArray(entry);
}

void TBossHanachanManager::loadAfter()
{
	J3DMaterialTable* table = gpMapObjManager->unkC0;
	ResTIMG* sandTex        = table->getTexture()->getResTIMG(
        table->getTextureName()->getIndex(cSandTextureName));
	for (int i = 0; i < 2; ++i)
		SMS_ChangeTextureAll(
		    getModelDataKeeper()->getNthData(i)->getModelData(),
		    cDummyTextureName, *sandTex);
}

void TBossHanachanManager::clipEnemies(JDrama::TGraphics* graphics)
{
	clipActorsAux(graphics, unk54->mSLViewClipFar.get(),
	              unk54->mSLViewClipRadius.get());
}

BOOL TBossHanachanManager::hasMapCollision() const { return TRUE; }

TBossHanachan::TBossHanachan(const char* name)
    : TSpineEnemy(name)
    , unk174(0)
    , unk178(nullptr)
    , unk17C(0.0f, 0.0f, 0.0f)
    , unk188(0.0f, 0.0f, 0.0f)
    , unk194(0.0f)
    , unk198(0.0f)
    , unk19C(nullptr)
    , unk1A0(0.0f, 0.0f, 0.0f)
    , unk1AC(0.0f, 0.0f, 0.0f)
    , unk1B8(-1)
    , unk1BC(nullptr)
    , unk1C0(nullptr)
{
	setRandomWeakBodyIndex();
}

void TBossHanachan::setRandomWeakBodyIndex()
{
	unk174 = 8.0f * MsRandF();
}

void TBossHanachan::init(TLiveManager* manager)
{
	mManager = manager;
	manager->manageActor(this);
	mMActorKeeper = new TMActorKeeper(manager, 10);
	unk19C        = mMActorKeeper->createMActor(cSandPillarModelName, 0);

	TBossHanachanManager* hanaManager = (TBossHanachanManager*)manager;
	unk1BC                            = hanaManager->unk54;
	unk1C0                            = hanaManager->unk58[0];

	mBodyScale        = 1.0f;
	mBodyRadius       = 350.0f;
	mWallRadius       = mBodyRadius;
	mHeadHeight       = 500.0f;
	mMarchSpeed       = 0.0f;
	mGravity          = 2.0f;
	mHitPoints        = 3;
	mScaledBodyRadius = 0.0f;
	onLiveFlag(LIVE_FLAG_UNK8 | LIVE_FLAG_UNK1000);

	mSpine->initWith(&TNerveBossHanachanGraphWander::theNerve());
	unk124->reset();
	goToShortestNextGraphNode();

	initHitActor(0x08000014, 0, 0, 0.0f, 0.0f, 0.0f, 0.0f);
	onHitFilter(HIT_FILTER_NO_COLLISION);

	for (int i = 0; i < ARRAY_COUNT(unk150); ++i) {
		unk150[i]         = new TBossHanachanPartsBody(this, "ボスハナチャンの体");
		unk150[i]->unk114 = i;
	}
	unk170 = new TBossHanachanPartsHead(this, "ボスハナチャンの頭");
	mMActor = unk170->getMActor();

	unk17C = mPosition;
	JGeometry::TVec3<f32> headPos;
	headPos = unk17C;
	const TParamT<f32>& headLen = unk1BC->mSLHeadLength;
	f32 sinY = MsSin(mRotation.y);
	f32 cosY = MsCos(mRotation.y);
	headPos.x = headPos.x - sinY * headLen.get();
	headPos.z = headPos.z - cosY * headLen.get();
	unk178 = new TSphereLink(8, JGeometry::TVec3<f32>(headPos.x, headPos.y, headPos.z), unk1BC->mSLBodyLength.get(),
	                         unk1BC->mSLBodyAttackRadius.get(), 0.2f, -2.0f,
	                         -3.5f, mRotation.y);

	unk170->mPosition    = mPosition;
	unk170->mRotation    = mRotation;
	unk170->mGroundPlane = mGroundPlane;
	for (int i = 0; i < ARRAY_COUNT(unk150); ++i) {
		TBossHanachanPartsBody* body = unk150[i];
		body->mPosition              = unk178->getPoint(i).unkC;
		body->unk124                 = body->mPosition;
		body->unk130                 = body->unk124;
		body->mRotation              = mRotation;
	}

	setHeadAndBodyAnm(BOSS_HANACHAN_ANM_UNK0, BOSS_HANACHAN_STOP_MOTION_BLEND_OFF);
	execHeadCalcAnim_();
	execBodyCalcAnim_();

	TIdxGroupObj* group = static_cast<TIdxGroupObj*>(
	    JDrama::TNameRefGen::search("敵グループ"));
	unk170->initMapCollisionAndHitActor_(group);
	for (int i = 0; i < ARRAY_COUNT(unk150); ++i) {
		unk150[i]->initMapCollisionAndHitActor_(group);
		unk150[i]->initFootHitActor_(group);
	}
}

void TBossHanachan::throwMario_(THitActor* hit)
{
	JGeometry::TVec3<f32> dir = *gpMarioPos - hit->mPosition;

	f32 power;
	if (dir.isZero()) {
		dir.set(0.0f, 1.0f, 0.0f);
		power = mMarchSpeed * unk1C0->mSLThrowTotalPower.get();
	} else {
		s16 moveAngle = CLBDegToShortAngle(MsGetRotFromZaxisY(unk188));
		s16 diff = moveAngle - CLBDegToShortAngle(MsGetRotFromZaxisY(dir));

		int absDiff = diff >= 0 ? diff : -diff;
		f32 t       = (1.0f / 32768.0f) * absDiff;
		f32 ratio   = 1.0f - t;
		power       = ratio * (mMarchSpeed * unk1C0->mSLThrowTotalPower.get());
		JGeometry::TVec3<f32> moveDir = unk188;
		moveDir.scale(ratio * unk1C0->mSLThrowMoveDirPower.get());
		dir += moveDir;
		dir.y = unk1C0->mSLThrowVecY.get();
	}

	f32 speed;
	speed = MsClamp(power, unk1C0->mSLThrowSpeedMin.get(),
	                unk1C0->mSLThrowSpeedMax.get());
	SMS_SendMessageToMario(unk170, HIT_MESSAGE_ATTACK);
	SMS_SendMessageToMario(unk170, HIT_MESSAGE_THROWN);
	SMS_ThrowMario(dir, speed);
	unk170->unk100->onWaterHitCounter();
}

// fabricated: rotates the XZ vector (vx, vz) around Y by `angle`
static inline void BHRotateXZ(f32 vx, f32 vz, s16 angle, f32* outX, f32* outZ)
{
	f32 c = JMASCos(angle);
	f32 s = JMASSin(angle);
	*outX = vx * c + vz * s;
	*outZ = -vx * s + vz * c;
}

static void CalcRevisionPosByRotateZ(const JGeometry::TVec3<f32>& rotation,
                                     f32 plus_y, Vec* pos)
{
	f32 absRotZ = fabsf(rotation.z);
	pos->y += plus_y * absRotZ;
	if (absRotZ > 90.0f) {
		f32 shift = 7.0f * (absRotZ - 90.0f);
		if (rotation.z > 0.0f)
			shift = -shift;
		s16 angle = CLBDegToShortAngle(rotation.y);
		f32 dx, dz;
		BHRotateXZ(shift, 0.0f, angle, &dx, &dz);
		pos->x += dx;
		pos->z += dz;
	}
}

void TBossHanachan::execHeadCalcAnim_()
{
	JGeometry::TVec3<f32> pos = mPosition;
	CalcRevisionPosByRotateZ(mRotation, unk1BC->mSLHeadPlusYByRotateZ.get(),
	                         &pos);
	CLBCalcRotateZXYTranslateMatrix(
	    unk170->getMActor()->getModel()->getBaseTRMtx(), mRotation, pos);
	unk170->getMActor()->calc();
}

void TBossHanachan::execBodyCalcAnim_()
{
	for (int i = 0; i < ARRAY_COUNT(unk150); ++i) {
		TBossHanachanPartsBody* body = unk150[i];
		JGeometry::TVec3<f32> pos = body->mPosition;
		CalcRevisionPosByRotateZ(body->mRotation,
		                         unk1BC->mSLBodyPlusYByRotateZ.get(), &pos);
		Mtx mtx;
		CLBCalcRotateZXYTranslateMatrix(mtx, body->mRotation, pos);
		MTXCopy(mtx, body->getMActor()->getModel()->getBaseTRMtx());
		body->getMActor()->calc();
	}
}

void TBossHanachan::kill() { }

void TBossHanachan::bind()
{
	if (checkLiveFlag(LIVE_FLAG_UNK10))
		return;

	if (mBinder != nullptr) {
		mBinder->bind(this);
		return;
	}

	JGeometry::TVec3<f32> nextPos = mPosition;
	nextPos += mPositionDelta;
	nextPos += mVelocity;

	mVelocity.y -= getGravityY();
	if (mVelocity.y < mVelocityMinY)
		mVelocity.y = mVelocityMinY;

	unk17C.x = nextPos.x;
	unk17C.y = nextPos.y;
	unk17C.z = nextPos.z;
	f32 shiftX, shiftZ;
	f32 headLen = unk178->unk14;
	BHSCalcRevisionDistXZByRotateZ(mRotation.y, mRotation.z, headLen,
	                               &shiftX, &shiftZ);
	unk17C.x += shiftX;
	unk17C.z += shiftZ;

	JGeometry::TVec3<f32> bindPos = unk17C;

	mGroundHeight = gpMap->checkGroundIgnoreWaterSurface(
	    unk17C.x, unk17C.y + mHeadHeight, unk17C.z, &mGroundPlane);
	mGroundHeight += 1.0f;

	if (unk17C.y <= mGroundHeight + 0.05f) {
		if (mGroundPlane != nullptr) {
			if (mGroundPlane->isLegal()) {
				offLiveFlag(LIVE_FLAG_AIRBORNE);
				mVelocity.set(0.0f, 0.0f, 0.0f);
				unk17C.y = mGroundHeight;
			}
		}
	} else {
		onLiveFlag(LIVE_FLAG_AIRBORNE);
	}

	gpMap->isTouchedOneWallAndMoveXZ(&unk17C.x, unk17C.y + mHeadHeight,
	                                 &unk17C.z, mBodyRadius);

	JGeometry::TVec3<f32> d2 = unk17C - bindPos;
	JGeometry::TVec3<f32> d1;
	d1 = nextPos - mPosition;
	// FAKEMATCH(frame): 12 bytes; real construct unknown
	volatile u32 pad[3];
	(void)pad;
	mPositionDelta = d1 + d2;
}

#ifdef SMS_PORT
#include <stdlib.h>
extern "C" u32 port_frame_count(void); // port_main.cpp
extern "C" float gPortBossAux[3];       // port_bosstrace.cpp (SMS_PORT_BOSSTRACE debug bots)
extern "C" unsigned gPortBossAuxFrame;
extern "C" int gPortBossWant;
extern "C" float gPortThrowAt[3];
extern "C" float gPortSand[48][5];        // MapObjMamma.cpp: sand sprout bases
extern "C" unsigned gPortSandFrame[48];
// SMS_PORT_BOSSTRACE (debug, w5): Wiggler for the PADSCRIPT AIM bot. While he lies on
// his back (Down), his weak body segment is the stomp target (want 2: jump, ground pound);
// while he walks, spray a sand sprout ahead of him so he walks over the mound and tips
// over (want 5); otherwise stay clear (want 3).
static void portWigglerBot(TBossHanachan* h)
{
	static const bool sOn = getenv("SMS_PORT_BOSSTRACE") != nullptr;
	if (!sOn)
		return;
	const void* n = h->mSpine->getCurrentNerve();
	if (n == &TNerveBossHanachanDown::theNerve() && h->unk174 >= 0 && h->unk174 < 8) {
		// The segment's joint matrix is where it lies (its mPosition lags hundreds of units).
		const JGeometry::TVec3<f32>& p = h->unk150[h->unk174]->mPosition;
		MtxPtr jm                      = h->unk150[h->unk174]->unk108;
		gPortBossAux[0]                = jm ? jm[0][3] : p.x;
		gPortBossAux[1]                = jm ? jm[1][3] : p.y;
		gPortBossAux[2]                = jm ? jm[2][3] : p.z;
		gPortBossAuxFrame              = port_frame_count();
		gPortBossWant                  = 2;
		if ((port_frame_count() % 60) == 0)
			OSReport("[boss] f%u wiggler down: weak segment %d at %.0f %.0f %.0f (joint %.0f %.0f %.0f, anm %d), mario "
			         "%.0f %.0f %.0f\n",
			         port_frame_count(), h->unk174, p.x, p.y, p.z,
			         h->unk150[h->unk174]->unk108 ? h->unk150[h->unk174]->unk108[0][3] : 0.0f,
			         h->unk150[h->unk174]->unk108 ? h->unk150[h->unk174]->unk108[1][3] : 0.0f,
			         h->unk150[h->unk174]->unk108 ? h->unk150[h->unk174]->unk108[2][3] : 0.0f,
			         h->unk150[h->unk174]->unkF4, SMS_GetMarioPos().x, SMS_GetMarioPos().y, SMS_GetMarioPos().z);
	} else if (n == &TNerveBossHanachanGraphWander::theNerve()) {
		// Keep the sand sprout nearest to him (within 4000) grown into a mound, so he walks
		// over it and tips over: stand 450 from it and spray it (want 5) until it is full
		// height; then wait beside it (want 6, same spot, no spray).
		// The choice is kept for 60 s (walking there and spraying takes a while); then
		// the sprout nearest to him again.
		static int sChosen = -1;
		static u32 sChosenAt;
		int best  = -1;
		f32 bestD = 4000.0f * 4000.0f;
		if (sChosen >= 0 && port_frame_count() - sChosenAt < 1800
		    && port_frame_count() - gPortSandFrame[sChosen] < 10) {
			best        = sChosen;
			const f32 rx = gPortSand[best][0] - h->mPosition.x, rz = gPortSand[best][2] - h->mPosition.z;
			bestD        = rx * rx + rz * rz;
		} else {
			for (int i = 0; i < 48; ++i) {
				if (port_frame_count() - gPortSandFrame[i] >= 10)
					continue;
				const f32 rx = gPortSand[i][0] - h->mPosition.x, rz = gPortSand[i][2] - h->mPosition.z;
				if (rx * rx + rz * rz < bestD) {
					bestD = rx * rx + rz * rz;
					best  = i;
				}
			}
			sChosen   = best;
			sChosenAt = port_frame_count();
		}
		static f32 sStandX, sStandZ;
		static int sStandFor = -1;
		if (best >= 0 && sStandFor != best) {
			// Stand 450 from the sprout on the side Mario comes from.
			f32 mx = SMS_GetMarioPos().x - gPortSand[best][0];
			f32 mz = SMS_GetMarioPos().z - gPortSand[best][2];
			const f32 ml = sqrtf(mx * mx + mz * mz) + 0.001f;
			sStandX      = gPortSand[best][0] + mx / ml * 450.0f;
			sStandZ      = gPortSand[best][2] + mz / ml * 450.0f;
			sStandFor    = best;
		}
		if ((port_frame_count() % 300) == 0)
			OSReport("[boss] f%u wiggler at %.0f %.0f heading %.0f: sprout %d (%.0f away, height %.2f state %.0f)\n",
			         port_frame_count(), h->mPosition.x, h->mPosition.z, h->mRotation.y, best,
			         best >= 0 ? sqrtf(bestD) : 0.0f, best >= 0 ? gPortSand[best][3] : 0.0f,
			         best >= 0 ? gPortSand[best][4] : 0.0f);
		if (best >= 0) {
			gPortBossAux[0]   = gPortSand[best][0];
			gPortBossAux[1]   = gPortSand[best][1];
			gPortBossAux[2]   = gPortSand[best][2];
			gPortThrowAt[0]   = sStandX;
			gPortThrowAt[1]   = gPortSand[best][1];
			gPortThrowAt[2]   = sStandZ;
			gPortBossAuxFrame = port_frame_count();
			gPortBossWant     = gPortSand[best][3] < 0.99f ? 5 : 6;
		} else {
			gPortBossWant = 3;
		}
	} else {
		gPortBossWant = 3;
	}
}
#endif

void TBossHanachan::moveObject()
{
#ifdef SMS_PORT
	portWigglerBot(this);
#endif
	updateSquareToMario();
	unk188.set(mPositionDelta);
	TLiveActor::moveObject();

	if (mSpine->getLatestNerve() != &TNerveBossHanachanGetUp::theNerve())
		CLBChaseDecrease(&mRotation.z, unk150[0]->mRotation.z, 0.04f, 0.0f);

	unk170->mPosition    = mPosition;
	unk170->mRotation    = mRotation;
	unk170->mGroundPlane = mGroundPlane;
}

// the target keeps MsWrap out of line in perform()
static inline f32 HanaWrapW1(f32 t, f32 l, f32 r) { return MsWrap(t, l, r); }

static inline void HanaSaveOldParams(TBossHanachanPartsBody* body)
{
	body->unk130 = body->unk124;
	body->unk124 = body->mPosition;
	body->unk140 = body->unk13C;
	body->unk13C = body->mRotation.z;
	body->unk148 = body->unk144;
}


static inline f32 HanaClamp179(f32 v)
{
	if (v > 179.0f)
		v = 179.0f;
	else if (v < -179.0f)
		v = -179.0f;
	return v;
}

// the target inlines a two-return bool test of the collision-removed filter
static inline bool HanaIsColRemoved(const THitActor* hit)
{
	return hit->checkHitFilter(HIT_FILTER_COLLISION_REMOVED) ? true : false;
}

// TODO (perform, 96.4%): the first body loop is indexed with lwzx in the
// target but fully unrolls here when written with unk150[i]; a few loads are
// scheduled differently around the sand-slope and wave code.
// fabricated: body of getBodyMaxRotateZ()
static inline f32 BodyMaxRotateZInline(const TBossHanachan* boss)
{
	f32 max = 0.0f;
	for (int i = 0; i < ARRAY_COUNT(boss->unk150); ++i) {
		if (fabsf(boss->unk150[i]->mRotation.z) > fabsf(max))
			max = boss->unk150[i]->mRotation.z;
	}
	return max;
}

void TBossHanachan::perform(u32 cue, JDrama::TGraphics* graphics)
{
	if (checkLiveFlag(LIVE_FLAG_DEAD | LIVE_FLAG_UNK200))
		return;

	if (checkLiveFlag(LIVE_FLAG_UNK40000)) {
		if ((cue & CUE_MOVE) && (graphics->unk0 & 2)) {
			SMSGetMSound()->startSoundActor(MSD_SE_WT_BOSS_FADEAWAY, &mPosition,
			                                0, nullptr, 0, 4);
			if (!gpMarDirector->isThing()
			    && (checkLiveFlag(LIVE_FLAG_UNK100000)
			        || SMSGetMSound()->unk98->modBgm(1, 1) == nullptr)) {
				onLiveFlag(LIVE_FLAG_DEAD | LIVE_FLAG_UNK40);
				gpItemManager->makeShineAppearWithDemo(
				    "シャイン（ボス用）", "ボスシャインカメラ", unk17C.x,
				    unk17C.y + unk1BC->mSLShineAppearOffsetY.get(), unk17C.z);
			}
		}
		return;
	}

	if (cue & CUE_MOVE) {
		if (gpMarDirector->isThing()) {
			mPositionDelta.zero();
			mRotationDelta.zero();
			if (graphics->unk0 & 2) {
				if (mSpine->getLatestNerve()
				        == &TNerveBossHanachanDead::theNerve()
				    && !checkLiveFlag(LIVE_FLAG_UNK100000)) {
					onLiveFlag(LIVE_FLAG_UNK100000);
					MSBgm::stopTrackBGM(1, 30);
				}
			}
		} else {
			if (!checkLiveFlag(LIVE_FLAG_UNK80000) && mHitPoints == 3
			    && mMarchSpeed != 0.0f) {
				if (mSpine->getLatestNerve()
				    != &TNerveBossHanachanTumble::theNerve()) {
					if (unk1B8 == -1
					    && gpMarDirector->mState == TMarDirector::STATE_UNK4) {
						unk1B8 = 7200;
					} else if (unk1B8 > 0) {
						unk1B8 -= 1;
						if (unk1B8 == 0) {
							unk1B8 = 7200;
							gpMarDirector->getConsole()->startAppearBalloon(6,
							                                                true);
						}
					}
				}
			}

			moveObject();

			const TNerveBase<TLiveActor>* nerve = mSpine->getLatestNerve();

			TBossHanachanPartsBody** bodyIt = unk150;
			for (int i = 0; i < ARRAY_COUNT(unk150); ++i, ++bodyIt)
				HanaSaveOldParams(*bodyIt);

			s16 headAngle = CLBDegToShortAngle(unk150[0]->mRotation.y);
			CLBChaseAngleDecrease(&headAngle,
			                      CLBDegToShortAngle(mRotation.y), 20);
			unk150[0]->mRotation.y = (360.0f / 65536.0f) * headAngle;

			for (int i = 1; i < ARRAY_COUNT(unk150); ++i) {
				TBossHanachanPartsBody* body = unk150[i];
				JGeometry::TVec3<f32> link = unk178->getPoint(i - 1).unkC - unk178->getPoint(i).unkC;
				body->mRotation.y = MsAngleWrap(MsGetRotFromZaxisY(link));
			}

			unk178->unk18 = unk150[0]->mRotation.y;
			for (int i = 0; i < ARRAY_COUNT(unk150); ++i)
				unk178->setDegreeZAndRevisionPosXZ(i, unk150[i]->mRotation.z);

			JGeometry::TVec3<f32> headPos = mPosition;
			headPos.x = mPosition.x
			            - MsSin(mRotation.y) * unk1BC->mSLHeadLength.get();
			headPos.z = mPosition.z
			            - MsCos(mRotation.y) * unk1BC->mSLHeadLength.get();
			f32 revisionX, revisionZ;
			BHSCalcRevisionDistXZByRotateZ(mRotation.y, mRotation.z,
			                               unk178->unk14, &revisionX,
			                               &revisionZ);
			headPos.x += revisionX;
			headPos.z += revisionZ;
			unk178->moveHead(headPos);

			for (int i = 0; i < ARRAY_COUNT(unk150); ++i) {
				TBossHanachanPartsBody* body = unk150[i];
				BHSCalcRevisionDistXZByRotateZ(
				    body->mRotation.y, body->mRotation.z, unk178->unk14,
				    &revisionX, &revisionZ);
				body->mPosition = unk178->getPoint(i).unkC;
				body->mPosition.x -= revisionX;
				body->mPosition.z -= revisionZ;
			}

			bool isTumble = false;
			f32 centrifugal = 0.0f;
			if (mSpine->getLatestNerve()
			    == &TNerveBossHanachanTumble::theNerve()) {
				isTumble    = true;
				centrifugal = getBodyMaxRotateZ();
			}
			for (int i = 0; i < ARRAY_COUNT(unk150); ++i) {
				TBossHanachanPartsBody* body = unk150[i];
				if (!isTumble)
					centrifugal = BHSCalcCentrifugalForce(
					                  body->mPosition, body->unk124,
					                  body->unk130, body->mRotation.y)
					              * unk1C0->mSLCentrifugalForce.get();
				CLBChaseGeneralConstantSpecifySpeed(
				    &body->unk144, centrifugal,
				    unk1C0->mSLCentrifugalSpeed.get());
				f32 v = body->unk144;
				if (v > 179.0f)
					v = 179.0f;
				else if (v < -179.0f)
					v = -179.0f;
				body->unk144 = v;
			}

			if (nerve != &TNerveBossHanachanDown::theNerve()) {
				for (int i = 0; i < ARRAY_COUNT(unk150); ++i) {
					TBossHanachanPartsBody* body = unk150[i];
					f32 groundY = gpMap->checkGroundIgnoreWaterSurface(
					    body->mPosition.x, 500.0f + body->mPosition.y,
					    body->mPosition.z, &body->mGroundPlane);
					const TLiveActor* sand = body->getSandActor_();
					if (sand != nullptr) {
						JGeometry::TVec3<f32> toSand;
						JGeometry::TVec3<f32> sandXZ;
						f32 limit;
						f32 sz;
						f32 sx;
						toSand = sand->mPosition - mPosition;
						sandXZ = toSand;
						limit = CLBSquared(50.0f);
						sz = sandXZ.z;
						sx = sandXZ.x;
						if (sx * sx + sz * sz <= limit) {
							body->unk120 = 0.0f;
						} else {
							f32 wrappedSand = HanaWrapW1(
							    MsGetRotFromZaxisY(sandXZ), -180.0f, 180.0f);
							f32 relative = HanaWrapW1(
							    wrappedSand
							        - HanaWrapW1(mRotation.y, -180.0f, 180.0f),
							    -180.0f, 180.0f);
							f32 absRelative = relative >= 0.0f ? relative : -relative;
							if (absRelative <= 15.0f || absRelative >= 165.0f) {
								body->unk120 = 0.0f;
							} else {
								f32 rise = SMS_GetSandRiseUpRatio(sand);
								if (relative > 15.0f)
									body->unk120 = 70.0f * rise;
								else
									body->unk120 = -70.0f * rise;
							}
						}
					} else {
						JGeometry::TVec3<f32> off(200.0f, 0.0f, 0.0f);
						s16 angle = CLBDegToShortAngle(body->mRotation.y);
						f32 s = JMASSin(angle);
						f32 c = JMASCos(angle);
						JGeometry::TVec3<f32> rot(off.x * c + off.z * s, off.y,
						                          -off.x * s + off.z * c);
						JGeometry::TVec3<f32> front(rot.x + body->mPosition.x,
						                            rot.y + body->mPosition.y,
						                            rot.z + body->mPosition.z);
						JGeometry::TVec3<f32> back(-rot.x + body->mPosition.x,
						                           -rot.y + body->mPosition.y,
						                           -rot.z + body->mPosition.z);
						const TBGCheckData* tmpPlane;
						f32 groundA = gpMap->checkGroundIgnoreWaterSurface(
						    front.x, 500.0f + front.y, front.z, &tmpPlane);
						f32 groundB = gpMap->checkGroundIgnoreWaterSurface(
						    back.x, 500.0f + back.y, back.z, &tmpPlane);
						f32 diffA = groundA - groundY;
						f32 diffB = groundB - groundY;
						f32 absA = fabsf(diffA);
						f32 absB = fabsf(diffB);
						if (absA < 0.001f && absB < 0.001f) {
							body->unk120 = 0.0f;
						} else if (absA > absB) {
							body->unk120
							    = (360.0f / 65536.0f) * matan(200.0f, diffA);
						} else {
							body->unk120
							    = -((360.0f / 65536.0f) * matan(200.0f, diffB));
						}
					}
				}

				if (nerve == &TNerveBossHanachanGetUp::theNerve()) {
					unk170->calcRotateZWhenGetUp_();
					mRotation.z = unk170->mRotation.z;
					for (int i = 0; i < ARRAY_COUNT(unk150); ++i)
						unk150[i]->calcRotateZWhenGetUp_();
				} else {
					f32 maxRotateZ = getBodyMaxRotateZ();
					nerve          = mSpine->getLatestNerve();
					TBossHanachanChangeSaveParams* params = unk1C0;
					f32 lengthSq = unk1BC->mSLBodyLength.get();
					f32 decrease = params->mSLWaveDecrease.get() * (1.0f / 120.0f);
					f32 velocity = params->mSLWaveVelocity.get();
					f32 invLengthSq = 1.0f / (lengthSq * lengthSq);
					f32 invDenom = 1.0f / (1.0f + decrease);
					f32 keep = 1.0f - decrease;
					f32 coeff = (1.0f / 120.0f)
					            * ((1.0f / 120.0f) * (velocity * velocity));
					for (int i = 0; i < ARRAY_COUNT(unk150); ++i) {
						TBossHanachanPartsBody* body = unk150[i];
						bool fullyTumbled = true;
						f32 rz = body->mRotation.z;
						if (rz != -179.0f && rz != 179.0f)
							fullyTumbled = false;
						bool tumbledBool = fullyTumbled ? true : false;
						if (!tumbledBool || rz != maxRotateZ) {
							f32 prev = (i == 0) ? unk150[i + 1]->unk13C
							                    : unk150[i - 1]->unk13C;
							f32 next = (i == 7) ? unk150[i - 1]->unk13C
							                    : unk150[i + 1]->unk13C;
							f32 twice = 2.0f * body->unk13C;
							f32 wave = HanaClamp179(
							    ((twice * invDenom)
							     + (invDenom
							        * (coeff
							           * ((invLengthSq * ((next + prev) - twice))
							              + body->unk148))))
							    - (invDenom * (body->unk140 * keep)));
							f32* rotZ = &body->mRotation.z;
							CLBChaseGeneralConstantSpecifySpeed(
							    rotZ, wave, unk1C0->mSLRotateZLeanSpeed.get());
							bool onSand = false;
							if (nerve
							    == &TNerveBossHanachanGraphWander::theNerve()) {
								const TLiveActor* sand = body->getSandActor_();
								if (sand != nullptr) {
									f32 slope = body->unk120;
									onSand    = true;
									if (slope != 0.0f) {
										f32 speed = slope * mMarchSpeed
										            * unk1C0->mSLSandSlopeForce
										                  .get();
										f32 target = 179.0f;
										if (slope < 0.0f)
											target = -179.0f;
										CLBChaseGeneralConstantSpecifySpeed(
										    &body->mRotation.z, target, speed);
									}
								} else {
									CLBChaseGeneralConstantSpecifySpeed(
									    rotZ, body->unk120,
									    unk1C0->mSLRotateZRestorationSpeed
									        .get());
								}
							} else if (nerve
							           == &TNerveBossHanachanTumble::
							                  theNerve()) {
								CLBChaseGeneralConstantSpecifySpeed(
								    rotZ, unk194, unk198);
							}

							f32 clamped = body->mRotation.z;
							if (clamped > 179.0f)
								clamped = 179.0f;
							else if (clamped < -179.0f)
								clamped = -179.0f;
							body->mRotation.z = clamped;

							if (nerve
							        == &TNerveBossHanachanGraphWander::theNerve()
							    && (mSpine->getTime()
							            < unk1C0->mSLNotFallDownFrames.get()
							        || !onSand)) {
								f32 maxNotSand = unk1C0->mSLMaxRotateZNotSand.get();
								f32 curRz = body->mRotation.z;
								if (curRz < -maxNotSand)
									CLBChaseGeneralConstantSpecifySpeed(
									    &body->mRotation.z, -maxNotSand, 15.0f);
								else if (curRz > maxNotSand)
									CLBChaseGeneralConstantSpecifySpeed(
									    &body->mRotation.z, maxNotSand, 15.0f);
							}
						}
					}

					f32 maxDiff = unk1C0->mSLDiffMaxRotateZ.get();
					for (int i = 1; i < ARRAY_COUNT(unk150); ++i) {
						f32& prevRz = unk150[i - 1]->mRotation.z;
						f32& curRz  = unk150[i]->mRotation.z;
						if (fabsf(prevRz - curRz) > maxDiff) {
							if (curRz < prevRz)
								curRz = prevRz - maxDiff;
							else
								curRz = prevRz + maxDiff;
						}
					}
				}
			}

			bool thrown = false;
			{
				TWaterHitActor* hit = unk170->unk100;
				if (hit->mHitCounter >= 1)
					hit->mHitCounter -= 1;
			}
			{
				TWaterHitActor* hit = unk170->unk100;
				for (int i = 0; i < hit->mColCount; ++i) {
					if (hit->mCollisions[i]->getActorType()
					    == ACTOR_TYPE_MARIO) {
						throwMario_(hit);
						thrown = true;
						break;
					}
				}
			}

			for (int i = 0; i < ARRAY_COUNT(unk150); ++i) {
				TBossHanachanPartsBody* body = unk150[i];
				{
					TWaterHitActor* hit = body->unk100;
					if (hit->mHitCounter >= 1)
						hit->mHitCounter -= 1;
				}
				if (!thrown) {
					TWaterHitActor* bodyHit = body->unk100;
					for (int j = 0; j < bodyHit->mColCount; ++j) {
						if (bodyHit->mCollisions[j]->getActorType()
						    == ACTOR_TYPE_MARIO) {
							throwMario_(bodyHit);
							thrown = true;
							break;
						}
					}
				}
				for (int k = 0; k < 2; ++k) {
					TFootHitActor* footHit = body->unk118[k];
					if (footHit->mHitCounter >= 1)
						footHit->mHitCounter -= 1;
					if (!thrown) {
						for (int j = 0; j < footHit->mColCount; ++j) {
							if (footHit->mCollisions[j]->getActorType()
							    == ACTOR_TYPE_MARIO) {
								throwMario_(footHit);
								thrown = true;
								break;
							}
						}
					}
				}
			}

			if ((graphics->unk0 & 2)
			    && nerve == &TNerveBossHanachanDead::theNerve()) {
				SMSGetMSound()->startSoundActor(MSD_SE_WT_BOSS_FADEAWAY,
				                                &mPosition, 0, nullptr, 0, 4);
				if (!checkLiveFlag(LIVE_FLAG_UNK100000))
					SMSGetMSound()->unk98->modBgm(1, 1);
			}
		}

		unk170->moveMapCollision_();
		for (int i = 0; i < ARRAY_COUNT(unk150); ++i)
			unk150[i]->moveMapCollision_();
	}

	if (cue & CUE_CALC_ANIM) {
		gpTargetArrow->unk14 = 0;
		if (!gpMarDirector->isThing()) {
			bool walking = false;
			if (mSpine->getLatestNerve()
			        == &TNerveBossHanachanGraphWander::theNerve()
			    && mMarchSpeed > 0.001f)
				walking = true;

			MtxPtr headMtx = unk170->unk108;
			JGeometry::TVec3<f32> headPos(headMtx[0][3], headMtx[1][3],
			                              headMtx[2][3]);
			unk170->unk100->mPosition.set(
			    headPos.x, headPos.y - unk1BC->mSLHeadHitOffsetY.get(),
			    headPos.z);
			if (walking) {
				if (!HanaIsColRemoved(unk170->unk100)) {
					unk170->unk100->onHitFilter(HIT_FILTER_COLLISION_REMOVED);
					unk170->unk104->remove();
				}
			} else {
				if (HanaIsColRemoved(unk170->unk100)) {
					unk170->unk100->offHitFilter(HIT_FILTER_COLLISION_REMOVED);
					unk170->unk104->setUpTrans(headPos);
				}
			}

			f32 footOffsetY = unk1BC->mSLFootHitOffsetY.get();
			f32 bodyOffsetY = unk1BC->mSLBodyHitOffsetY.get();
			for (int i = 0; i < ARRAY_COUNT(unk150); ++i) {
				TBossHanachanPartsBody* body = unk150[i];
				MtxPtr bodyMtx = body->unk108;
				THitActor* bodyHit = body->unk100;
				body->unk154.set(bodyMtx[0][3], bodyMtx[1][3], bodyMtx[2][3]);
				bodyHit->mPosition.set(body->unk154.x,
				                       body->unk154.y - bodyOffsetY,
				                       body->unk154.z);
				if (walking) {
					if (!HanaIsColRemoved(bodyHit)) {
						bodyHit->onHitFilter(HIT_FILTER_COLLISION_REMOVED);
						body->unk104->remove();
					}
				} else {
					if (HanaIsColRemoved(bodyHit)) {
						bodyHit->offHitFilter(HIT_FILTER_COLLISION_REMOVED);
						body->unk104->setUpTrans(body->unk154);
					}
				}

				for (int k = 0; k < 2; ++k) {
					TFootHitActor* foot = body->unk118[k];
					MtxPtr footMtx = foot->unk6C;
					foot->mPosition.set(footMtx[0][3],
					                    footMtx[1][3] - footOffsetY,
					                    footMtx[2][3]);
					if (walking)
						foot->onHitFilter(HIT_FILTER_COLLISION_REMOVED);
					else
						foot->offHitFilter(HIT_FILTER_COLLISION_REMOVED);
				}
			}
			emitParticle_();
			emitCamShake_();
		}

		if (checkLiveFlag(LIVE_FLAG_UNK10000)
		    && unk19C->curAnmEndsNext(0, nullptr))
			offLiveFlag(LIVE_FLAG_UNK10000);

		if (!gpMarDirector->isThing())
			changeAnmRateAndFrameUpdate_();

		unk170->unk110->execMotionBlend(unk170->getMActor());
		for (int i = 0; i < ARRAY_COUNT(unk150); ++i)
			unk150[i]->unk110->execMotionBlend(unk150[i]->getMActor());
	}

	if (cue & CUE_CALC_ANIM) {
		execHeadCalcAnim_();
		execBodyCalcAnim_();

		if (!gpMarDirector->isThing()) {
			if (mSpine->getLatestNerve() == &TNerveBossHanachanTumble::theNerve()
			    || mSpine->getLatestNerve()
			           == &TNerveBossHanachanDown::theNerve()) {
				MtxPtr weakMtx = unk150[unk174]->unk108;
				JGeometry::TVec3<f32> arrowPos(weakMtx[0][3],
				                               400.0f + weakMtx[1][3],
				                               weakMtx[2][3]);
				gpTargetArrow->setPos(arrowPos);
				gpTargetArrow->unk14 = 1;
			}
		}
	}

	if (cue & CUE_ENTRY) {
		unk170->entryCircleShadow_();
		unk170->setDamageFog_(graphics);
		unk170->drawObject(graphics);
		for (int i = 0; i < ARRAY_COUNT(unk150); ++i) {
			TBossHanachanPartsBody* body = unk150[i];
			body->entryCircleShadow_();
			body->setDamageFog_(graphics);
			body->drawObject(graphics);
		}
	}

	if (cue & CUE_CALC_VIEW) {
		unk170->getMActor()->viewCalc();
		for (int i = 0; i < ARRAY_COUNT(unk150); ++i)
			unk150[i]->getMActor()->viewCalc();
	}

	if (checkLiveFlag(LIVE_FLAG_UNK10000))
		unk19C->perform(cue, graphics);
}

bool TBossHanachan::isTumbleCompletelyAllBody() const
{
	f32 first   = unk150[0]->mRotation.z;
	bool result = true;
	bool tumbled = true;
	if (first != -179.0f && first != 179.0f)
		tumbled = false;
	if (!(tumbled ? true : false)) {
		result = false;
	} else {
		for (int i = 1; i < ARRAY_COUNT(unk150); ++i) {
			if (unk150[i]->mRotation.z != first) {
				result = false;
				break;
			}
		}
	}
	return result;
}

static inline f32 HanaAbs(f32 v) { return v >= 0.0f ? v : -v; }

bool TBossHanachan::checkFallDecideAndSetup()
{
	for (int i = 0; i < ARRAY_COUNT(unk150); ++i) {
		TBossHanachanPartsBody* body = unk150[i];
		if (HanaAbs(body->mRotation.z) > unk1C0->mSLFallDecideRotateZ.get()) {
			emitOneTimeSandPillar_(body);
			if (body->mRotation.z > 0.0f)
				unk194 = 179.0f;
			else
				unk194 = -179.0f;
			f32 diff = HanaAbs(body->unk13C - body->mRotation.z);
			unk198   = unk1C0->mSLWaveFallDownSpeed.get() * diff;
			f32 minSpeed = unk1C0->mSLFallDecideMinSpeed.get();
			if (unk198 < minSpeed)
				unk198 = minSpeed;
			return true;
		}
	}
	return false;
}

// TODO: the target calls getBodyMaxRotateZ() out of line from perform(), but
// inlines it into execSlip(); our compiler can't pick per call site, so
// execSlip() uses the fabricated inline copy below
#pragma dont_inline on
f32 TBossHanachan::getBodyMaxRotateZ() const
{
	f32 max = 0.0f;
	for (int i = 0; i < ARRAY_COUNT(unk150); ++i) {
		if (fabsf(unk150[i]->mRotation.z) > fabsf(max))
			max = unk150[i]->mRotation.z;
	}
	return max;
}
#pragma dont_inline off

static inline f32 SqXZ(f32 ax, f32 az, f32 bx, f32 bz)
{
	return JGeometry::TVec3<f32>(ax - bx, 0.0f, az - bz).squared();
}

// TODO: the target calls TVec3::set<f> out of line here (frame-only otherwise)
bool TBossHanachan::isCanWalk() const
{
	bool canWalk = true;
	JGeometry::TVec3<f32> goal = unkF4.getPoint();
	f32 distSq = SqXZ(goal.x, goal.z, mPosition.x, mPosition.z);
	if (distSq < CLBSquared(10.0f))
		canWalk = false;
	return canWalk;
}

void TBossHanachan::execWalk(bool run)
{
	if (run)
		CLBChaseGeneralConstantSpecifySpeed(
		    &mMarchSpeed, unk1C0->mSLMaxMarchSpeed.get(),
		    unk1C0->mSLMarchAccel.get());
	else
		CLBChaseGeneralConstantSpecifySpeed(&mMarchSpeed, 0.0f,
		                                    unk1C0->mSLMarchDecrease.get());

	mTurnSpeed = unk1C0->mSLWalkTurnSpeed.get();
	if (isCanWalk())
		walkToCurPathNode(mMarchSpeed, mTurnSpeed, 0.0f);

	JGeometry::TVec3<f32> goal = unkF4.getPoint();
	f32 distSq = SqXZ(goal.x, goal.z, mPosition.x, mPosition.z);
	if (distSq < CLBSquared(100.0f)) {
		if (!unk114.empty())
			switchNextGoalPath();
		else
			goToDirLimitedNextGraphNode(90.0f);
	}
}

void TBossHanachan::execSlip()
{
	CLBChaseGeneralConstantSpecifySpeed(&mMarchSpeed, 0.0f,
	                                    unk1C0->mSLMarchDecrease.get());
	mTurnSpeed = 0.1f;
	if (mMarchSpeed > 0.001f) {
		JGeometry::TVec3<f32> dir = unk188;
		if (mMarchSpeed > 4.0f) {
			f32 maxRotateZ = BodyMaxRotateZInline(this);
			JGeometry::TVec3<f32> side(1.0f, 0.0f, 0.0f);
			if (maxRotateZ > 0.0f)
				side.negate();
			s16 angle = CLBDegToShortAngle(mRotation.y);
			f32 s     = JMASSin(angle);
			f32 c     = JMASCos(angle);
			f32 push  = 0.005f * mMarchSpeed;
			JGeometry::TVec3<f32> add;
			add.x = side.x * c + side.z * s;
			add.y = side.y;
			add.z = -side.x * s + side.z * c;
			add *= push;
			dir += add;
		}
		bool zero = dir.isZero();
		if (!zero) {
			MsVECNormalize(&dir, &dir);
			dir.x *= 500.0f;
			dir.y *= 500.0f;
			dir.z *= 500.0f;
			JGeometry::TVec3<f32> goal = mPosition;
			goal.x += dir.x;
			goal.y += dir.y;
			goal.z += dir.z;
			setGoalPath(TPathNode(goal));
			walkToCurPathNode(mMarchSpeed, mTurnSpeed, 0.0f);
		}
		gpCameraShake->keepShake((EnumCamShakeMode)9, 1.0f);
		if (SMS_IsMarioTouchGround4cm() && mSpine->getTime() < 120)
			SMSRumbleMgr->start(22, (f32*)nullptr);
	}
}

void TBossHanachan::goToInitialRecoverGraphNode()
{
	unk124->reset();
	unk124->reset2();
	int index = unk124->getGraph()->findNearestVisibleIndex(
	    mPosition, mRotation.y, unk1BC->mSLRecoverSearchDist.get(),
	    unk1BC->mSLRecoverSearchDegree.get(), -1);
	if (index < 0) {
		goToShortestNextGraphNode();
		return;
	}
	unk124->setTo(index);
	setGoalPathFromGraph();
	unk128 = 0;
	unk12C = 0.0f;
}

void TBossHanachan::execDamage()
{
	mSpine->reset();
	decHitPoints();
	if (mHitPoints == 0) {
		unk170->unk100->onHitFilter(HIT_FILTER_NO_COLLISION);
		for (int i = 0; i < ARRAY_COUNT(unk150); ++i) {
			unk150[i]->unk100->onHitFilter(HIT_FILTER_NO_COLLISION);
			unk150[i]->unk118[0]->onHitFilter(HIT_FILTER_NO_COLLISION);
			unk150[i]->unk118[1]->onHitFilter(HIT_FILTER_NO_COLLISION);
		}
		mSpine->setNext(&TNerveBossHanachanDead::theNerve());
		setAnmTimerWhenDead();
		unk1AC = *gpMarioPos;
		SMSGetMSound()->startSoundActor(MSD_SE_BS_HANA_DOWN, &unk1AC, 0,
		                                nullptr, 0, 4);
	} else {
		mSpine->setNext(&TNerveBossHanachanDamage::theNerve());
		setAnmTimerWhenDamage();
		TBossHanachanManager* hm = (TBossHanachanManager*)mManager;
		int idx = 3 - mHitPoints;
		unk1C0 = (&hm->unk58[0])[idx];
		const char* railName = cHitPoint1_RailName;
		switch (mHitPoints) {
		case 1:
			railName = cHitPoint1_RailName;
			break;
		case 2:
			railName = cHitPoint2_RailName;
			break;
		}
		unk124->setGraph(gpConductor->getGraphByName(railName));
		onLiveFlag(LIVE_FLAG_UNK20000);
		SMSGetMSound()->startSoundActor(MSD_SE_BS_DAMAGE_SYMBL,
		                                &unk150[unk174]->unk154, 0, nullptr, 0,
		                                4);
	}
}

void TBossHanachan::removeAllMapCollision()
{
	unk170->unk104->remove();
	for (int i = 0; i < ARRAY_COUNT(unk150); ++i)
		unk150[i]->unk104->remove();
}

BOOL TBossHanachan::hasMapCollision() const { return TRUE; }
