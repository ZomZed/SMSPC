#include <Enemy/TinKoopa.hpp>
#include <Enemy/CoasterKiller.hpp>
#include <Enemy/Conductor.hpp>
#include <Enemy/EffectObj.hpp>
#include <Enemy/Graph.hpp>
#include <M3DUtil/MActor.hpp>
#include <M3DUtil/MActorData.hpp>
#include <Strategic/ObjModel.hpp>
#include <GC2D/GCConsole2.hpp>
#include <Player/Mario.hpp>
#include <Player/MarioAccess.hpp>
#include <Camera/CameraShake.hpp>
#include <MarioUtil/MathUtil.hpp>
#include <MSound/MSound.hpp>
#include <MSound/MSoundSE.hpp>
#include <System/EmitterViewObj.hpp>
#include <System/FlagManager.hpp>
#include <System/MarDirector.hpp>
#include <System/Particles.hpp>
#include <Strategic/Strategy.hpp>
#include <JSystem/JDrama/JDRNameRefGen.hpp>
#include <JSystem/JParticle/JPAEmitter.hpp>
#include <JSystem/JUtility/JUTNameTab.hpp>
#include <JSystem/J3D/J3DGraphAnimator/J3DModel.hpp>
#include <JSystem/J3D/J3DGraphAnimator/J3DCluster.hpp>

// rogue includes needed for matching sinit & bss
#include <MSound/MSSetSound.hpp>
#include <MSound/MSoundBGM.hpp>
#include <M3DUtil/InfectiousStrings.hpp>
#include <Map/MapCollisionEntry.hpp>

static const char* tinkoopa_bastable[] = {
	"/scene/tinkoopa/bas/tinkoopa_break1.bas",
	"/scene/tinkoopa/bas/tinkoopa_break2.bas",
	"/scene/tinkoopa/bas/tinkoopa_break3.bas",
	"/scene/tinkoopa/bas/tinkoopa_break4.bas",
	nullptr,
	"/scene/tinkoopa/bas/tinkoopa_damage1.bas",
	"/scene/tinkoopa/bas/tinkoopa_damage2.bas",
	"/scene/tinkoopa/bas/tinkoopa_damage3.bas",
	"/scene/tinkoopa/bas/tinkoopa_damage4.bas",
	nullptr,
	nullptr,
	nullptr,
	"/scene/tinkoopa/bas/tinkoopa_wait1.bas",
	"/scene/tinkoopa/bas/tinkoopa_wait2.bas",
	"/scene/tinkoopa/bas/tinkoopa_wait3.bas",
	"/scene/tinkoopa/bas/tinkoopa_wait4.bas",
	"/scene/tinkoopa/bas/tinkoopa_wait5.bas",
};

TTinKoopaParams::TTinKoopaParams(const char* path)
    : TSpineEnemyParams(path)
    , PARAM_INIT(mSLPartsHP, 2)
    , PARAM_INIT(mSLFlameHP, 10)
    , PARAM_INIT(mSLFlameRevivalTime, 10)
    , PARAM_INIT(mSLFlameDamageRadius0, 100.0f)
    , PARAM_INIT(mSLFlameDamageHeight0, 400.0f)
    , PARAM_INIT(mSLFlameDamageRadius1, 100.0f)
    , PARAM_INIT(mSLFlameDamageHeight1, 400.0f)
    , PARAM_INIT(mSLDamageRadius, 1000.0f)
    , PARAM_INIT(mSLDamageHeight0, 4000.0f)
    , PARAM_INIT(mSLDamageHeight1, 4000.0f)
    , PARAM_INIT(mSLKillerInterval, 30)
    , PARAM_INIT(mSLDefeatWaitTime, 600)
    , PARAM_INIT(mSLKillerApproachingDistance, 6000.0f)
{
	TParams::load(mPrmPath);
	mSLPartsHP.set(1);
	mSLFlameHP.set(50);
	mSLFlameRevivalTime.set(1200);
	mSLFlameDamageRadius0.set(500.0f);
	mSLFlameDamageHeight0.set(300.0f);
	mSLFlameDamageRadius1.set(700.0f);
	mSLFlameDamageHeight1.set(400.0f);
	mSLKillerInterval.set(60);
	mSLDamageRadius.set(800.0f);
	mSLDamageHeight0.set(4000.0f);
	mSLDamageHeight1.set(3400.0f);
	mSLDefeatWaitTime.set(160);
	mSLKillerApproachingDistance.set(2000.0f);
}


static const char* TTinKoopa_jointNameTable[] = {
	"jnt_head",     "jnt_breast",    "jnt_stomach",  "jnt_rarm",
	"jnt_larm",     "jnt_leg",       "jnt_leye",     "jnt_reye",
	"fire_null",    "fire_col_null", "jnt_femur",    "killer_null1",
	"killer_null2", "killer_null3",  "killer_null4",
};

static int TTinKoopa_jointIndexTable[15];

int TTinKoopa_getJointIndex(int index)
{
	return TTinKoopa_jointIndexTable[index];
}

const char* TTinKoopa_getCollisionFileName(int index)
{
	static const char* table[] = {
		"/scene/tinkoopa/head_col.col",    "/scene/tinkoopa/breast_col.col",
		"/scene/tinkoopa/stomach_col.col", "/scene/tinkoopa/rarm_col.col",
		"/scene/tinkoopa/larm_col.col",    "/scene/tinkoopa/leg_col.col",
	};
	return table[index];
}

const char* TTinKoopa_getPartsFileName(int index)
{
	static const char* table[] = {
		nullptr,
		"tinkoopa_breast.bmd",
		"tinkoopa_stomach.bmd",
		"tinkoopa_rarm.bmd",
		"tinkoopa_larm.bmd",
		nullptr,
	};
	return table[index];
}

int TTinKoopa_getBreakingAnimationIndex(int index)
{
	static int table[] = { 0, 4, 11, 9, 10, 0 };
	return table[index];
}

u32 TTinKoopa_getActorType(int index)
{
	static u32 table[] = {
		ACTOR_TYPE_BOSS_UNK19, ACTOR_TYPE_BOSS_UNK1A, ACTOR_TYPE_BOSS_UNK1B,
		ACTOR_TYPE_BOSS_UNK1D, ACTOR_TYPE_BOSS_UNK1C, ACTOR_TYPE_BOSS_UNK1E,
	};
	return table[index];
}

int TTinKoopa_getWaitAnimationIndex(int stage)
{
	static int table[] = { 12, 13, 14, 15, 16 };
	return table[stage];
}

int TTinKoopa_getBreakAnimationIndex(int stage)
{
	static int table[] = { 0, 1, 2, 3, 16 };
	return table[stage];
}

int TTinKoopa_getDamageAnimationIndex(int stage)
{
	static int table[] = { 5, 6, 7, 8, 16 };
	return table[stage];
}

int TTinKoopa_getPartsVisibleFrame(int stage)
{
	static int table[] = { 100, 134, 134, 100, 0 };
	return table[stage];
}

int TTinKoopa_getBreakingPartsIndex(int stage)
{
	static int table[] = { 2, 3, 4, 1, 2 };
	return table[stage];
}

static const char* breastTrackJointNameTable[] = {
	"breast_1", "breast_2", "breast_3", "breast_4", "breast_5", "breast_6",
};

static const char* bellyTrackJointNameTable[] = {
	"stomach_1", "stomach_2", "stomach_3",
	"stomach_4", "stomach_5", "stomach_6",
};

static const char* rightArmTrackJointNameTable[] = {
	"rarm_1",
	"rarm_2",
	"rarm_3",
	"rarm_4",
};

static const char* leftArmTrackJointNameTable[] = {
	"larm_1",
	"larm_2",
	"larm_3",
	"larm_4",
};

void TTinKoopa::init(TLiveManager* manager)
{
	mManager = manager;
	mManager->manageActor(this);
	mSpine->initWith(&TNerveTinKoopaWait::theNerve());
	mMActorKeeper = new TMActorKeeper(mManager, 7);
	mMActor       = mMActorKeeper->createMActor("tinkoopa_body.bmd", 0);

	JUTNameTab* jointNames = getModel()->getModelData()->getJointName();
	for (int i = 0; i < 15; ++i)
		TTinKoopa_jointIndexTable[i]
		    = jointNames->getIndex(TTinKoopa_jointNameTable[i]);

	mFlame = new TTinKoopaFlame("flame", this);

	for (int i = 0; i < 6; ++i) {
		mParts[i] = new TTinKoopaPartsBase(TTinKoopa_getCollisionFileName(i),
		                                   i, this);
		mParts[i]->initTinKoopaPartsBase();
	}

	f32 height = getParams()->mSLDamageHeight0.get();
	f32 radius = getParams()->mSLDamageRadius.get();
	initHitActor(ACTOR_TYPE_BOSS_UNK18, 1, 0, 0.0f, 0.0f, radius, height);
	offHitFilter(HIT_FILTER_NO_COLLISION);
	mMActor->setLightType(1);

	J3DModel* model = mMActor->getModel();
	if (!model->getSkinDeform())
		model->setSkinDeform(new J3DSkinDeform, J3D_DEFORM_ATTACH_FLAG_UNK_1);

	calcRootMatrix();
	mMActor->calc();

	mKillerGraph          = gpConductor->getGraphByName("killer");
	mCoasterDistanceTable = new f32[mKillerGraph->getNodeNum() - 1];
	makeCoasterDistanceTable();

	mLaunchSchedule = new TTinKoopaLaunchSchedule(11, this);
	makeLaunchSchedule();
	initAnmSound();
	resetTinKoopa();
}

// TODO: the target never inlines this into TTinKoopa::init
#pragma dont_inline on
void TTinKoopa::makeLaunchSchedule()
{
	int i = 0;
	mLaunchSchedule->mOrders[i++]->makeOrder(0, 600, -1, 0);
	mLaunchSchedule->mOrders[i++]->makeOrder(0, 1230, -1, 1);
	mLaunchSchedule->mOrders[i++]->makeOrder(0, 2450, -1, 1);
	mLaunchSchedule->mOrders[i++]->makeOrder(1, 240, -1, 0);
	mLaunchSchedule->mOrders[i++]->makeOrder(1, 280, -1, 1);
	mLaunchSchedule->mOrders[i++]->makeOrder(1, 630, -1, 0);
	mLaunchSchedule->mOrders[i++]->makeOrder(1, 900, -1, 0);
	mLaunchSchedule->mOrders[i++]->makeOrder(1, 1200, -1, 1);
	mLaunchSchedule->mOrders[i++]->makeOrder(2, 565, -1, 0);
	mLaunchSchedule->mOrders[i++]->makeOrder(2, 700, -1, 0);
	mLaunchSchedule->mOrders[i++]->makeOrder(2, 2220, -1, 1);
}
#pragma dont_inline off

TTinKoopaFlame::TTinKoopaFlame(const char* name, TTinKoopa* owner)
    : THitActor(name)
    , mOwner(owner)
{
	f32 height = mOwner->getParams()->mSLFlameDamageHeight0.get();
	f32 radius = mOwner->getParams()->mSLFlameDamageRadius0.get();
	initHitActor(ACTOR_TYPE_BOSS_UNK27, 0, 0, 0.0f, 0.0f, radius, height);
	offHitFilter(HIT_FILTER_NO_COLLISION);

	TIdxGroupObj* group = static_cast<TIdxGroupObj*>(
	    JDrama::TNameRefGen::search("敵グループ"));
	group->getChildren().push_back(this);

	resetTinKoopaFlame();
}

void TTinKoopaFlame::makeHitCollision()
{
	if (mOwner->mBreakStage == 0) {
		setHitParams(0.0f, 0.0f,
		             mOwner->getParams()->mSLFlameDamageRadius0.get(),
		             mOwner->getParams()->mSLFlameDamageHeight0.get());
	} else if (mOwner->mBreakStage == 1) {
		setHitParams(0.0f, 0.0f,
		             mOwner->getParams()->mSLFlameDamageRadius1.get(),
		             mOwner->getParams()->mSLFlameDamageHeight1.get());
	}
}

void TTinKoopaFlame::resetTinKoopaFlame()
{
	mFlameHP     = mOwner->getParams()->mSLFlameHP.get();
	mFlameScale  = 1.0f;
	mHitByWater  = false;
}

BOOL TTinKoopaFlame::receiveMessage(THitActor* sender, u32 message)
{
	if (message == HIT_MESSAGE_SPRAYED_BY_WATER) {
		hitWater();
		return true;
	}
	return false;
}

void TTinKoopaFlame::hitWater()
{
	if (mOwner->mFlameRevivalTimer > 0)
		return;

	if (mFlameHP > 0)
		--mFlameHP;

	if (mFlameHP <= 0) {
		mFlameHP = mOwner->getParams()->mSLFlameHP.get();
		// TODO: the revival time goes through an s16 truncation
		mOwner->mFlameRevivalTimer
		    = (s16)mOwner->getParams()->mSLFlameRevivalTime.get();
		onHitFilter(HIT_FILTER_NO_COLLISION);
	}

	if (!mHitByWater) {
		mHitByWater = true;
		gpMarioParticleManager->emitAndBindToPosPtr(0xF3, &mPosition, 0, this);
	}
}

void TTinKoopaFlame::perform(u32 cue, JDrama::TGraphics* graphics)
{
	THitActor::perform(cue, graphics);

	if (cue & CUE_MOVE)
		checkMario();

	if (cue & CUE_CALC_ANIM) {
		MtxPtr mtx = mOwner->getModel()->getAnmMtx(TTinKoopa_getJointIndex(9));
		mPosition.x = mtx[0][3];
		mPosition.y = mtx[1][3];
		mPosition.z = mtx[2][3];
		emitFlameEffects();
		if (mOwner->mFlameRevivalTimer <= 0)
			offHitFilter(HIT_FILTER_NO_COLLISION);
		mHitByWater = false;
	}
}

void TTinKoopaFlame::checkMario()
{
	if (mOwner->mFlameRevivalTimer > 0)
		return;

	if (mOwner->mBreakStage == 4)
		return;

	int frame = isHighPosition() ? 2750 : 3400;
	if (mOwner->checkTruckAnimationPass(frame))
		SMS_SendMessageToMario(this, HIT_MESSAGE_ATTACK);
}

bool TTinKoopaFlame::isHighPosition() { return mOwner->mBreakStage == 0; }

void TTinKoopaFlame::emitFlameEffects()
{
	if (mOwner->mSpine->getCurrentNerve() != &TNerveTinKoopaWait::theNerve())
		return;

	MtxPtr mtx = mOwner->getModel()->getAnmMtx(TTinKoopa_getJointIndex(8));

	f32 stageScale = 1.0f;
	if (mOwner->mBreakStage != 0)
		stageScale = 1.6f;

	if (mOwner->mFlameRevivalTimer > 0) {
		mFlameScale -= 0.05f;
		if (mFlameScale < 0.3f)
			mFlameScale = 0.3f;
	} else {
		mFlameScale += 0.05f;
		if (mFlameScale > 1.0f)
			mFlameScale = 1.0f;
	}

	f32 scale = mFlameScale * stageScale;
	JGeometry::TVec3<f32> emitterScale(scale, scale, scale);
	if (mOwner->mFlameRevivalTimer > 0)
		emitterScale.y *= 0.5f;

	JPABaseEmitter* emitter;
	emitter = gpMarioParticleManager->emitAndBindToMtxPtr(0x1BB, mtx, 1, this);
	if (emitter)
		emitter->setGlobalScale(emitterScale);

	emitter = gpMarioParticleManager->emitAndBindToMtxPtr(0x1BC, mtx, 1, this);
	if (emitter)
		emitter->setGlobalScale(emitterScale);

	emitter = gpMarioParticleManager->emitAndBindToMtxPtr(0x1F2, mtx, 3, this);
	if (emitter)
		emitter->setGlobalScale(emitterScale);

	SMSGetMSound()->startSoundActorWithInfo(MSD_SE_BS_MKP_FIRE, &mPosition,
	                                        nullptr, scale, 0, 0, nullptr, 0,
	                                        4);
}

TTinKoopaLaunchOrder::TTinKoopaLaunchOrder(TTinKoopa* owner)
    : mOwner(owner)
    , mLap(0)
    , mFrame(0)
    , mNum(0)
    , mDir(0)
{
}

void TTinKoopaLaunchOrder::makeOrder(s8 lap, long frame, s8 num, s8 dir)
{
	mLap   = lap;
	mFrame = frame;
	mNum   = num;
	mDir   = dir;
}

void TTinKoopaLaunchOrder::checkOrder()
{
	// FAKEMATCH(frame): 8 bytes; real construct unknown
	volatile u32 pad[2]; (void)pad;
	if (mOwner->mLap != mLap)
		return;

	if (!mOwner->checkTruckAnimationPass(mFrame))
		return;

	int num = 1;
	if (mNum == -1) {
		if (mOwner->mBreakStage == 0)
			num = 1;
		else if (mOwner->mBreakStage == 1)
			num = 1;
		else if (mOwner->mBreakStage == 2)
			num = 2;
		else if (mOwner->mBreakStage == 3)
			num = 3;
	} else {
		num = mNum;
	}

	if (mDir == 1)
		num = num <= 2 ? num : 2;

	mOwner->makeKillerQueue(num, mDir);
}

TTinKoopaLaunchSchedule::TTinKoopaLaunchSchedule(u8 num, TTinKoopa* owner)
    : mOrderNum(num)
    , mOwner(owner)
{
	mOrders = new TTinKoopaLaunchOrder*[mOrderNum];
	for (int i = 0; i < mOrderNum; ++i)
		mOrders[i] = new TTinKoopaLaunchOrder(owner);
}

void TTinKoopaLaunchSchedule::checkOrder()
{
	for (int i = 0; i < mOrderNum; ++i)
		mOrders[i]->checkOrder();
}

TTinKoopaPartsBase::TTinKoopaPartsBase(const char* name, int index,
                                       TTinKoopa* owner)
    : TLiveActor(name)
    , mIsBreaking(false)
    , mPartsIndex(index)
    , mOwner(owner)
    , mBreakMActor(nullptr)
{
}

// TODO: the target never inlines this into TTinKoopa::init
#pragma dont_inline on
void TTinKoopaPartsBase::initTinKoopaPartsBase()
{
	initHitActor(TTinKoopa_getActorType(mPartsIndex), 0, 0, 0.0f, 0.0f, 0.0f,
	             0.0f);
	onHitFilter(HIT_FILTER_NO_COLLISION);

	TIdxGroupObj* group = static_cast<TIdxGroupObj*>(
	    JDrama::TNameRefGen::search("敵グループ"));
	group->getChildren().push_back(this);

	mCollision = new TMapCollisionMove;
	mCollision->init(TTinKoopa_getCollisionFileName(mPartsIndex), 0, this);
	mCollision->setUpTrans(JGeometry::TVec3<f32>(0.0f, 0.0f, 0.0f));

	const char* partsFileName = TTinKoopa_getPartsFileName(mPartsIndex);
	if (partsFileName) {
		mBreakMActor
		    = mOwner->getActorKeeper()->createMActor(partsFileName, 0);
		mBreakMActor->setLightType(1);
	}

	resetTinKoopaPartsBase();
}
#pragma dont_inline off

void TTinKoopaPartsBase::reset() { resetTinKoopaPartsBase(); }

void TTinKoopaPartsBase::resetTinKoopaPartsBase()
{
	mIsBreaking = false;
	mCollision->setUpTrans(JGeometry::TVec3<f32>(0.0f, 0.0f, 0.0f));
}

void TTinKoopaPartsBase::startBreaking()
{
    // FAKEMATCH(frame): 8 bytes; real construct unknown
    volatile u32 pad[2]; (void)pad;
	mIsBreaking = true;

	MtxPtr mtx = mOwner->getModel()->getAnmMtx(
	    TTinKoopa_getJointIndex(mPartsIndex));
	mPosition.x = mtx[0][3];
	mPosition.y = mtx[1][3];
	mPosition.z = mtx[2][3];

	if (mBreakMActor) {
		mBreakMActor->setBckFromIndex(
		    TTinKoopa_getBreakingAnimationIndex(mPartsIndex));
		MtxPtr baseMtx = mBreakMActor->getModel()->getBaseTRMtx();
		baseMtx[0][3]  = mPosition.x;
		baseMtx[1][3]  = mPosition.y;
		baseMtx[2][3]  = mPosition.z;
		mBreakMActor->getModel()->setBaseTRMtx(baseMtx);
		emitPartsTrackEffects();
	}
}

void TTinKoopaPartsBase::emitPartsTrackEffects()
{
	if (!mBreakMActor)
		return;

	if (mPartsIndex == 1)
		emitPartsTrackEffects(breastTrackJointNameTable, 6);
	else if (mPartsIndex == 2)
		emitPartsTrackEffects(bellyTrackJointNameTable, 6);
	else if (mPartsIndex == 3)
		emitPartsTrackEffects(rightArmTrackJointNameTable, 4);
	else if (mPartsIndex == 4)
		emitPartsTrackEffects(leftArmTrackJointNameTable, 4);
}

void TTinKoopaPartsBase::emitPartsTrackEffects(const char** joint_names,
                                               int num)
{
	mBreakMActor->getModel()->calc();

	JUTNameTab* jointNames
	    = mBreakMActor->getModel()->getModelData()->getJointName();
	for (int i = 0; i < num; ++i) {
		int index = jointNames->getIndex(joint_names[i]);
		if (index < 0)
			return;

		MtxPtr mtx = mBreakMActor->getModel()->getAnmMtx(index);
		mEffectPos[i].set(mtx[0][3], mtx[1][3], mtx[2][3]);
		gpMarioParticleManager->emitAndBindToPosPtr(0xF4, &mEffectPos[i], 0,
		                                            mOwner);
	}
}

void TTinKoopaPartsBase::emitPartsDisappearEffects()
{
	if (!mBreakMActor)
		return;

	if (!mBreakMActor->checkCurBckFromIndex(
	        TTinKoopa_getBreakingAnimationIndex(mPartsIndex)))
		return;

	J3DFrameCtrl* ctrl = mBreakMActor->getFrameCtrl(0);
	int disappearFrame = 60;
	if (!ctrl->checkPass(disappearFrame))
		return;

	if (mPartsIndex == 1)
		emitPartsDisappearEffects(breastTrackJointNameTable, 6, 4.0f);
	else if (mPartsIndex == 2)
		emitPartsDisappearEffects(bellyTrackJointNameTable, 6, 4.0f);
	else if (mPartsIndex == 3)
		emitPartsDisappearEffects(rightArmTrackJointNameTable, 4, 3.0f);
	else if (mPartsIndex == 4)
		emitPartsDisappearEffects(leftArmTrackJointNameTable, 4, 3.0f);

	mOwner->mBreakingParts = nullptr;
}

void TTinKoopaPartsBase::emitPartsDisappearEffects(const char** joint_names,
                                                   int num, f32 scale)
{
	JGeometry::TVec3<f32> explosionScale = mScaling;
	explosionScale.x *= scale;
	explosionScale.y *= scale;
	explosionScale.z *= scale;

	JUTNameTab* jointNames
	    = mBreakMActor->getModel()->getModelData()->getJointName();
	for (int i = 0; i < num; ++i) {
		int index = jointNames->getIndex(joint_names[i]);
		if (index < 0)
			return;

		MtxPtr mtx = mBreakMActor->getModel()->getAnmMtx(index);
		mEffectPos[i].set(mtx[0][3], mtx[1][3], mtx[2][3]);
		TEffectExplosion* explosion
		    = (TEffectExplosion*)gpConductor->makeOneEnemyAppear(
		        mEffectPos[i], "エフェクト爆発マネージャー", 1);
		if (!explosion)
			return;

		explosion->generate(mEffectPos[i], explosionScale);
	}
}

BOOL TTinKoopaPartsBase::receiveMessage(THitActor* sender, u32 message)
{
	if (sender->getActorType() == ACTOR_TYPE_ROCKET) {
		mOwner->hitParts();
		return true;
	}
	return false;
}

void TTinKoopaPartsBase::perform(u32 cue, JDrama::TGraphics* graphics)
{
	TLiveActor::perform(cue, graphics);

	if (cue & CUE_MOVE)
		mCollision->moveMtx(mOwner->getModel()->getAnmMtx(
		    TTinKoopa_getJointIndex(mPartsIndex)));

	if (cue & CUE_CALC_ANIM) {
		if (mIsBreaking && mBreakMActor
		    && mBreakMActor->curAnmEndsNext(0, nullptr))
			mIsBreaking = false;
	}

	if (mIsBreaking && mBreakMActor)
		mBreakMActor->perform(cue, graphics);
}

TTinKoopaMtxCalc::TTinKoopaMtxCalc(TTinKoopa* owner)
    : M3UMtxCalcSIAnmBlendQuat()
    , mOwner(owner)
{
	mMotionBlendRatio = 0.0f;
}

void TTinKoopaMtxCalc::joinAnm(int index)
{
	M3UMtxCalcSIAnmBlendQuat::joinAnm(
	    mOwner->getActorKeeper()->getMActorAnmData()->getUnk2C()->getAnmPtr(
	        index));
}

void TTinKoopaMtxCalc::calc(u16 joint)
{
	M3UMtxCalcSIAnmBlendQuat::calc(joint);
}

TTinKoopa::TTinKoopa(const char* name)
    : TSpineEnemy(name)
    , mKillerManager(nullptr)
    , unk1F8(0)
{
	onLiveFlag(LIVE_FLAG_UNK10);
	offLiveFlag(LIVE_FLAG_UNK100);
	mScaledBodyRadius = 2000.0f;
}

void TTinKoopa::makeCoasterDistanceTable()
{
	JGeometry::TVec3<f32> next = mKillerGraph->indexToPoint(0);
	f32 total                  = 0.0f;
	for (int i = 0; i < mKillerGraph->getNodeNum() - 1; ++i) {
		JGeometry::TVec3<f32> prev = next;
		next                       = mKillerGraph->indexToPoint(i + 1);
		mCoasterDistanceTable[i]   = prev.distance(next);
		total += mCoasterDistanceTable[i];
	}
}

f32 TTinKoopa::calcCoasterDistance(int from, int to)
{
	f32 distance = 0.0f;
	for (int i = from; i < to; ++i)
		distance += mCoasterDistanceTable[i];
	return distance;
}

f32 TTinKoopa::calcCoasterDistanceInOrder(int from, int to)
{
	if (to >= from)
		return calcCoasterDistance(from, to);

	return calcCoasterDistance(0, to)
	       + calcCoasterDistance(from, mKillerGraph->getNodeNum() - 1);
}

bool TTinKoopa::checkKillerApproachingFromBack(TCoasterKiller* killer,
                                               JGeometry::TVec3<f32> pos,
                                               f32 distance)
{
	if (killer->checkLiveFlag(LIVE_FLAG_DEAD))
		return false;

	if (killer->getPathDir() != 0)
		return false;

	int nearest = mKillerGraph->findNearestNodeIndex(pos, 0xFFFFFFFF);
	return calcCoasterDistanceInOrder(killer->getPathIdx(), nearest)
	       <= distance;
}

void TTinKoopa::reset()
{
	TSpineEnemy::reset();
	mFlame->resetTinKoopaFlame();
	for (int i = 0; i < 6; ++i)
		mParts[i]->reset();
	makeHitCollision();
	resetTinKoopa();
	changeBck(TTinKoopa_getWaitAnimationIndex(mBreakStage));
}

void TTinKoopa::resetTinKoopa()
{
    // FAKEMATCH(frame): 40 bytes; real construct unknown
    volatile u32 pad[10]; (void)pad;
	if (!mKillerManager)
		mKillerManager = (TCoasterKillerManager*)JDrama::TNameRefGen::search(
		    "コースターキラーマネージャー");

	mKillerManager->getActiveObjNum();

	mKoopaRail               = gpMarioOriginal->mKoopaRail;
	mBreakStage              = 0;
	unk154                   = 0;
	unk158                   = 0;
	mLap                     = 0;
	mFirstFlameMessageShown  = false;
	mPartsHP                 = getParams()->mSLPartsHP.get();
	mBreakingParts           = nullptr;
	mKillerQueueNum          = 0;
	mKillerQueueIndex        = 0;
	mKillerQueue[0]          = 0;
	mKillerQueue[1]          = 0;
	mKillerQueue[2]          = 0;
	mKillerQueue[3]          = 0;
	mKillerTimer             = 0;
	mFlameRevivalTimer       = 0;
	mDefeatTimer             = 0;
	mKillerTimer             = 0;
	unk1B4                   = 0.0f;
	unk1B8                   = 30.0f;
	unk1BC                   = 15.0f;
	unk1C0                   = 45.0f;
}

void TTinKoopa::makeHitCollision()
{
	if (mBreakStage == 0) {
		setHitParams(0.0f, 0.0f, getParams()->mSLDamageRadius.get(),
		             getParams()->mSLDamageHeight0.get());
	} else if (mBreakStage == 1) {
		setHitParams(0.0f, 0.0f, getParams()->mSLDamageRadius.get(),
		             getParams()->mSLDamageHeight1.get());
	} else if (mBreakStage == 2) {
	}

	mFlame->makeHitCollision();
}

void TTinKoopa::makeKillerQueue(int num, s8 dir)
{
	if (num > 4)
		num = 4;
	mKillerQueueIndex = 0;
	mKillerQueueNum   = num;
	for (int i = 0; i < mKillerQueueNum; ++i)
		mKillerQueue[i] = dir;
	mKillerTimer = 0;
}

void TTinKoopa::checkLap()
{
	if (!mKoopaRail)
		return;

	J3DFrameCtrl* ctrl = mKoopaRail->getFrameCtrl(0);
	if (ctrl->checkPass(ctrl->getEnd() - 1)) {
		++mLap;
		if (mLap > 2)
			mLap = 0;
	}
}

bool TTinKoopa::checkTruckAnimationPass(int frame)
{
	if (!mKoopaRail)
		return false;

	if (mKoopaRail->getFrameCtrl(0)->checkPass(frame))
		return true;

	return false;
}

void printTinKoopaDebugInfo(TTinKoopa*)
{
	// TODO: debug-only function, no inlined copy seen
}

void TTinKoopa::perform(u32 cue, JDrama::TGraphics* graphics)
{
	if (cue & CUE_CALC_ANIM) {
		emitTinKoopaEffects();
		checkKillerLaunch();
	}

	if (cue & CUE_MOVE) {
		updateTimers();
		checkLap();
		mLaunchSchedule->checkOrder();
		checkTinKoopaMessage();
	}

	TSpineEnemy::perform(cue, graphics);
	mFlame->perform(cue, graphics);
	for (int i = 0; i < 6; ++i)
		mParts[i]->perform(cue, graphics);
}

void TTinKoopa::makeEyeBeamEffect()
{
	// TODO: no inlined copy seen. The map shows that the original (0x3B4
	// bytes) called an out-of-line
	// TRotation3<TMatrix34<SMatrix34C<f32> > >::setEular(f32, f32, f32),
	// presumably with the eye beam angles in unk1B4..unk1C0.
}

void TTinKoopa::updateTimers()
{
	// TODO: the target stores each decremented timer through its address,
	// as if through some inline taking it by reference
	if (mKillerTimer > 0)
		--mKillerTimer;
	if (mFlameRevivalTimer > 0)
		--mFlameRevivalTimer;
	if (mDefeatTimer > 0)
		--mDefeatTimer;
}

const char** TTinKoopa::getBasNameTable() const { return tinkoopa_bastable; }

void TTinKoopa::changeBck(int index)
{
	getMActor()->setBckFromIndex(index);
	setAnmSound(getBas(index));
}

BOOL TTinKoopa::receiveMessage(THitActor* sender, u32 message)
{
	if (sender->getActorType() == ACTOR_TYPE_ROCKET) {
		hitParts();
		return true;
	}
	return false;
}

// TODO: the target never inlines this into the receiveMessage functions
#pragma dont_inline on
void TTinKoopa::hitParts()
{
	if (mSpine->getCurrentNerve() == &TNerveTinKoopaBreak::theNerve())
		return;

	if (mSpine->getCurrentNerve() == &TNerveTinKoopaDamage::theNerve())
		return;

	if (mBreakStage == 4)
		return;

	startTinKoopaMessage(0x24);
	--mPartsHP;
	if (mPartsHP <= 0)
		mSpine->pushNerve(&TNerveTinKoopaBreak::theNerve());
	else
		mSpine->pushNerve(&TNerveTinKoopaDamage::theNerve());
}
#pragma dont_inline off

void TTinKoopa::startBreakingParts()
{
	mBreakingParts = mParts[TTinKoopa_getBreakingPartsIndex(mBreakStage)];
	mBreakingParts->startBreaking();
}

void TTinKoopa::launchKiller(int dir)
{
    // FAKEMATCH(frame): 8 bytes; real construct unknown
    volatile u32 pad[2]; (void)pad;
	TCoasterKiller* killer = (TCoasterKiller*)mKillerManager->getDeadEnemy();
	if (!killer)
		return;

	killer->reset();

	int idx;
	if (dir == 1)
		idx = mKillerQueueIndex + 11;
	else
		idx = 14 - mKillerQueueIndex;

	int jointIndex = TTinKoopa_getJointIndex(idx);
	getJointTransByIndex(jointIndex, &killer->mPosition);
	killer->mPathDir = dir;
	gpMarioParticleManager->emitAndBindToMtxPtr(
	    0xEF, getModel()->getAnmMtx(jointIndex), 0, this);
	SMSGetMSound()->startSoundActor(MSD_SE_EN_KILLER_FIRE, &killer->mPosition,
	                                0, nullptr, 0, 4);
}

void TTinKoopa::checkKillerLaunch()
{
	if (mSpine->getCurrentNerve() == &TNerveTinKoopaWait::theNerve()
	    && mKillerQueueIndex < mKillerQueueNum && mKillerTimer <= 0) {
		launchKiller(mKillerQueue[mKillerQueueIndex]);
		++mKillerQueueIndex;
		mKillerTimer = getParams()->mSLKillerInterval.get();
	}
}

void TTinKoopa::checkTinKoopaMessage()
{
	checkTinKoopaKillerApproachingMessage();
	checkTinKoopaFirstRocketMessage();
	checkTinKoopaFirstFlameMessage();
}

void TTinKoopa::checkTinKoopaKillerApproachingMessage()
{
	for (int i = 0; i < mKillerManager->getActiveObjNum(); ++i) {
		TCoasterKiller* killer = (TCoasterKiller*)mKillerManager->getObj(i);
		if (killer->checkLiveFlag(LIVE_FLAG_DEAD))
			continue;

		f32 distance = getParams()->mSLKillerApproachingDistance.get();
		if (checkKillerApproachingFromBack(killer, SMS_GetMarioPos(),
		                                   distance))
			startTinKoopaMessage(9);
	}
}

void TTinKoopa::checkTinKoopaFirstRocketMessage()
{
	if (!mKoopaRail)
		return;

	J3DFrameCtrl* ctrl = mKoopaRail->getFrameCtrl(0);
	if (mLap == 0 && ctrl->checkPass(300.0f))
		startTinKoopaMessage(0xA);
}

void TTinKoopa::checkTinKoopaFirstFlameMessage()
{
    // FAKEMATCH(frame): 24 bytes; real construct unknown
    volatile u32 pad[6]; (void)pad;
	if (!mKoopaRail)
		return;

	if (mFirstFlameMessageShown)
		return;

	if (mSpine->getCurrentNerve() != &TNerveTinKoopaWait::theNerve())
		return;

	J3DFrameCtrl* ctrl = mKoopaRail->getFrameCtrl(0);
	if (mBreakStage == 0) {
		if (ctrl->checkPass(2600.0f)) {
			startTinKoopaMessage(0xB);
			mFirstFlameMessageShown = true;
		}
	} else {
		if (ctrl->checkPass(3100.0f)) {
			startTinKoopaMessage(0xB);
			mFirstFlameMessageShown = true;
		}
	}
}

void TTinKoopa::startTinKoopaMessage(u32 message)
{
	gpMarDirector->getConsole()->startAppearBalloon(message, true);
}

void TTinKoopa::emitTinKoopaEffects()
{
	MtxPtr mtx;

	mtx = getModel()->getAnmMtx(TTinKoopa_getJointIndex(0));
	mHeadPos.set(mtx[0][3], mtx[1][3], mtx[2][3]);
	mtx = getModel()->getAnmMtx(TTinKoopa_getJointIndex(1));
	mBreastPos.set(mtx[0][3], mtx[1][3], mtx[2][3]);
	mtx = getModel()->getAnmMtx(TTinKoopa_getJointIndex(3));
	mRArmPos.set(mtx[0][3], mtx[1][3], mtx[2][3]);
	mtx = getModel()->getAnmMtx(TTinKoopa_getJointIndex(4));
	mLArmPos.set(mtx[0][3], mtx[1][3], mtx[2][3]);

	gpMarioParticleManager->emitAndBindToPosPtr(0x1AC, &mBreastPos, 1, this);

	if (mBreakStage > 1)
		gpMarioParticleManager->emitAndBindToMtxPtr(
		    0x1AD, getModel()->getAnmMtx(TTinKoopa_getJointIndex(3)), 1, this);

	if (mBreakStage > 2)
		gpMarioParticleManager->emitAndBindToMtxPtr(
		    0x1AE, getModel()->getAnmMtx(TTinKoopa_getJointIndex(4)), 1, this);

	if (mBreakStage <= 0 && mSpine->getCurrentNerve() == &TNerveTinKoopaWait::theNerve())
		gpMarioParticleManager->emitAndBindToMtxPtr(
		    0x1AF, getModel()->getAnmMtx(TTinKoopa_getJointIndex(2)), 1, this);

	gpMarioParticleManager->emitAndBindToMtxPtr(
	    0x1B0, getModel()->getAnmMtx(TTinKoopa_getJointIndex(3)), 1, this);
	gpMarioParticleManager->emitAndBindToMtxPtr(
	    0x1B0, getModel()->getAnmMtx(TTinKoopa_getJointIndex(4)), 1, &unk1F8 + 1);
	gpMarioParticleManager->emitAndBindToMtxPtr(
	    0x1B1, getModel()->getAnmMtx(TTinKoopa_getJointIndex(10)), 1, this);
	gpMarioParticleManager->emitAndBindToMtxPtr(
	    0x1B2, getModel()->getAnmMtx(TTinKoopa_getJointIndex(0)), 1, this);

	if (mSpine->getCurrentNerve() == &TNerveTinKoopaDamage::theNerve()
	    || mSpine->getCurrentNerve() == &TNerveTinKoopaBreak::theNerve()) {
		gpMarioParticleManager->emitAndBindToPosPtr(0x1B3, &mBreastPos, 1,
		                                            this);
		gpMarioParticleManager->emitAndBindToPosPtr(0x1B3, &mBreastPos, 1,
		                                            &unk1F8 + 1);
		gpMarioParticleManager->emitAndBindToPosPtr(0x1B4, &mBreastPos, 1,
		                                            this);
		gpMarioParticleManager->emitAndBindToPosPtr(0x1B4, &mBreastPos, 1,
		                                            &unk1F8 + 1);
	}

	if ((mSpine->getCurrentNerve() == &TNerveTinKoopaWait::theNerve() && mBreakStage > 1)
	    || mSpine->getCurrentNerve() == &TNerveTinKoopaDamage::theNerve()
	    || mSpine->getCurrentNerve() == &TNerveTinKoopaBreak::theNerve())
		gpMarioParticleManager->emitAndBindToPosPtr(0x1B5, &mRArmPos, 1, this);

	if ((mSpine->getCurrentNerve() == &TNerveTinKoopaWait::theNerve() && mBreakStage > 2)
	    || mSpine->getCurrentNerve() == &TNerveTinKoopaDamage::theNerve()
	    || mSpine->getCurrentNerve() == &TNerveTinKoopaBreak::theNerve())
		gpMarioParticleManager->emitAndBindToPosPtr(0x1B5, &mLArmPos, 1,
		                                            &unk1F8 + 1);

	if ((mSpine->getCurrentNerve() == &TNerveTinKoopaWait::theNerve() && mBreakStage > 0)
	    || (mSpine->getCurrentNerve() == &TNerveTinKoopaDamage::theNerve()
	        && mBreakStage > 0)
	    || (mSpine->getCurrentNerve() == &TNerveTinKoopaBreak::theNerve()
	        && (mBreakStage == 1 || mBreakStage == 2))) {
		mtx = getModel()->getAnmMtx(TTinKoopa_getJointIndex(1));
		gpMarioParticleManager->emitAndBindToMtxPtr(0x1B6, mtx, 1, this);
		gpMarioParticleManager->emitAndBindToMtxPtr(0x1B7, mtx, 1, this);
	}

	if ((mSpine->getCurrentNerve() == &TNerveTinKoopaWait::theNerve() && mBreakStage > 2)
	    || mSpine->getCurrentNerve() == &TNerveTinKoopaDamage::theNerve()
	    || mSpine->getCurrentNerve() == &TNerveTinKoopaBreak::theNerve())
		gpMarioParticleManager->emitAndBindToPosPtr(0x1B8, &mHeadPos, 1, this);

	if (mSpine->getCurrentNerve() == &TNerveTinKoopaDamage::theNerve()
	    || mSpine->getCurrentNerve() == &TNerveTinKoopaBreak::theNerve()) {
		mtx = getModel()->getAnmMtx(TTinKoopa_getJointIndex(10));
		gpMarioParticleManager->emitAndBindToMtxPtr(0x1BA, mtx, 1, this);
		gpMarioParticleManager->emitAndBindToMtxPtr(0x1B9, mtx, 1, this);
	}

	if (mSpine->getCurrentNerve() == &TNerveTinKoopaBreak::theNerve()) {
		if (mBreakStage == 0 || mBreakStage == 3) {
			if (getMActor()->getFrameCtrl(0)->checkPass(100.0f))
				gpCameraShake->startShake(CAM_SHAKE_MODE_UNK6, 1.0f);
		}

		if (mBreakStage == 1 || mBreakStage == 2) {
			if (getMActor()->getFrameCtrl(0)->checkPass(104.0f))
				gpCameraShake->startShake(CAM_SHAKE_MODE_UNK6, 1.0f);
		}

		if (mBreakStage == 0 || mBreakStage == 3) {
			if (getMActor()->getFrameCtrl(0)->checkPass(108.0f))
				gpMarioParticleManager->emitAndBindToMtxPtr(
				    0xF0, getModel()->getAnmMtx(TTinKoopa_getJointIndex(2)), 0,
				    this);
		}

		if (mBreakStage == 0) {
			if (getMActor()->getFrameCtrl(0)->checkPass(100.0f))
				gpMarioParticleManager->emitAndBindToMtxPtr(
				    0xF1, getModel()->getAnmMtx(TTinKoopa_getJointIndex(2)), 0,
				    this);
		}

		if (mBreakStage == 3) {
			if (getMActor()->getFrameCtrl(0)->checkPass(100.0f))
				gpMarioParticleManager->emitAndBindToMtxPtr(
				    0xF1, getModel()->getAnmMtx(TTinKoopa_getJointIndex(1)), 0,
				    this);
		}

		if (mBreakStage == 1) {
			if (getMActor()->getFrameCtrl(0)->checkPass(106.0f))
				gpMarioParticleManager->emitAndBindToMtxPtr(
				    0xF2, getModel()->getAnmMtx(TTinKoopa_getJointIndex(3)), 0,
				    this);
		}

		if (mBreakStage == 2) {
			if (getMActor()->getFrameCtrl(0)->checkPass(106.0f))
				gpMarioParticleManager->emitAndBindToMtxPtr(
				    0xF2, getModel()->getAnmMtx(TTinKoopa_getJointIndex(4)), 0,
				    this);
		}
	}

	if (mBreakingParts)
		mBreakingParts->emitPartsDisappearEffects();
}

TTinKoopaManager::TTinKoopaManager(const char* name)
    : TEnemyManager(name)
{
}

void TTinKoopaManager::createModelData()
{
	static const TModelDataLoadEntry entry[] = {
		{ "tinkoopa_body.bmd", 0x14240000, 0 },
		{ nullptr, 0, 0 },
	};
	createModelDataArray(entry);
}

void TTinKoopaManager::load(JSUMemoryInputStream& stream)
{
	TEnemyManager::load(stream);
	unk38 = new TTinKoopaParams("/enemy/tinkoopa.prm");
}

void TTinKoopaManager::loadAfter()
{
	static const char* onetimeFilenames[] = {
		"/scene/tinkoopa/jpa/ms_mkp_hibana_d1he.jpa",
		"/scene/tinkoopa/jpa/ms_mkp_killer.jpa",
		"/scene/tinkoopa/jpa/ms_mkp_smoke1.jpa",
		"/scene/tinkoopa/jpa/ms_mkp_parge_b14.jpa",
		"/scene/tinkoopa/jpa/ms_mkp_parge_b23.jpa",
		"/scene/tinkoopa/jpa/ms_mkp_flame_yuge.jpa",
		"/scene/tinkoopa/jpa/ms_mkp_kemu_parts.jpa",
	};
	for (int i = 0; i < 7; ++i)
		SMS_LoadParticle(onetimeFilenames[i], 0xEE + i);

	static const char* loopFilenames[] = {
		"/scene/tinkoopa/jpa/ms_mkp_hibana_w1br.jpa",
		"/scene/tinkoopa/jpa/ms_mkp_hibana_w3ar.jpa",
		"/scene/tinkoopa/jpa/ms_mkp_hibana_w4ar.jpa",
		"/scene/tinkoopa/jpa/ms_mkp_biri_w1st.jpa",
		"/scene/tinkoopa/jpa/ms_mkp_biri_w1ar.jpa",
		"/scene/tinkoopa/jpa/ms_mkp_biri_w1fe.jpa",
		"/scene/tinkoopa/jpa/ms_mkp_biri_w1he.jpa",
		"/scene/tinkoopa/jpa/ms_mkp_biri_d1br_a.jpa",
		"/scene/tinkoopa/jpa/ms_mkp_biri_d1br_b.jpa",
		"/scene/tinkoopa/jpa/ms_mkp_kemu_b1ar.jpa",
		"/scene/tinkoopa/jpa/ms_mkp_kemu_w2br_a.jpa",
		"/scene/tinkoopa/jpa/ms_mkp_kemu_w2br_b.jpa",
		"/scene/tinkoopa/jpa/ms_mkp_kemu_b1he.jpa",
		"/scene/tinkoopa/jpa/ms_mkp_kemu_b1fe_l.jpa",
		"/scene/tinkoopa/jpa/ms_mkp_kemu_b1fe_r.jpa",
		"/scene/tinkoopa/jpa/ms_mkp_fire_a.jpa",
		"/scene/tinkoopa/jpa/ms_mkp_fire_b.jpa",
	};
	for (int i = 0; i < 17; ++i)
		SMS_LoadParticle(loopFilenames[i], 0x1AC + i);

	static const char* loopIndirectFilenames[1] = {
		"/scene/tinkoopa/jpa/ms_mkp_fire_c.jpa",
	};
	for (int i = 0; i < 1; ++i)
		SMS_LoadParticle(loopIndirectFilenames[i], 0x1F2 + i);
}

TSpineEnemy* TTinKoopaManager::createEnemyInstance() { return nullptr; }

DEFINE_NERVE(TNerveTinKoopaWait, TLiveActor)
{
	TTinKoopa* koopa = (TTinKoopa*)spine->getBody();
	if (spine->getTime() == 0) {
		koopa->changeBck(TTinKoopa_getWaitAnimationIndex(koopa->mBreakStage));
		koopa->mDefeatTimer = koopa->getParams()->mSLDefeatWaitTime.get();
	}

	if (koopa->mBreakStage == 4 && koopa->mDefeatTimer <= 0)
		TFlagManager::getInstance()->setBool(true, 0x5000A);

	return false;
}

DEFINE_NERVE(TNerveTinKoopaDamage, TLiveActor)
{
	TTinKoopa* koopa = (TTinKoopa*)spine->getBody();
	if (spine->getTime() == 0) {
		koopa->changeBck(
		    TTinKoopa_getDamageAnimationIndex(koopa->mBreakStage));
		gpMarioParticleManager->emitAndBindToMtxPtr(
		    0xEE, koopa->getModel()->getAnmMtx(TTinKoopa_getJointIndex(0)), 0,
		    this);
		gpCameraShake->startShake(CAM_SHAKE_MODE_UNK6, 1.0f);
	}

	if (koopa->getMActor()->checkCurBckFromIndex(
	        TTinKoopa_getDamageAnimationIndex(koopa->mBreakStage))
	    && koopa->getMActor()->curAnmEndsNext(0, nullptr)) {
		koopa->changeBck(TTinKoopa_getWaitAnimationIndex(koopa->mBreakStage));
		return true;
	}

	return false;
}

DEFINE_NERVE(TNerveTinKoopaBreak, TLiveActor)
{
	TTinKoopa* koopa = (TTinKoopa*)spine->getBody();
	if (spine->getTime() == 0) {
		koopa->changeBck(TTinKoopa_getBreakAnimationIndex(koopa->mBreakStage));
		gpMarioParticleManager->emitAndBindToMtxPtr(
		    0xEE, koopa->getModel()->getAnmMtx(TTinKoopa_getJointIndex(0)), 0,
		    this);
		gpCameraShake->startShake(CAM_SHAKE_MODE_UNK6, 1.0f);
		if (koopa->mBreakStage == 3) {
			koopa->mKillerManager->unk60 = 1;
			MSBgm::stopTrackBGMs(7, 10);
		}
	}

	if (koopa->getMActor()->checkCurBckFromIndex(
	        TTinKoopa_getBreakAnimationIndex(koopa->mBreakStage))) {
		TTinKoopaPartsBase* parts
		    = koopa->mParts[TTinKoopa_getBreakingPartsIndex(koopa->mBreakStage)];
		if (koopa->getMActor()->curAnmEndsNext(0, nullptr)) {
			parts->mCollision->remove();
			++koopa->mBreakStage;
			koopa->makeHitCollision();
			koopa->changeBck(
			    TTinKoopa_getWaitAnimationIndex(koopa->mBreakStage));
			koopa->mPartsHP = koopa->getParams()->mSLPartsHP.get();
			return true;
		}

		if (!parts->mIsBreaking
		    && koopa->getMActor()->getFrameCtrl(0)->checkPass(
		        TTinKoopa_getPartsVisibleFrame(koopa->mBreakStage)))
			koopa->startBreakingParts();
	}

	return false;
}
