#include "MoveBG/MapObjCorona.hpp"
#include "MoveBG/MapObjBase.hpp"
#include <JSystem/J3D/J3DGraphAnimator/J3DModel.hpp>
#include <JSystem/J3D/J3DGraphLoader/J3DModelLoader.hpp>
#include <M3DUtil/MActor.hpp>
#include <Map/MapCollisionEntry.hpp>
#include <JSystem/JMath.hpp>
#include <System/Particles.hpp>
#include <MarioUtil/MathUtil.hpp>
#include <JSystem/JGeometry.hpp>
#include <JSystem/JDrama/JDRNameRefGen.hpp>
#include <Enemy/Koopa.hpp>
#include <Player/MarioAccess.hpp>
#include <Camera/CameraShake.hpp>
#include <Enemy/BathtubKiller.hpp>
#include <Player/Mario.hpp>
#include <Player/NozzleTrigger.hpp>
#include <math.h>
#include <macros.h>
#include <MarioUtil/RumbleMgr.hpp>
#include <MSound/MSSetSound.hpp>
#include <MSound/MSoundBGM.hpp>
#include <System/MarDirector.hpp>
#include <GC2D/GCConsole2.hpp>
#include <MSound/MSound.hpp>
#include <MSound/SoundEffects.hpp>
#include <M3DUtil/InfectiousStrings.hpp>
#include <stdio.h>
#ifdef SMS_PORT
#include <stdlib.h> // getenv (SMS_PORT_BOSSTRACE, w5)
#endif

// Balloon message ids: GMSE01 passes plain ids (verified against asm), the
// other versions use 0xE0000-based ids.
#ifdef VERSION_GMSP01
#define BATHTUB_MSG_BASE 0x0
#define BATHTUB_MSG_FINAL 0x2D
#else
#define BATHTUB_MSG_BASE 0xE0000
#define BATHTUB_MSG_FINAL 0xE0030
#endif

TBathtubParams::TBathtubParams()
    : TParams("/MapObj/bathtub.prm")
    , PARAM_INIT(resetGrip, 0)
    , PARAM_INIT(trampleRelease, 10)
    , PARAM_INIT(trampleRecover, 10)
    , PARAM_INIT(quakeRelease, 500)
    , PARAM_INIT(quakeRecover, 500)
    , PARAM_INIT(hipdropRelease, 35)
    , PARAM_INIT(hipdropRecover, 35)
    , PARAM_INIT(breakCount0, 750)
    , PARAM_INIT(breakCount1, 710)
    , PARAM_INIT(breakCount2, 685)
    , PARAM_INIT(breakCount3, 655)
    , PARAM_INIT(launchStopCount, 1000)
    , PARAM_INIT(animSpeed0, 0.15f)
    , PARAM_INIT(animSpeed1, 0.15f)
    , PARAM_INIT(animSpeed2, 0.15f)
    , PARAM_INIT(animSpeed3, 0.15f)
    , PARAM_INIT(animSpeed4, 0.22f)
    , PARAM_INIT(shake, 0.0f)
    , PARAM_INIT(watermark, 0.3f)
    , PARAM_INIT(maxAngle, 35.0f)
    , PARAM_INIT(angleVelDamp, 0.93f)
    , PARAM_INIT(rebound, 0.0005f)
    , PARAM_INIT(shakeDamp, 0.93f)
    , PARAM_INIT(marioWeight, 0.01f)
    , PARAM_INIT(marioDropWeight, 5.0f)
    , PARAM_INIT(outerHeight, 20.0f)
{
	TParams::load(mPrmPath);
}

// Unused
TBathtubGripParts::TBathtubGripParts(const char* name, int index,
                                     TBathtubGrip* parent)
    : TLiveActor(name)
    , unkF4(parent)
    , unkF8(index)
{
}

// Unused
TBathtubGripPartsFragile::TBathtubGripPartsFragile(int index,
                                                   TBathtubGrip* parent)
    : TBathtubGripParts("バスタブの足場の一部（弱点）", index, parent)
{
}

// Unused
TBathtubGripPartsHard::TBathtubGripPartsHard(int index, TBathtubGrip* parent)
    : TBathtubGripParts("バスタブの足場の一部（壊れない）", index, parent)
{
}

Mtx* TBathtubGripParts::getRootJointMtx() const
{
    // FAKEMATCH(frame): 16 bytes; real construct unknown
    volatile u32 pad[4]; (void)pad;
	return (Mtx*)unkF4->getModel()->getAnmMtx(unkF4->unk200[unkF8]);
}

BOOL TBathtubGripPartsFragile::receiveMessage(THitActor* sender, u32 message)
{
	return unkF4->receiveMessage(sender, message);
}

BOOL TBathtubGripPartsHard::receiveMessage(THitActor* sender, u32 message)
{
	if (message == HIT_MESSAGE_SUPER_HIP_DROP)
		message = HIT_MESSAGE_HIP_DROP;
	return unkF4->receiveMessage(sender, message);
}

void TBathtubGrip::kill()
{
	unk24A = 1;
	makeObjDead();
	removeCollisions_();
}

// Unused
void TBathtubGrip::reset()
{
	offLiveFlag(LIVE_FLAG_DEAD);
	unk248 = 0;
	unk24A = 0;
	unk249 = 1;
	unk24B = 0;
	startAnim(0);
	J3DFrameCtrl* ctrl = mMActor->getFrameCtrl(0);
	if (ctrl != nullptr) {
		ctrl->setFrame(0.0f);
		ctrl->setRate(0.0f);
	}
	unk250 = 1.0f;
	unk258 = 100;
	unk260 = 0;
}

TBathtubGrip::TBathtubGrip(TBathtub* param_1, f32 param_2,
                           MActorAnmData* param_3, const char* name)
    : TMapObjBase(name)
{
	unk25C = new MActor(param_3);
	void* resource
	    = JKRGetResource("/scene/map/map/stand_effect/stand_effect.bmd");
	u32 modelFlags = 0x50050000;
	unk25C->setModel(
	    new J3DModel(J3DModelLoaderDataBase::load(resource, modelFlags), 0, 1),
	    modelFlags);
	unk254 = 0;
	unk244 = param_1;
	unk24C = param_2;
	initAndRegister("stand_break");
	calcRootMatrix();
	getModel()->calc();
	JUTNameTab* names = getModel()->getModelData()->getJointName();
	for (int i = 0; i < ARRAY_COUNT(unk164); ++i) {
		char joint[32];
		char path[256];
		sprintf(joint, "c%d", i + 1);
		sprintf(path, "/scene/mapObj/stand_break_%s.col", joint);
		unk200[i] = names->getIndex(joint);
		unk164[i] = new TMapCollisionMove;
		unk1BC[i] = new TBathtubGripPartsHard(i, this);
		unk164[i]->init(path, 0, unk1BC[i]);
		if (i < ARRAY_COUNT(unk150)) {
			sprintf(joint, "b%d", i + 1);
			sprintf(path, "/scene/mapObj/stand_break_%s.col", joint);
			unk150[i] = new TMapCollisionMove;
			unk1A8[i] = new TBathtubGripPartsFragile(i, this);
			unk150[i]->init(path, 0, unk1A8[i]);
		}
	}
	reset();
}

void TBathtubGrip::perform(u32 cue, JDrama::TGraphics* graphics)
{
	TMapObjBase::perform(cue, graphics);
	if (unk260 != 0)
		return;

	if (cue & CUE_MOVE) {
		MTXCopy(*getRootJointMtx(), unk25C->getModel()->getBaseTRMtx());
		// isCracking() written out: the inline materializes a bool here
		if (unk254 > 0 || unk248 != 0) {
			if (unk25C->curAnmEndsNext(0, nullptr))
				unk260 = 1;
		}
	}

	unk25C->perform(cue, graphics);
}

// Unused
bool TBathtubGrip::isCracking() const { return unk254 > 0 || unk248 != 0; }

// Unused
void TBathtubGrip::startCrack()
{
	unk254 = 1;
	unk25C->setBck("stand_effect");
	unk25C->setBtk("stand_effect");
	unk25C->setBrk("stand_effect");
	J3DFrameCtrl* ctrl = unk25C->getFrameCtrl(0);
	if (ctrl != nullptr)
		ctrl->setRate(SMSGetAnmFrameRate() * 12.0f * 0.5f);
}

// Unused
void TBathtubGrip::startBreak(int animation, int count, f32 speed)
{
	unk258 = count;
	unk250 = speed;
	startCrack();
	unk248 = 1;
	unk249 = 0;
	if (animation == 0)
		animation = 1;
	else if (animation == 1)
		animation = 0;
	startAnim(animation);
}

BOOL TBathtubGrip::receiveMessage(THitActor* sender, u32 message)
{
	switch (message) {
	case HIT_MESSAGE_SUPER_HIP_DROP: {
#ifdef SMS_PORT
		if (getenv("SMS_PORT_BOSSTRACE"))
			OSReport("[boss] bathtub grip %p got a super hip drop (cracking %d, standing %d)\n", (void*)this, unk248,
			         unk249);
#endif
		if (unk248 != 0)
			return false;
		if (unk249 == 0)
			return false;

		unk138[0].set(SMS_GetMarioPos());
		SMSGetMSound()->startSoundActor(MSD_SE_OBJ_SUPERBLOCK_BREAK, &unk138[0],
		                                0, nullptr, 0, 4);

		u16 dead = unk244->getNumGripsDead();
		if (dead == 4) {
			unk244->startDemo();
			return true;
		}
		unk244->quake(sender->mPosition);

		s32 count;
		f32 speed;
		switch (dead) {
		case 1:
			count = unk244->unk16C->breakCount0.get();
			speed = unk244->unk16C->animSpeed1.get();
			break;
		case 2:
			count = unk244->unk16C->breakCount1.get();
			speed = unk244->unk16C->animSpeed2.get();
			break;
		case 3:
			count = unk244->unk16C->breakCount2.get();
			speed = unk244->unk16C->animSpeed3.get();
			break;
		case 4:
			count = unk244->unk16C->breakCount3.get();
			speed = unk244->unk16C->animSpeed4.get();
			break;
		default:
			count = unk244->unk16C->breakCount0.get();
			speed = unk244->unk16C->animSpeed0.get();
			break;
		}
		startBreak(dead, count, speed);
		return true;
	}
	case HIT_MESSAGE_HIP_DROP:
		unk244->hipdrop(sender->mPosition);
		return true;
	case HIT_MESSAGE_TRAMPLE:
		unk244->trample(sender->mPosition);
		return true;
	}
	return false;
}

Mtx* TBathtubGrip::getRootJointMtx() const
{
	return (Mtx*)getModel()->getBaseTRMtx();
}

void TBathtubGrip::calcRootMatrix()
{
	TPosition3f& out = (TPosition3f&)*getModel()->getBaseTRMtx();
	TPosition3f rot;
	MsMtxSetRotRPH(rot, 0.0f, unk24C, 0.0f);
	rot.ref(0, 3) = 0.0f;
	rot.ref(1, 3) = 0.0f;
	rot.ref(2, 3) = 0.0f;
	out.concat((TPosition3f&)*unk244->getRootJointMtx(), rot);
}

// Unused
bool TBathtubGrip::marioIsOn() const { return false; }

// Unused
void TBathtubGrip::removeCollisions_()
{
	for (int i = 0; i < ARRAY_COUNT(unk164); i++)
		unk164[i]->remove();
	for (int i = 0; i < ARRAY_COUNT(unk150); i++)
		unk150[i]->remove();
}

// Unused
void TBathtubGrip::setupCollisions_()
{
	for (int i = 0; i < ARRAY_COUNT(unk164); ++i) {
		unk164[i]->moveMtx(*unk1BC[i]->getRootJointMtx());
		unk164[i]->setUp();
	}
	for (int i = 0; i < ARRAY_COUNT(unk150); ++i) {
		unk150[i]->moveMtx(*unk1A8[i]->getRootJointMtx());
		unk150[i]->setUp();
	}
}

void TBathtubGrip::control()
{
	calcRootMatrix();
	TMapObjBase::control();
	if (unk24A != 0) {
		removeCollisions_();
		return;
	}

	mMActor->calcAnm();
	if (unk24B != 0)
		setupCollisions_();

	if (unk248 != 0) {
		if (animIsFinished()) {
			if (unk244->unk16C->resetGrip.get() != 0) {
				reset();
			} else {
				kill();
			}
		} else {
			J3DFrameCtrl* ctrl = mMActor->getFrameCtrl(0);
			if (ctrl != nullptr)
				ctrl->setRate(SMSGetAnmFrameRate() * unk250 * 0.5f);
			MtxPtr mtx = *unk1A8[0]->getRootJointMtx();
			unk138[1].set(mtx[0][3], mtx[1][3], mtx[2][3]);
			SMSGetMSound()->startSoundActor(MSD_SE_OBJ_STAND_BREAK, &unk138[1],
			                                0, nullptr, 0, 4);
			SMSRumbleMgr->start(8, unk138[0]);
		}
	} else {
		J3DFrameCtrl* ctrl = mMActor->getFrameCtrl(0);
		ctrl->setRate(0.0f);
		if (unk254 > 0) {
			ctrl->setFrame(1.0f);
			if (++unk254 > unk258) {
				unk248 = 1;
				unk254 = 0;
				gpMarioParticleManager->emitAndBindToMtxPtr(
				    MAP_MAP_MS_KP_BREAK_A, *getRootJointMtx(), 0, this);
				gpMarioParticleManager->emitAndBindToMtxPtr(
				    MAP_MAP_MS_KP_BREAK_B, *getRootJointMtx(), 0, this);
			}
		} else {
			ctrl->setFrame(0.0f);
		}
	}
}

void TBathtub::loadAfter()
{
	SMS_LoadParticle("/scene/map/map/ms_lkp_yuge1.jpa", MAP_MAP_MS_LKP_YUGE1);
	SMS_LoadParticle("/scene/map/map/ms_kp_funsui.jpa", MAP_MAP_MS_KP_FUNSUI);
	SMS_LoadParticle("/scene/map/map/ms_kp_break_a.jpa", MAP_MAP_MS_KP_BREAK_A);
	SMS_LoadParticle("/scene/map/map/ms_kp_break_b.jpa", MAP_MAP_MS_KP_BREAK_B);
}

void TBathtub::hipdrop(const JGeometry::TVec3<f32>& pos)
{
	if (unk29A != 0)
		return;
	if (unk250 > unk16C->hipdropRelease.get())
		return;

	JGeometry::TVec3<f32> dir;
	JGeometry::TVec3<f32> d;
	d.x = pos.x - mInitialPosition.x;
	d.y = 0.0f;
	d.z = pos.z - mInitialPosition.z;
	dir.normalize(d);

	unk250 = unk16C->hipdropRelease.get();
	unk258 = unk16C->hipdropRecover.get();
	unk25C = unk16C->hipdropRecover.get();
	unk254 = unk16C->hipdropRelease.get();

	static_cast<TKoopa*>(JDrama::TNameRefGen::search("クッパ"))->stagger(false);
}

void TBathtub::quake(const JGeometry::TVec3<f32>& pos)
{
	if (unk29A != 0)
		return;

	JGeometry::TVec3<f32> dir;
	JGeometry::TVec3<f32> d;
	d.x = pos.x - mInitialPosition.x;
	d.y = 0.0f;
	d.z = pos.z - mInitialPosition.z;
	dir.normalize(d);

	unk24C = 300;
	unk250 = unk16C->quakeRelease.get();
	unk258 = unk16C->quakeRecover.get();
	unk25C = unk16C->quakeRecover.get();
	unk254 = unk16C->hipdropRelease.get();
	unk248 = unk16C->launchStopCount.get();

	TKoopa* koopa = static_cast<TKoopa*>(JDrama::TNameRefGen::search("クッパ"));
	gpCameraShake->startShake(CAM_SHAKE_MODE_UNK25, 1.0f);
	gpCameraShake->startShake(CAM_SHAKE_MODE_UNK26, 1.0f);
	SMSRumbleMgr->start(4, (f32*)nullptr);
	JGeometry::TVec3<f32> up(0.0f, 1.0f, 0.0f);
	SMS_ThrowMario(up, 10.0f);
	koopa->getDown();
}

int TBathtub::getNumGripsDead() const
{
	int count = 0;
	for (int i = 0; i < 5; i++) {
		if (!unk168[i]->unk249)
			count++;
	}
	return count;
}

// Unused
void TBathtub::trample(const JGeometry::TVec3<f32>&)
{
	if (unk29A != 0)
		return;

	if (unk250 <= unk16C->trampleRelease.get()) {
		unk250 = unk16C->trampleRelease.get();
		unk258 = unk16C->trampleRecover.get();
		unk25C = unk16C->trampleRecover.get();
		unk254 = unk16C->hipdropRelease.get();
	}
}

// Unused
void TBathtub::liftMario(const JGeometry::TVec3<f32>& pos)
{
	f32 weight = unk16C->marioWeight.get();
	if (unk250 > 0)
		weight += unk16C->marioDropWeight.get();
	if (weight <= 0.0000000001f)
		return;

	static JGeometry::TVec3<f32> yDown(0.0f, -1.0f, 0.0f);
	JGeometry::TVec3<f32> lever;
	lever.sub(pos, mPosition);
	JGeometry::TVec3<f32> torque;
	torque.cross(lever, yDown);
	torque.scale(0.00000001f * weight);
	unk1E8.add(torque);
}

void TBathtub::tumble(f32 param_1, f32 param_2)
{
	if (unk29A != 0)
		return;

	s16 angle = DEG2SHORTANGLE(param_1);
	param_2 *= 0.0001f;

	JGeometry::TVec3<f32> delta;
	delta.x = param_2 * JMASCos(angle);
	delta.y = 0.0f;
	delta.z = param_2 * -JMASSin(angle);
	unk1E8.add(delta);
}

MtxPtr TBathtub::getTakingMtx()
{
	return mMActor->mModel->getAnmMtx(mMarioJntIdx);
}

// Unused
MtxPtr TBathtub::getShineMtx()
{
	return mMActor->mModel->getAnmMtx(mStarJntIdx);
}

// Unused
MtxPtr TBathtub::getShineEffectMtx()
{
	return unk29C->getModel()->getAnmMtx(mShineBodyJntIdx);
}

// Unused
MtxPtr TBathtub::getWaterMtx(int index)
{
	return mMActor->mModel->getAnmMtx(mWaterJntIdx[index]);
}

MtxPtr TBathtub::getSubmarineMtxInDemo()
{
	return mMActor->mModel->getAnmMtx(mSubmarineJntIdx);
}

MtxPtr TBathtub::getPeachMtxInDemo()
{
	return mMActor->mModel->getAnmMtx(mDuckJntIdx);
}

// Unused
MtxPtr TBathtub::getKoopaMtxInDemo() { return nullptr; }

MtxPtr TBathtub::getKoopaJrMtxInDemo()
{
	return mMActor->mModel->getAnmMtx(mJuniorJntIdx);
}

BOOL TBathtub::receiveMessage(THitActor* sender, u32 message)
{
#ifdef SMS_PORT
	if ((message == HIT_MESSAGE_HIP_DROP || message == HIT_MESSAGE_SUPER_HIP_DROP)
	    && getenv("SMS_PORT_BOSSTRACE"))
		OSReport("[boss] bathtub (not a grip) got a %s at %.0f %.0f %.0f\n",
		         message == HIT_MESSAGE_SUPER_HIP_DROP ? "super hip drop" : "hip drop", SMS_GetMarioPos().x,
		         SMS_GetMarioPos().y, SMS_GetMarioPos().z);
#endif
	switch (message) {
	case HIT_MESSAGE_HIP_DROP:
		hipdrop(SMS_GetMarioPos());
		return true;
	case HIT_MESSAGE_SUPER_HIP_DROP:
		hipdrop(SMS_GetMarioPos());
		return true;
	case HIT_MESSAGE_TRAMPLE:
		trample(SMS_GetMarioPos());
		return true;
	}
	return false;
}

Mtx* TBathtub::getRootJointMtx() const
{
	if (unk29A != 0)
		return (Mtx*)getModel()->getAnmMtx(0);
	return (Mtx*)getModel()->getBaseTRMtx();
}

#ifdef SMS_PORT
#include <stdlib.h>
extern "C" u32 port_frame_count(void);   // port_main.cpp
extern "C" float gPortItems[20][3];      // port_bosstrace.cpp (SMS_PORT_BOSSTRACE debug bots)
extern "C" int gPortItemCount;
extern "C" unsigned gPortGripFrame;
// SMS_PORT_BOSSTRACE (debug, w5): the bathtub grips still standing (unk249), for the
// PADSCRIPT ROCKET bot (stand on one, rocket up, ground pound: a super hip drop).
static void portBathtubBot(const TBathtub* tub)
{
	static const bool sOn = getenv("SMS_PORT_BOSSTRACE") != nullptr;
	if (!sOn)
		return;
	gPortItemCount = 0;
	for (int i = 0; i < 5; ++i) {
		const TBathtubGrip* g = tub->unk168[i];
		if (g == nullptr || !g->unk249)
			continue;
		// The grip's own matrix sits at the tub's centre (rotated); its fragile parts
		// are where it stands on the rim: their mean x/z, and the highest y.
		f32 x = 0.0f, y = -1e9f, z = 0.0f;
		int n = 0;
		for (int k = 0; k < 5; ++k) {
			if (g->unk1A8[k] == nullptr)
				continue;
			Mtx* pm = g->unk1A8[k]->getRootJointMtx();
			if (pm == nullptr)
				continue;
			x += (*pm)[0][3];
			z += (*pm)[2][3];
			if ((*pm)[1][3] > y)
				y = (*pm)[1][3];
			++n;
		}
		if (n == 0)
			continue;
		// The joint matrices leave out the tub's own scale (Mario starts on the rim at
		// radius ~3300, the grips' joints sit at ~5300): scale about the tub's position.
		const JGeometry::TVec3<f32>& c = tub->mPosition;
		gPortItems[gPortItemCount][0] = c.x + (x / n - c.x) * tub->mScaling.x;
		gPortItems[gPortItemCount][1] = c.y + (y - c.y) * tub->mScaling.y;
		gPortItems[gPortItemCount][2] = c.z + (z / n - c.z) * tub->mScaling.z;
		++gPortItemCount;
	}
	static int sLast = -1;
	if ((port_frame_count() % 120) == 0 && gPortItemCount > 0)
		OSReport("[boss] f%u bathtub grips y: %.0f %.0f %.0f %.0f %.0f, mario %.0f %.0f %.0f\n", port_frame_count(),
		         gPortItems[0][1], gPortItemCount > 1 ? gPortItems[1][1] : 0.0f,
		         gPortItemCount > 2 ? gPortItems[2][1] : 0.0f, gPortItemCount > 3 ? gPortItems[3][1] : 0.0f,
		         gPortItemCount > 4 ? gPortItems[4][1] : 0.0f, SMS_GetMarioPos().x, SMS_GetMarioPos().y,
		         SMS_GetMarioPos().z);
	if (gPortItemCount != sLast) {
		OSReport("[boss] f%u bathtub: %d grips standing (tub at %.0f %.0f %.0f, scale %.3f %.3f %.3f)\n",
		         port_frame_count(), gPortItemCount, tub->mPosition.x, tub->mPosition.y, tub->mPosition.z,
		         tub->mScaling.x, tub->mScaling.y, tub->mScaling.z);
		for (int i = 0; i < gPortItemCount; ++i)
			OSReport("[boss]   grip at %.0f %.0f %.0f\n", gPortItems[i][0], gPortItems[i][1], gPortItems[i][2]);
		sLast = gPortItemCount;
	}
	gPortGripFrame = port_frame_count();
}
#endif

void TBathtub::perform(u32 cue, JDrama::TGraphics* graphics)
{
#ifdef SMS_PORT
	if (cue & CUE_MOVE)
		portBathtubBot(this);
#endif
	TMapObjBase::perform(cue, graphics);
	if (cue & CUE_MOVE) {
		MTXCopy(getShineMtx(), unk29C->getModel()->getBaseTRMtx());
		JGeometry::TVec3<f32> scale(3.0f, 3.0f, 3.0f);
		unk29C->getModel()->setBaseScale(scale);
	}

	if (cue & CUE_MOVE) {
		int ticks = SMSGetMarDirector()->mMoveTickCount;
		switch (getNumGripsDead()) {
		case 0:
			if (ticks >= 7200)
				showMessage(BATHTUB_MSG_BASE + 0x1F);
			else if (ticks >= 3600)
				showMessage(BATHTUB_MSG_BASE + 0x1E);
			break;
		case 1:
			showMessage(BATHTUB_MSG_BASE + 0x20);
			break;
		case 2:
			showMessage(BATHTUB_MSG_BASE + 0x21);
			break;
		case 3:
			showMessage(BATHTUB_MSG_BASE + 0x22);
			break;
		case 4:
			showMessage(BATHTUB_MSG_FINAL);
			break;
		case 5:
			showMessage(BATHTUB_MSG_BASE + 0x23);
			break;
		}
	}
	if (cue & CUE_CALC_ANIM)
		unk29C->calc();
	if (cue & CUE_CALC_VIEW)
		unk29C->viewCalc();
	if (cue & CUE_ENTRY) {
		MtxPtr mtx = unk29C->getModel()->getBaseTRMtx();
		JGeometry::TVec3<f32> pos;
		pos.set(mtx[0][3], mtx[1][3], mtx[2][3]);
		unk29C->setLightData(mGroundPlane, pos);
		unk29C->entry();
	}
}

void TBathtub::control()
{
	if (unk29A != 0) {
		if (mMActor->curAnmEndsNext(0, nullptr)) {
			switch (unk294++) {
			case 0:
				startBck("bath_overturn2");
				break;
			case 1:
				startBck("bath_overturn3");
				break;
			}
		}
		JGeometry::TVec3<f32> scale(3.0f, 3.0f, 3.0f);
		JPABaseEmitter* emitter = gpMarioParticleManager->emitAndBindToMtxPtr(
		    PARTICLE_MS_SHINE_SENKO, getShineEffectMtx(), 1, this);
		if (emitter != nullptr)
			emitter->setGlobalScale(scale);
		emitter = gpMarioParticleManager->emitAndBindToMtxPtr(
		    PARTICLE_MS_SHINE_KIRA, getShineEffectMtx(), 1, this);
		if (emitter != nullptr)
			emitter->setGlobalScale(scale);

		MtxPtr shineMtx = getShineMtx();
		unk200.set(shineMtx[0][3], shineMtx[1][3], shineMtx[2][3]);
		SMSGetMSound()->startSoundActor(MSD_SE_SHINE_EXIST, &unk200, 0, nullptr,
		                                0, 4);
		calcRootMatrix();
		calcBathtubData();
		removeCollisions_();
		THitActor* mario = SMS_GetMarioHitActor();
		if (mario->receiveMessage(this, HIT_MESSAGE_TAKE))
			mHeldObject = static_cast<TTakeActor*>(mario);
		gpMarioOriginal->mFaceAngle.y = 0x7fff;
		if (unk290 > 0)
			--unk290;
		return;
	}

	if (unk24C > 0)
		--unk24C;
	if (unk248 > 0)
		--unk248;
	if (unk250 > unk16C->hipdropRelease.get()
	    || (marioIsOn() && mHeldObject == nullptr)) {
		liftMario(SMS_GetMarioPos());
	}
	updatePosture_();
	calcRootMatrix();
	TMapObjBase::control();
	calcBathtubData();
	getModel()->calc();
	JGeometry::TVec3<f32> up;
	mBathtubData.unk18.getYDir(up);
	if (mBathtubData.unk0C.dot(up) > 0.995f)
		gpMarioParticleManager->emit(MAP_MAP_MS_LKP_YUGE1, &unk1F4, 1, this);
	for (int i = 0; i < 5; ++i) {
		if (unk168[i]->unk248 != 0) {
			MtxPtr mtx = getWaterMtx(i);
			gpMarioParticleManager->emitAndBindToMtxPtr(MAP_MAP_MS_KP_FUNSUI,
			                                            mtx, 1, mtx);
		}
	}
	if (mHeldObject == nullptr)
		setupCollisions_();
	else
		removeCollisions_();
}

void TBathtub::calcBathtubData()
{
	Mtx* root = getRootJointMtx();

	mBathtubData.unk18.setXDir((*root)[0][0], (*root)[1][0], (*root)[2][0]);
	mBathtubData.unk18.setYDir((*root)[0][1], (*root)[1][1], (*root)[2][1]);
	mBathtubData.unk18.setZDir((*root)[0][2], (*root)[1][2], (*root)[2][2]);
	mBathtubData.mPos.set((*root)[0][3], (*root)[1][3], (*root)[2][3]);

	f32 y11        = mBathtubData.unk18.at(1, 1);
	f32 horizontal = JGeometry::TUtil<f32>::sqrt(1.0f - y11 * y11);
	f32 watermark  = unk16C->watermark.get();
	if (watermark < horizontal)
		watermark = horizontal;
	mBathtubData.unk44 = mBathtubData.unk3C * watermark;
	mBathtubData.unk48 = unk16C->outerHeight.get();
	mBathtubData.unk18.getYDir(mBathtubData.unk0C);
	mBathtubData.unk58.zero();

	TKoopa* koopa = static_cast<TKoopa*>(JDrama::TNameRefGen::search("クッパ"));
	if (koopa->effectsTumble() || unk24C > 0) {
		JGeometry::TVec3<f32> up(0.0f, 1.0f, 0.0f);
		JGeometry::TVec3<f32> axis;
		axis.cross(up, mBathtubData.unk0C);
		axis.normalize();
		if (!axis.isZero()) {
			f32 angle = unk16C->maxAngle.get() * 6.2831855f / 360.0f;
			JGeometry::TQuat4<f32> rotation;
			rotation.setRotate(axis, angle);
			rotation.rotate(mBathtubData.unk0C, mBathtubData.unk0C);
			mBathtubData.unk58.set(0.0f, unk16C->shake.get(), 0.0f);
		}
	} else {
		mBathtubData.unk0C.set(0.0f, 1.0f, 0.0f);
	}

	if (unk250 < unk254 / 2 && unk258 > 0)
		mBathtubData.unk64 = 1;
	else
		mBathtubData.unk64 = 0;
	mBathtubData.unk65 = unk29A;
	unk1F4     = mBathtubData.getThing();
}

void TBathtub::setupCollisions_()
{
	const TBathtubData& data = mBathtubData;
	f32 marioY = gpMarioPos->y;
	f32 dy     = marioY - data.mPos.y;
	f32 dx     = gpMarioPos->x - data.mPos.x;
	f32 dz     = gpMarioPos->z - data.mPos.z;
	f32 localZ = data.unk18.at(2, 2) * dz
	             + (data.unk18.at(0, 2) * dx + data.unk18.at(1, 2) * dy);
	f32 localX = data.unk18.at(2, 0) * dz
	             + (data.unk18.at(0, 0) * dx + data.unk18.at(1, 0) * dy);

	if (marioY < 300.0f || localX * localX + localZ * localZ < 0.01f) {
		for (int i = 0; i < 30; ++i)
			unk164[i]->remove();
		for (int i = 0; i < 5; ++i)
			unk168[i]->unk24B = 0;
		return;
	}

	f32 angle = atan2f(localX, localZ);
	if (angle < 0.0f)
		angle += 6.2831855f;

	f32 slot = angle * (30.0f / 6.2831855f);
	if (slot < 0.0f)
		slot += 30.0f;
	int start = ((int)((0.5f + slot) - 1.0f) + 30) % 30;

	int i = 0;
	for (; i < 2; ++i) {
		int index   = (i + start) % 30;
		f32 theta   = ((f32)(index + 1) * 6.2831855f) / 30.0f - 3.1415927f;
		f32 s       = sinf(theta);
		f32 c       = cosf(theta);
		TPosition3f rot;
		rot.set(c, 0.0f, s, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, -s, 0.0f, c, 0.0f);
		TPosition3f mtx;
		mtx.concat((TPosition3f&)*getModel()->getBaseTRMtx(), rot);
		unk164[index]->moveMtx((MtxPtr)&mtx);
		unk164[index]->setUp();
	}
	for (; i < 30; ++i)
		unk164[(i + start) % 30]->remove();

	for (int j = 0; j < 5; ++j)
		unk168[j]->unk24B = 0;
	unk168[((int)((5.0f * angle) / 6.2831855f) + 10) % 5]->unk24B = 1;
}

namespace {

// Unused
BOOL CameraDemoCallBack(u32, u32) { return false; }

} // namespace

void TBathtub::startDemo()
{
	if (unk29A != 0)
		return;

	MSBgm::stopTrackBGMs(7, 10);
	showMessage(BATHTUB_MSG_BASE + 0x23);
	unk290 = 10;

	for (int i = 0; i < 5; ++i)
		unk168[i]->kill();

	unk168[2]->reset();

	unk168[2]->startBreak(0, 2, unk16C->animSpeed1.get());

	onMapObjFlag(MAP_OBJ_FLAG_UNK8);
	offMapObjFlag(MAP_OBJ_FLAG_UNK100);
	startBck("bath_overturn1");
	THitActor* mario = SMS_GetMarioHitActor();
	if (mario->receiveMessage(this, HIT_MESSAGE_TAKE))
		mHeldObject = static_cast<TTakeActor*>(mario);
	gpMarioOriginal->mFaceAngle.y = 0x7fff;
	SMSGetMarDirector()->fireStartDemoCamera("koopa_last2", &mPosition, -1,
	                                         mRotation.y, false, nullptr, 0,
	                                         nullptr, JDrama::TFlagT<u16>(0));
	SMSGetMarDirector()->fireStreamingMovie(14);
	static_cast<TKoopa*>(JDrama::TNameRefGen::search("クッパ"))->fall();
	unk29A = 1;
}

// Unused
void TBathtub::removeCollisions_()
{
	for (int i = 0; i < 30; ++i)
		unk164[i]->remove();
}

// fabricated
inline JGeometry::TVec3<f32> bathtubToLocal(const TBathtubData& data, const JGeometry::TVec3<f32>& delta)
{
	return JGeometry::TVec3<f32>(data.unk18.at(0, 0) * delta.x + data.unk18.at(1, 0) * delta.y
		              + data.unk18.at(2, 0) * delta.z,
		          data.unk18.at(0, 1) * delta.x + data.unk18.at(1, 1) * delta.y
		              + data.unk18.at(2, 1) * delta.z,
		          data.unk18.at(0, 2) * delta.x + data.unk18.at(1, 2) * delta.y
		              + data.unk18.at(2, 2) * delta.z);
}

bool TBathtub::allowsTumble() const
{
	JGeometry::TVec3<f32> marioPos(SMS_GetMarioPos());
	f32 grip;
	if (getNearGrip(marioPos, 18.0f, &grip)) {

		JGeometry::TVec3<f32> delta(marioPos.x - mBathtubData.mPos.x,
		                            marioPos.y - mBathtubData.mPos.y,
		                            marioPos.z - mBathtubData.mPos.z);
		
		const TBathtubData& data = mBathtubData;
		JGeometry::TVec3<f32> flat = bathtubToLocal(data, delta);
		flat.y                     = 0.0f;
		f32 distance               = flat.length();
		if (distance < 4200.0f)
			return false;
		if (4700.0f < distance) {
			if (mBathtubData.unk18.at(1, 1) > 0.99f) {
				TBathtubKillerManager* manager = static_cast<TBathtubKillerManager*>(
				    JDrama::TNameRefGen::search("バスタブキラーマネージャー"));
				u32 status = SMS_GetMarioStatus();
				if (status == MARIO_STATUS_HIP_DROP)
					return false;
				if (status == MARIO_STATUS_ROCKET)
					return false;
				if (status == MARIO_STATUS_ROCKET_LANDING)
					return false;

				const TWaterGun* fludd = gpMarioOriginal->mWaterGun;
				if (fludd != nullptr) {
					TNozzleTrigger* nozzle
					    = static_cast<TNozzleTrigger*>(fludd->getCurrentNozzle());
					if (nozzle != nullptr && nozzle->getNozzleKind() == 1
					    && nozzle->unk388 > 0.0f)
						return false;
				}
				return manager->countActiveKillers() == 0;
			}
			return false;
		}
		return true;
	}
	return false;
}

void TBathtub::calcRootMatrix()
{
	if (unk29A != 0) {
		TPosition3f& mtx = (TPosition3f&)*getModel()->getBaseTRMtx();
		MsMtxSetRotRPH(mtx, 0.0f, mRotation.y, 0.0f);
		mtx.setTrans(mPosition);
	} else {
		TPosition3f& mtx = (TPosition3f&)*getModel()->getBaseTRMtx();
		mtx.setQuat(unk1D8);
		mtx.setTrans(mPosition);
	}
}

// Unused
void QuatRotate(JGeometry::TQuat4<f32>& q, const JGeometry::TVec3<f32>& w)
{
	JGeometry::TQuat4<f32> dq;
	dq.xyz().scale(0.5f, w);
	dq.w = 0.0f;
	dq.mul(dq, q);
	q.x += dq.x;
	q.y += dq.y;
	q.z += dq.z;
	q.w += dq.w;
	q.normalize();
}

namespace {

// Unused
f32 getDir(MtxPtr mtx, const JGeometry::TVec3<f32>& pos)
{
	JGeometry::TVec3<f32> xDir(mtx[0][0], mtx[1][0], mtx[2][0]);
	JGeometry::TVec3<f32> zDir(mtx[0][2], mtx[1][2], mtx[2][2]);
	JGeometry::TVec3<f32> trans(mtx[0][3], mtx[1][3], mtx[2][3]);
	JGeometry::TVec3<f32> delta;
	delta.sub(pos, trans);
	f32 localZ = zDir.dot(delta);
	f32 localX = xDir.dot(delta);
	return (360.0f / 65536.0f) * matan(localZ, localX);
}

// Unused
f32 getDir(MtxPtr mtx, const JGeometry::TVec3<f32>& pos,
           const JGeometry::TVec3<f32>& direction)
{
	JGeometry::TVec3<f32> xDir(mtx[0][0], mtx[1][0], mtx[2][0]);
	JGeometry::TVec3<f32> zDir(mtx[0][2], mtx[1][2], mtx[2][2]);
	JGeometry::TVec3<f32> trans(mtx[0][3], mtx[1][3], mtx[2][3]);
	JGeometry::TVec3<f32> delta;
	delta.sub(pos, trans);
	JGeometry::TVec3<f32> normal;
	normal.normalize(delta);
	JGeometry::TVec3<f32> tangent;
	tangent.scaleAdd(-normal.dot(direction), direction, normal);
	delta += tangent;
	f32 localZ = zDir.dot(delta);
	f32 localX = xDir.dot(delta);
	return (360.0f / 65536.0f) * matan(localZ, localX);
}

} // namespace

// Unused
u8 TBathtub::getNearJuncture(const JGeometry::TVec3<f32>&) const { return 0; }

// TODO: the target does not inline this into allowsTumble
#pragma dont_inline on
bool TBathtub::getNearGrip(const JGeometry::TVec3<f32>& pos, f32 maxDiff,
                           f32* out) const
{
	f32 angle = getDir(*getRootJointMtx(), pos);

	f32 best  = 360.0f;
	int index = 0;
	for (int i = 0; i < ARRAY_COUNT(unk150); i++) {
		f32 diff = fabsf(
		    -180.0f
		    + std::fmodf(360.0f + (unk150[i] - angle - -180.0f), 360.0f));
		if (diff < best) {
			index = i;
			best  = diff;
		}
	}

	if (best < maxDiff) {
		*out = unk150[index];
		return true;
	}
	return false;
}
#pragma dont_inline off

f32 TBathtub::getNextJuncture(const JGeometry::TVec3<f32>& pos,
                              const JGeometry::TVec3<f32>& direction) const
{
	f32 angle = getDir(*getRootJointMtx(), pos, direction);
	f32 best  = 360.0f;
	int index = 0;
	for (int i = 0; i < ARRAY_COUNT(unk13C); ++i) {
		f32 diff = fabsf(
		    -180.0f
		    + std::fmodf(360.0f + (unk13C[i] - angle - -180.0f), 360.0f));
		if (diff < best) {
			index = i;
			best  = diff;
		}
	}
	return unk13C[index];
}

bool TBathtub::getNextGrip(const JGeometry::TVec3<f32>& pos,
                           const JGeometry::TVec3<f32>& direction, f32 maxDiff,
                           f32* out) const
{
	f32 angle = getDir(*getRootJointMtx(), pos, direction);
	f32 best  = 360.0f;
	int index = 0;
	for (int i = 0; i < ARRAY_COUNT(unk150); ++i) {
		f32 diff = fabsf(
		    -180.0f
		    + std::fmodf(360.0f + (unk150[i] - angle - -180.0f), 360.0f));
		if (diff < best) {
			index = i;
			best  = diff;
		}
	}
	if (best < maxDiff) {
		*out = unk150[index];
		return true;
	}
	return false;
}

// Unused
void TBathtub::showMessage(u32 message)
{
	u32 flag = 1 << (message - (BATHTUB_MSG_BASE + 0x1E));
	if (!(unk2A0 & flag))
		SMSGetMarDirector()->getConsole()->startAppearBalloon(message, true);
	unk2A0 |= flag;
}

void TBathtub::updatePosture_()
{
	static JGeometry::TVec3<f32> y(0.0f, 1.0f, 0.0f);

	if (unk250 == 0) {
		f32 rate;
		if (unk258 == 0) {
			rate = 1.0f;
		} else {
			unk258--;
			rate = 1.0f - (f32)unk258 / (f32)unk25C;
		}
		JGeometry::TVec3<f32> up;
		unk1D8.getYDir(up);
		JGeometry::TVec3<f32> axis;
		axis.cross(y, up);
		axis.normalize();
		f32 rebound = unk16C->rebound.get();
		rebound *= rate * -acosf(y.dot(up));
		axis.scale(rebound);
		unk1E8.scaleAdd(unk16C->angleVelDamp.get(), axis, unk1E8);
	} else {
		unk250--;
	}

	QuatRotate(unk1D8, unk1E8);

	JGeometry::TVec3<f32> up;
	unk1D8.getYDir(up);
	f32 angle    = acosf(y.dot(up));
	f32 maxAngle = DEG_TO_RAD(unk16C->maxAngle.get());
	f32 excess   = angle - maxAngle;
	if (excess > 0.0f) {
		JGeometry::TQuat4<f32> limit;
		limit.setRotate(up, y, excess / angle);
		unk1D8.mul(limit, unk1D8);
	}
	unk1D8.normalize();
}

void TBathtub::load(JSUMemoryInputStream& stream)
{
    // FAKEMATCH(frame): 16 bytes; real construct unknown
    volatile u32 pad[4]; (void)pad;
	unk24C = 0;
	TMapObjBase::load(stream);
	mPosition.set(mInitialPosition);
	unk164 = new TMapCollisionMove*[30];
	for (int i = 0; i < 30; ++i) {
		unk164[i]        = new TMapCollisionMove;
		const char* path = nullptr;
		switch (i % 6) {
		case 0:
			path = "/scene/mapObj/bath_col_inside3.col";
			break;
		case 1:
			path = "/scene/mapObj/bath_col_inside2.col";
			break;
		case 2:
			path = "/scene/mapObj/bath_col_inside1.col";
			break;
		case 3:
			path = "/scene/mapObj/bath_col_inside6.col";
			break;
		case 4:
			path = "/scene/mapObj/bath_col_inside5.col";
			break;
		case 5:
			path = "/scene/mapObj/bath_col_inside4.col";
			break;
		}
		unk164[i]->init(path, 0, this);
		unk164[i]->setUp();
	}
	mBathtubData.mPos.set(mInitialPosition);
	mBathtubData.unk18.identity();
	mBathtubData.unk3C = 3000.0f;
	mBathtubData.unk40 = 3600.0f;
	mBathtubData.unk44 = mBathtubData.unk3C * sinf(0.27925268f);
	mBathtubData.unk4C = mBathtubData.unk50 = mBathtubData.unk54 = 0.0f;
	mBathtubData.unk58.zero();
	mBathtubData.unk48 = 100.0f;
	mBathtubData.unk64 = 0;
	mBathtubData.unk0C.set(0.0f, 1.0f, 0.0f);
	unk168 = new TBathtubGrip*[5];
	unk138 = new MActorAnmData;
	unk138->init("scene/map/map/stand_effect", nullptr);
	for (int i = 0; i < 5; ++i) {
		f32 offset    = 360.0f * (0.5f + i) / 5.0f - 180.0f;
		unk168[i % 5] = new TBathtubGrip(this, offset, unk138);
		unk168[i % 5]->appear();
		unk13C[i] = -180.0f + 360.0f * (0.5f + i) / 5.0f;
		unk150[i] = -180.0f + 360.0f * i / 5.0f;
	}
	JUTNameTab* names        = getModel()->getModelData()->getJointName();
	mMarioJntIdx             = names->getIndex("mario");
	mStarJntIdx              = names->getIndex("star");
	mWaterJntIdx[0]          = names->getIndex("water4");
	mWaterJntIdx[1]          = names->getIndex("water5");
	mWaterJntIdx[2]          = names->getIndex("water1");
	mWaterJntIdx[3]          = names->getIndex("water2");
	mWaterJntIdx[4]          = names->getIndex("water3");
	mDuckJntIdx              = names->getIndex("ahiru");
	mSubmarineJntIdx         = names->getIndex("submarin");
	mJuniorJntIdx            = names->getIndex("Jr");
	mKoopaJntIdx             = names->getIndex("koopa");
	MActorAnmData* shineData = new MActorAnmData;
	shineData->init("/scene/map/map/shine", nullptr);
	unk29C    = new MActor(shineData);
	void* res = JKRGetResource("/scene/map/map/shine/shine_3bai.bmd");
	unk29C->setModel(
	    new J3DModel(J3DModelLoaderDataBase::load(res, 0x10000000), 0, 1),
	    0x10000000);
	mShineBodyJntIdx
	    = unk29C->getModel()->getModelData()->getJointName()->getIndex("body");
	unk298 = true;
}

TBathtub::TBathtub(const char* name)
    : TMapObjBase(name)
    , unk164(nullptr)
    , mBathtubData()
    , unk290(0)
{
	unk16C = new TBathtubParams;
	unk1D8.set(0.0f, 0.0f, 0.0f, 1.0f);
	mPosition.zero();
	unk1E8.zero();
	unk250 = 0;
	unk254 = 1;
	unk258 = 0;
	unk25C = 1;
	unk248 = 0;
	unk298 = 0;
	unk23C = unk240 = unk244 = 0.0f;
	unk299                   = 0;
	unk29A                   = 0;
	unk2A0                   = 0;
	unk294                   = 0;
}

// Unused
bool TBathtub::isKillerLaunchable() const
{
	if (unk29A != 0)
		return false;

	TKoopa* koopa = static_cast<TKoopa*>(JDrama::TNameRefGen::search("クッパ"));
	if (!koopa->allowsLaunch())
		return false;

	return isKillerAttackable();
}

int TBathtub::getNumKillerLaunchable() const
{
	if (!isKillerLaunchable())
		return 0;

	int num = getNumGripsDead() + 1;
	if (num < 2)
		num = 2;
	if (num > 4)
		num = 4;
	return num;
}

bool TBathtub::isKillerAttackable() const { return unk248 <= 0; }

// Unused
bool TBathtub::isBreaking() const { return false; }

int TBathtub::getNumKillerBurstable() const
{
	if (!isKillerLaunchable())
		return 0;

	int num = getNumGripsDead();
	if (num >= 4)
		return 8;

	if (!allowsTumble() && unk250 == 0 && unk258 == 0) {
		switch (num) {
		case 1:
			return 4;
		case 2:
			return 6;
		case 3:
			return 8;
		case 4:
			return 8;
		}
	}
	return 0;
}

TBathtub::~TBathtub() { }
