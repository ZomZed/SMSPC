#include <Enemy/Killer.hpp>
#include <Enemy/Conductor.hpp>
#include <Enemy/EffectObj.hpp>
#include <Enemy/Enemy.hpp>
#include <Enemy/SmallEnemy.hpp>
#include <Enemy/WalkerEnemy.hpp>
#include <Camera/CameraShake.hpp>
#include <M3DUtil/MActor.hpp>
#include <Map/Map.hpp>
#include <Map/MapData.hpp>
#include <Map/MapCollisionData.hpp>
#include <MarioUtil/MathUtil.hpp>
#include <MarioUtil/PacketUtil.hpp>
#include <MarioUtil/RandomUtil.hpp>
#include <MarioUtil/RumbleMgr.hpp>
#include <MoveBG/Item.hpp>
#include <MoveBG/ItemManager.hpp>
#include <MoveBG/MapObjBlock.hpp>
#include <MSound/MSound.hpp>
#include <MSound/MSoundSE.hpp>
#include <Player/MarioAccess.hpp>
#include <Strategic/LiveActor.hpp>
#include <Strategic/ObjModel.hpp>
#include <Strategic/Spine.hpp>
#include <System/EmitterViewObj.hpp>
#include <System/MarDirector.hpp>
#include <System/Particles.hpp>
#include <JSystem/JMath.hpp>
#include <JSystem/J3D/J3DGraphAnimator/J3DCluster.hpp>
#include <JSystem/J3D/J3DGraphAnimator/J3DJoint.hpp>
#include <JSystem/J3D/J3DGraphAnimator/J3DModel.hpp>
#include <JSystem/J3D/J3DGraphAnimator/J3DNode.hpp>
#include <JSystem/J3D/J3DGraphBase/J3DSys.hpp>
#include <JSystem/JUtility/JUTNameTab.hpp>
#include <dolphin/mtx.h>

// rogue includes needed for matching sinit & bss
#include <MSound/MSSetSound.hpp>
#include <MSound/MSoundBGM.hpp>
#include <M3DUtil/InfectiousStrings.hpp>

static TKiller* gpCurKiller;

u8 TKiller::mSerialBomb = 1;
// TODO: unused in the final game, value unknown
u8 TKiller::mSmokeJntNo = 1;
u8 TKiller::mTrampleDie = 1;
u8 TKiller::mRollSw;

f32 TFlyEnemy::mTestSp          = 2.5f;
int TFlyEnemy::mInvalidTime     = 200;
f32 TFlyEnemy::mTestMarioSpMax  = 12.0f;

static const char* killer_bastable[] = {
	"/scene/killer/bas/downkiller_down1.bas", nullptr, nullptr,
	"/scene/killer/bas/killer_search1.bas",   nullptr,
};

TFlyEnemyParams::TFlyEnemyParams(const char* path)
    : TWalkerEnemyParams(path)
    , PARAM_INIT(mSLNormalFlyGravityY, 0.2f)
    , PARAM_INIT(mSLNormalFlySpeed, 10.0f)
    , PARAM_INIT(mSLChaseFlyGravityY, 0.1f)
    , PARAM_INIT(mSLChaseDist, 2000.0f)
    , PARAM_INIT(mSLForceGravityY, 0.1f)
{
	TParams::load(mPrmPath);
}

void TFlyEnemy::init(TLiveManager* manager)
{
	TWalkerEnemy::init(manager);
	unk19C = (TFlyEnemyParams*)getSaveParam();
}

f32 TFlyEnemy::getGravityY() const
{
    // FAKEMATCH(frame): 8 bytes; real construct unknown
    volatile u32 pad[2]; (void)pad;

	if (mSpine->getCurrentNerve() == &TNerveFlyEnemyChaseFly::theNerve())
		return unk194;

	return unk19C->mSLNormalFlyGravityY.get();
}

void TFlyEnemy::reset()
{
	TWalkerEnemy::reset();
	unk1A0 = 0;
	unk198 = 1;
	unk1A4 = 0;
	unk194 = unk19C->mSLNormalFlyGravityY.get();
	unk1A5 = 0;
}

void TFlyEnemy::fly()
{
	JGeometry::TVec3<f32> pos = mPosition;
	pos += mPositionDelta;

	JGeometry::TVec3<f32> vel = mVelocity;
	vel.x += SMS_GetMarioSpeedX() / mTestMarioSpMax;
	vel.z += SMS_GetMarioSpeedZ() / mTestMarioSpMax;
	pos += vel;
	pos.y += unk194;

	f32 y         = pos.y;
	mGroundHeight = gpMap->checkGround(pos.x, y + mHeadHeight, pos.z,
	                                   &mGroundPlane);
	mGroundHeight += 1.0f;

	if (y <= mGroundHeight) {
		if (unk1A0 > mInvalidTime) {
			offLiveFlag(LIVE_FLAG_AIRBORNE);
			mVelocity.set(0.0f, 0.0f, 0.0f);
			pos.y = mGroundHeight;
		}

		const TLiveActor* actor = mGroundPlane->getActor();
		if (actor && actor->isActorType(ACTOR_TYPE_BASKET_REVERSE))
			const_cast<TLiveActor*>(actor)->kill();

		if (mGroundPlane->checkFlag(BG_CHECK_FLAG_ILLEGAL))
			kill();
	} else {
		onLiveFlag(LIVE_FLAG_AIRBORNE);
	}

	mPositionDelta = pos - mPosition;
}

void TFlyEnemy::calcChaseParam()
{
	JGeometry::TVec3<f32> diff(SMS_GetMarioPos().x - mPosition.x,
	                           SMS_GetMarioPos().y - mPosition.y,
	                           SMS_GetMarioPos().z - mPosition.z);
	diff.x *= 1.1f;
	diff.z *= 1.1f;

	JGeometry::TVec3<f32> goal;
	goal.add(mPosition, diff);
	if (unk1A5)
		setGoalPath(TPathNode(goal));

	if (diff.y > 100.0f || fabsf(diff.y) < 100.0f) {
		if (unk1A5) {
			if (unk198 != 2 && mSprayedByWaterCooldown == 0)
				unk194 = unk19C->mSLChaseFlyGravityY.get();
			unk198 = 2;
		} else {
			mPosition.y -= 1.0f;
		}
	} else {
		unk194 = unk19C->mSLNormalFlyGravityY.get();

		JGeometry::TVec3<f32> vel(0.0f, 0.0f, 0.0f);
		if (unk198 != 2 || diff.y > 150.0f) {
			unk198 = 0;
			MsVECNormalize(&diff, &diff);
			TFlyEnemyParams* params = unk19C;
			f32 speed               = params->mSLNormalFlySpeed.get();
			vel.x                   = diff.x * speed;
			vel.z                   = diff.z * speed;
			unk194                  = params->mSLForceGravityY.get();
		} else {
			mPosition.y -= 3.0f;
		}

		// TODO: the horizontal velocity computed above is thrown away here.
		// This is what the original code does.
		vel.x = vel.z = 0.0f;
		mVelocity = vel;
	}
}

void TFlyEnemy::bind()
{
	if (mSpine->getCurrentNerve() == &TNerveFlyEnemyChaseFly::theNerve()
	    || unk1A0 < mInvalidTime) {
		fly();
		return;
	}

	TLiveActor::bind();
}

// fabricated
// TODO: the target keeps MsSin, MsCos and TVec3::set out of line here, which
// needs this helper to sit one inline level below polarXZ. The original
// spelling is unknown, and the return copy below is not in the target.
static inline void setPolarXZ(JGeometry::TVec3<f32>& result, f32 theta,
                              f32 radius)
{
	result.set(radius * MsSin(theta), 0.0f, radius * MsCos(theta));
}

// fabricated
static inline JGeometry::TVec3<f32> polarXZ(f32 theta, f32 radius)
{
	JGeometry::TVec3<f32> result;
	setPolarXZ(result, theta, radius);
	return result;
}

// TODO: the map lists flyMove as UNUSED (emitted, then dead-stripped), yet the
// target expands it inside TNerveFlyEnemyChaseFly::execute. Our compiler setup
// only does that with an explicit inline, which in turn drops the out-of-line
// copy (the same happens to other UNUSED inlines, e.g. in coasterkiller.cpp).
inline void TFlyEnemy::flyMove()
{
	JGeometry::TVec3<f32> vel = mVelocity;
	vel.scale(0.9f);
	mVelocity = vel;

	JGeometry::TVec3<f32> diff = getUnkF4().getPoint();
	diff -= mPosition;
	VECMag(&diff);

	f32 targetAngle = MsAngleWrap(MsGetRotFromZaxisY(diff));
	f32 angleDiff   = MsAngleDiff(targetAngle, mRotation.y);

	f32 step;
	if (angleDiff > 0.0f)
		step = MsMin(angleDiff, mTurnSpeed);
	else
		step = MsMax(angleDiff, -mTurnSpeed);

	mRotation.y = MsAngleWrap(mRotation.y + step);

	JGeometry::TVec3<f32> delta = mPositionDelta;
	delta += polarXZ(mRotation.y, mMarchSpeed);
	mPositionDelta = delta;
}

DEFINE_NERVE(TNerveFlyEnemyNormalFly, TLiveActor)
{
	TFlyEnemy* self = (TFlyEnemy*)spine->getBody();

	if (spine->getTime() == 0)
		self->setNormalFlyAnm();

	if (self->unk1A4 && self->unk1A0 > 500) {
		self->updateSquareToMario();
		f32 chaseDist = self->unk19C->mSLChaseDist.get();
		if (self->getDistToMarioSquared() < chaseDist * chaseDist) {
			spine->pushAfterCurrent(&TNerveFlyEnemyChaseFly::theNerve());
			return true;
		}
	} else if (!self->unk1A6 && self->isFindMario(1.0f)
	           && self->unk1A0 > 100) {
		self->updateSquareToMario();
		f32 chaseDist = self->unk19C->mSLChaseDist.get();
		if (self->getDistToMarioSquared() < chaseDist * chaseDist) {
			spine->pushAfterCurrent(&TNerveFlyEnemyChaseFly::theNerve());
			return true;
		}
	}

	JGeometry::TVec3<f32> vel = self->mVelocity;
	self->mRotation.x         = MsGetRotFromZaxis(vel).x;

	f32 scale = MsClamp(1.05f * self->mScaling.x, 0.0f, self->getBodyScale());
	self->mScaling.x = self->mScaling.y = self->mScaling.z = scale;
	return false;
}

DEFINE_NERVE(TNerveFlyEnemyChaseFly, TLiveActor)
{
	TFlyEnemy* self = (TFlyEnemy*)spine->getBody();

	if (spine->getTime() == 0) {
		self->unk1A8 = self->mVelocity;
		self->calcChaseParam();
		self->setChaseFlyAnm();
	}

	f32 rate = 1.0f;
	if (!self->unk1A5) {
		f32 speedSq    = self->getMarchSpeed() * self->getMarchSpeed();
		self->unk1A8.y = 0.1f;
		rate           = self->unk1A8.length() / speedSq * TFlyEnemy::mTestSp;
	} else if (self->unk198 == 0) {
		rate = 2.0f;
	}

	self->walkBehavior(2, rate);

	if ((self->unk1A5 && self->isReachedToGoalXZ())
	    || (self->unk198 == 0
	        && self->mPosition.y < 100.0f + self->getGroundHeight()))
		self->calcChaseParam();

	switch (self->unk198) {
	case 0:
	case 1:
		self->walkBehavior(3, 1.0f);
		break;
	case 2:
		self->flyMove();
		break;
	}

	if (self->unk198 != 2
	    && self->mPosition.y > 200.0f + self->getGroundHeight()) {
		JGeometry::TVec3<f32> dir(self->getMarchSpeed(),
		                          -40.0f * self->getGravityY(),
		                          self->getMarchSpeed());
		self->mRotation.x = MsGetRotFromZaxis(dir).x;

		JGeometry::TVec3<f32> delta = self->mPositionDelta;
		delta.y                     = self->getGravityY();
		self->mRotation.x           = MsGetRotFromZaxis(delta).x;
	} else {
		self->mRotation.x *= 0.99f;
	}

	self->flyBehavior();

	f32 scale = MsClamp(1.1f * self->mScaling.x, 0.0f, self->getBodyScale());
	self->mScaling.x = self->mScaling.y = self->mScaling.z = scale;
	return false;
}

TKillerSaveLoadParams::TKillerSaveLoadParams(const char* path)
    : TFlyEnemyParams(path)
    , PARAM_INIT(mSLWaterAddGravityY, 1.0f)
    , PARAM_INIT(mSLChaseTimer, 1000)
    , PARAM_INIT(mSLBombRange, 300.0f)
{
	TParams::load(mPrmPath);
}

TKillerManager::TKillerManager(const char* name)
    : TSmallEnemyManager(name)
{
	gpCurKiller = nullptr;
}

void TKillerManager::load(JSUMemoryInputStream& stream)
{
	TSmallEnemyManager::load(stream);
	unk38 = new TKillerSaveLoadParams("/enemy/killer.prm");
}

void TKillerManager::createModelData()
{
	static TModelDataLoadEntry entry[] = {
		{ "killer_model1.bmd", 0x10220000, 0 },
		{ "downkiller_model1.bmd", 0x10220000, 0 },
		{ nullptr, 0, 0 },
	};

	createModelDataArray(entry);
}

TSpineEnemy* TKillerManager::createEnemyInstance() { return new TKiller; }

static BOOL KillerBodyCallback(J3DNode* node, BOOL param_2)
{
	if (param_2 == 0) {
		if (gpCurKiller == nullptr || !TKiller::mRollSw
		    || !gpCurKiller->isRollFly())
			return TRUE;

		MtxPtr mtx = gpCurKiller->getModel()->getAnmMtx(
		    ((J3DJoint*)node)->getJntNo());

		TPosition3f scale;
		scale.setTrans(0.0f, 0.0f, 0.0f);
		f32 bodyScale = gpCurKiller->getBodyScale();
		scale.setScale(bodyScale, bodyScale, bodyScale);

		Mtx local;
		MtxPtr rot = local;
		MsMtxSetRotZ(rot, gpCurKiller->unk1B8);

		MTXConcat(mtx, rot, mtx);
		MTXConcat(mtx, scale, mtx);
		MTXConcat(J3DSys::mCurrentMtx, rot, J3DSys::mCurrentMtx);
		MTXConcat(J3DSys::mCurrentMtx, scale, J3DSys::mCurrentMtx);
	}
	return TRUE;
}

TKiller::TKiller(const char* name)
    : TFlyEnemy(name)
{
}

void TKiller::init(TLiveManager* manager)
{
	TFlyEnemy::init(manager);
	mActorType = ACTOR_TYPE_ENEMY_UNK1F;
	unk150     = 0x11;
	unk1B4     = (TKillerSaveLoadParams*)getSaveParam();
	mSpine->initWith(&TNerveFlyEnemyNormalFly::theNerve());
	onLiveFlag(LIVE_FLAG_FORCE_SHADOW);
	offLiveFlag(LIVE_FLAG_UNK800);
	onHitFilter(0x40000000);

	J3DModel* model = mMActor->getModel();
	if (!model->getSkinDeform())
		model->setSkinDeform(new J3DSkinDeform, J3D_DEFORM_ATTACH_FLAG_UNK_1);
	mMActor->resetDL();

	if (mInstanceIndex == 0) {
		// TODO: what this loop did is unknown
		for (u8 i = 0; i < getModel()->getModelData()->getJointNum(); ++i) { }
	}

	mMActor->setJointCallback(1, KillerBodyCallback);
	unk188 = 0.0f;
}

void TKiller::setMActorAndKeeper()
{
	mMActorKeeper = new TMActorKeeper(mManager, 2);
	mMActor       = mMActorKeeper->createMActor("killer_model1.bmd", 3);
	mMActorKeeper->createMActor("downkiller_model1.bmd", 3);

	s32 noseMatIdx = getActorKeeper()
	                     ->getMActor("killer_model1.bmd")
	                     ->getModel()
	                     ->getModelData()
	                     ->getMaterialName()
	                     ->getIndex("_nosemat1");
	s32 eyesMatIdx = getActorKeeper()
	                     ->getMActor("killer_model1.bmd")
	                     ->getModel()
	                     ->getModelData()
	                     ->getMaterialName()
	                     ->getIndex("_eyesmat1");
	s32 bodyMatIdx = getActorKeeper()
	                     ->getMActor("killer_model1.bmd")
	                     ->getModel()
	                     ->getModelData()
	                     ->getMaterialName()
	                     ->getIndex("_body1");

	SMS_InitPacket_OneTevColor(getMActor()->getModel(), noseMatIdx, GX_TEVREG0,
	                           &mNoseColor);
	SMS_InitPacket_OneTevColor(getMActor()->getModel(), eyesMatIdx, GX_TEVREG0,
	                           &mEyesColor);
	SMS_InitPacket_OneTevColor(getMActor()->getModel(), bodyMatIdx, GX_TEVREG0,
	                           &mBodyColor);
	SMS_InitPacket_OneTevColor(
	    getActorKeeper()->getMActor("downkiller_model1.bmd")->getModel(),
	    bodyMatIdx, GX_TEVREG0, &mBaseColor);
}

void TKiller::behaveToWater(THitActor* param_1)
{
	if (mSpine->getCurrentNerve() == &TNerveFlyEnemyNormalFly::theNerve()
	    || mSpine->getCurrentNerve() == &TNerveFlyEnemyChaseFly::theNerve())
		genEventCoin();

	if (mSpine->getCurrentNerve() != &TNerveKillerExplosion::theNerve()) {
		mSpine->pushNerve(&TNerveKillerExplosion::theNerve());
		onHitFilter(HIT_FILTER_NO_COLLISION);
		setVelocity(JGeometry::TVec3<f32>(0.0f, 0.0f, 0.0f));
	}
}

void TKiller::genEventCoin()
{
	int num = 2;
	if (unk1A6)
		num = 8;

	for (int i = 0; i < num; ++i) {
		Vec offset;
		offset.x = 0.0f;
		offset.y = 0.0f;
		offset.z = 30.0f;
		Mtx m;
		MtxPtr mtx = m;
		MsMtxSetRotY(mtx, 360.0f * (1.0f / num) * (i + 1));
		MTXMultVec(mtx, &offset, &offset);

		TCoin* coin = (TCoin*)gpItemManager->makeObjAppear(
		    mPosition.x + offset.x, mPosition.y, mPosition.z + offset.z,
		    ACTOR_TYPE_COIN, true);
		if (coin) {
			coin->mPosition.y = mPosition.y;
			MsVECNormalize(&offset, &offset);
			coin->mVelocity.set(3.0f * offset.x, 20.0f, 3.0f * offset.z);
			coin->offLiveFlag(LIVE_FLAG_UNK10);
		}
	}
}

bool TKiller::isHitValid(u32 message)
{
	if (message == HIT_MESSAGE_UNKB) {
		onLiveFlag(LIVE_FLAG_DEAD);
		onHitFilter(HIT_FILTER_NO_COLLISION);
		genEventCoin();
		return false;
	}

	if (mTrampleDie)
		unk194 -= 12.0f;

	return false;
}

void TKiller::setDeadAnm()
{
	mMActor = getActorKeeper()->getMActor("downkiller_model1.bmd");
	setBckAnm(0);
	TEffectExplosion* effect
	    = (TEffectExplosion*)gpConductor->makeOneEnemyAppear(
	        mPosition, "エフェクト爆発マネージャー", 1);
	if (effect)
		effect->generate(mPosition, mScaling);
	gpCameraShake->startShake(CAM_SHAKE_MODE_UNK6, 1.0f);
	SMSRumbleMgr->start(0x15, 5, (f32*)nullptr);
}

void TKiller::attackToMario()
{
	if (SMS_GetMarioPos().y < mPosition.y) {
		if (mSpine->getCurrentNerve() != &TNerveKillerExplosion::theNerve()) {
			mSpine->pushNerve(&TNerveKillerExplosion::theNerve());
			sendAttackMsgToMario();
			return;
		}

		SMS_SendMessageToMario(this, HIT_MESSAGE_UNKA);
	}
}

const char** TKiller::getBasNameTable() const { return killer_bastable; }

void TKiller::setNormalFlyAnm()
{
	mMActor = getActorKeeper()->getMActor("killer_model1.bmd");
	setBckAnm(2);
	unk1B8 = 0.0f;
	unk1A0 = 0;
}

void TKiller::setChaseFlyAnm() { setBckAnm(3); }

bool TKiller::isRollFly()
{
	if (mSpine->getCurrentNerve() == &TNerveFlyEnemyChaseFly::theNerve()
	    && isBckAnm(1))
		return true;

	return false;
}

bool TKiller::isCollidMove(THitActor* param_1)
{
	if (param_1->isActorType(ACTOR_TYPE_BASKET_REVERSE))
		((TLiveActor*)param_1)->kill();

	if (mSerialBomb
	    && mSpine->getCurrentNerve() == &TNerveFlyEnemyChaseFly::theNerve())
		mSpine->pushNerve(&TNerveKillerExplosion::theNerve());

	return true;
}

void TKiller::flyBehavior()
{
    // FAKEMATCH(frame): 8 bytes; real construct unknown
    volatile u32 pad[2]; (void)pad;

	mTurnSpeed = unk1B4->mSLTurnSpeedLow.get();
	if (mSpine->getTime() > unk1B4->mSLChaseTimer.get())
		unk194 -= unk1B4->mSLWaterAddGravityY.get();

	if (checkCurAnmEnd(0) && isBckAnm(3))
		setBckAnm(1);

	unk1B8 += 2.5f;
}

void TKiller::changeOut()
{
    // FAKEMATCH(frame): 8 bytes; real construct unknown
    volatile u32 pad[2]; (void)pad;

	SMSGetMSound()->startSoundActor(MSD_SE_EN_TELSA_RECOVER, &mPosition, 0,
	                                nullptr, 0, 4);

	onLiveFlag(LIVE_FLAG_DEAD);
	genEventCoin();
	onHitFilter(HIT_FILTER_NO_COLLISION);
	mPosition = mJuiceBlock->mPosition;

	gpMarioParticleManager->emitAndBindToPosPtr(PARTICLE_MS_TLS_CHANGE,
	                                            &mPosition, 0, nullptr);
	getMActor()->setFrameRate(SMSGetAnmFrameRate(), ANM_TYPE_BCK);
	mJuiceBlock->kill();
	mJuiceBlock = nullptr;
}

void TKiller::reset()
{
	gpCurKiller = this;
	TFlyEnemy::reset();

	TMsRange<f32> range(0.0f, 1.0f);
	mBaseColor.r = mBaseColor.g = mBaseColor.b = 0;
	mBodyColor.r = mBodyColor.g = mBodyColor.b = 0;
	unk1A6                                     = 0;
	if (range.rand() < 0.05f) {
		unk1A6       = 1;
		mBodyColor.r = 200;
		mBodyColor.g = 185;
		mBodyColor.b = 0;
		mBaseColor.r = 255;
		mBaseColor.g = 225;
		mBaseColor.b = 70;
	}
}

void TKiller::setColorType()
{
	if (unk1A6)
		unk1A5 = 0;

	if (unk1A5) {
		mBodyColor.r = 70;
		mBodyColor.g = 20;
		mBodyColor.b = 70;
		mBaseColor.r = 70;
		mBaseColor.g = 20;
		mBaseColor.b = 70;
	}
}

void TKiller::bind()
{
	TFlyEnemy::bind();
	++unk1A0;

	if (mSpine->getCurrentNerve() != &TNerveKillerExplosion::theNerve()) {
		if (!isAirborne() && unk1A0 > mInvalidTime) {
			mSpine->pushNerve(&TNerveKillerExplosion::theNerve());
		} else if (unk1A0 > mInvalidTime) {
			TBGWallCheckRecord record(mPosition.x, mPosition.y + mHeadHeight,
			                          mPosition.z, 2.0f * mBodyRadius, 1, 0);
			if (gpMap->isTouchedWallsAndMoveXZ(&record)) {
				const TLiveActor* actor = record.mResultWalls[0]->getActor();
				if (actor && actor->isActorType(ACTOR_TYPE_BASKET_REVERSE))
					const_cast<TLiveActor*>(actor)->kill();
				mSpine->pushNerve(&TNerveKillerExplosion::theNerve());
			}
		}
	}

	if (isAirborne()) {
		SMSGetMSound()->startSoundActor(MSD_SE_EN_KILLER_FLY, &mPosition, 0,
		                                nullptr, 0, 4);
		mRotation.x = MsClamp(mRotation.x, -25.0f, 90.0f);
		MsMtxSetXYZRPH(unk1BC, mPosition.x, mPosition.y, mPosition.z,
		               mRotation.x, mRotation.y, mRotation.z);
		gpMarioParticleManager->emitAndBindToMtxPtr(PARTICLE_MS_KIL_SMOKE,
		                                            unk1BC, 1, this);
	}
}

void TKiller::calcRootMatrix()
{
    // FAKEMATCH(frame): 32 bytes; real construct unknown
    volatile u32 pad[8]; (void)pad;

	if (gpMarDirector->checkFlag(0xF)) {
		onLiveFlag(LIVE_FLAG_DEAD);
		onHitFilter(HIT_FILTER_NO_COLLISION);
	}

	if (isBckAnm(2)) {
		if (unk1A6) {
			mNoseColor.r = mEyesColor.r = 170;
			mNoseColor.g = mEyesColor.g = 140;
			mNoseColor.b = mEyesColor.b = 0;
		} else {
			mNoseColor.r = mNoseColor.g = mNoseColor.b = mEyesColor.r
			    = mEyesColor.g = mEyesColor.b              = 0;
		}
	}

	if (isBckAnm(3)) {
		mEyesColor.r = mNoseColor.g = mNoseColor.b = 0;
		if (mSpine->getTime() % 10 < 5)
			mEyesColor.g = mEyesColor.b = mNoseColor.r = 0;

		if (unk1A6) {
			if (mSpine->getTime() % 10 < 5) {
				mNoseColor.r = 170;
				mNoseColor.g = 140;
				mNoseColor.b = 0;
			} else {
				mNoseColor.r = 180;
				mNoseColor.g = 140;
				mNoseColor.b = 150;
			}
		}

		if (unk1A5) {
			if (mSpine->getTime() % 10 < 5) {
				mNoseColor.r = mEyesColor.r = 200;
				mNoseColor.g                = 0;
				mNoseColor.b                = 0;
			} else {
				mNoseColor.r = 70;
				mNoseColor.g = 20;
				mNoseColor.b = 70;
			}
		}
	}

	if (isBckAnm(1)) {
		mEyesColor.g = mEyesColor.b = mEyesColor.r = 0;
		mNoseColor.g = mNoseColor.b = 0;
		f32 t = fabsf(MsSin(360.0f * mSpine->getTime() / 120.0f));
		if (unk1A6) {
			mBodyColor.r = 170;
			mBodyColor.g = 140;
			mBodyColor.b = 0;
			mNoseColor.r = (u8)(10.0f * t + 160.0f);
			mNoseColor.g = (u8)(30.0f * t + 140.0f);
			mNoseColor.b = (u8)(150.0f * t);
		}

		if (unk1A5) {
			mBodyColor.r = 70;
			mBodyColor.g = 20;
			mBodyColor.b = 70;
			mBaseColor.r = 70;
			mBaseColor.g = 20;
			mBaseColor.b = 70;
			mNoseColor.r = (u8)(130.0f * t + 70.0f);
			mNoseColor.g = (u8)(20.0f - 20.0f * t);
			mNoseColor.b = (u8)(70.0f - 70.0f * t);
		}
	}

	gpCurKiller = this;
	mRotation.x = MsClamp(mRotation.x, -25.0f, 90.0f);
	TSpineEnemy::calcRootMatrix();
}

bool TKiller::isFindMario(f32 param_1)
{
	TSmallEnemyParams* params = (TSmallEnemyParams*)getSaveParam();

	if (fabsf(SMS_GetMarioPos().y - mPosition.y)
	    < params->mSLSearchHeight.get()) {
		JGeometry::TVec3<f32> marioPos(SMS_GetMarioPos().x,
		                               SMS_GetMarioPos().y,
		                               SMS_GetMarioPos().z);
		f32 searchLength = params->mSLSearchLength.get();
		f32 searchAngle  = params->mSLSearchAngle.get();
		f32 searchAware  = params->mSLSearchAware.get();
		if (isInSight(marioPos, searchLength * param_1, searchAngle * param_1,
		              searchAware * param_1))
			return true;
		return false;
	}

	return false;
}

DEFINE_NERVE(TNerveKillerExplosion, TLiveActor)
{
	TKiller* self = (TKiller*)spine->getBody();

	if (spine->getTime() == 0) {
		self->unk20C = self->getKillerParams()->mSLBombRange.get()
		               * self->getBodyScale() / self->getAttackRadius();
		self->mRotation.x = 0.0f;
		self->setDeadAnm();
		if (!self->isAirborne()) {
			if (self->getGroundPlane()->isWaterSurface()) {
				TEffectBombColumWater* effect
				    = (TEffectBombColumWater*)gpConductor->makeOneEnemyAppear(
				        self->mPosition, "エフェクト爆発水柱マネージャー", 1);
				if (effect) {
					JGeometry::TVec3<f32> scale(2.0f, 2.0f, 2.0f);
					effect->generate(self->mPosition, scale);
				}
			}
			if (self->getGroundPlane()->isSand()) {
				TEffectColumSand* effect
				    = (TEffectColumSand*)gpConductor->makeOneEnemyAppear(
				        self->mPosition, "エフェクト砂柱マネージャー", 1);
				if (effect) {
					JGeometry::TVec3<f32> scale(0.6f, 0.9f, 0.6f);
					effect->generate(self->mPosition, scale);
				}
			}
		}
		SMSRumbleMgr->start(0x13, &self->mPosition);
	}

	if (self->unk190 < self->unk20C) {
		self->unk190 *= 1.3f;
	} else {
		self->onHitFilter(HIT_FILTER_NO_COLLISION);
		if (self->checkCurAnmEnd(0)) {
			self->onLiveFlag(LIVE_FLAG_DEAD);
			self->onLiveFlag(LIVE_FLAG_UNK8);
			self->offLiveFlag(TSmallEnemy::LIVE_FLAG_MELT_ON_DEATH);
			self->mHolder = nullptr;
			self->stopAnmSound();
			spine->reset();
			spine->setNext(&TNerveSmallEnemyDie::theNerve());
			spine->pushAfterCurrent(spine->getDefault());
			self->mPosition.y -= 200.0f;
			return true;
		}
	}

	self->expandCollision();
	return false;
}
